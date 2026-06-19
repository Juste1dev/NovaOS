
#include "vfs.h"
#include "../libc.h"
#include "../kernel/memory.h"
#include "../kernel/timer.h"
#include "../drivers/sound.h"
#include "../net/webcache.h"
#include <stdint.h>
#include <stddef.h>

#define FAT_BYTES_PER_SECTOR      512u
#define FAT_SECTORS_PER_CLUSTER   1u
#define FAT_RESERVED_SECTORS      32u
#define FAT_NUM_FATS              2u
#define FAT_SECTORS_PER_FAT       128u
#define FAT_TOTAL_SECTORS         8192u
#define FAT_ROOT_CLUSTER          2u
#define FAT_MEDIA_TYPE            0xF8u
#define FAT_ATTR_DIRECTORY        0x10u
#define FAT_ATTR_ARCHIVE          0x20u
#define FAT_ATTR_LFN              0x0Fu
#define FAT_FREE_CLUSTER          0x00000000u
#define FAT_EOC                   0x0FFFFFF8u
#define FAT_BAD_CLUSTER           0x0FFFFFF7u
#define FAT_LAST_LONG_PARTS       20
#define FAT_CACHE_FILE_READ_MAX   (VFS_MAX_DATA - 1)

typedef struct __attribute__((packed)) {
    uint8_t  jmp_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
    uint8_t  boot_code[420];
    uint16_t signature;
} fat32_bpb_t;

typedef struct __attribute__((packed)) {
    uint32_t lead_sig;
    uint8_t  reserved1[480];
    uint32_t struct_sig;
    uint32_t free_count;
    uint32_t next_free;
    uint8_t  reserved2[12];
    uint32_t trail_sig;
} fat32_fsinfo_t;

typedef struct __attribute__((packed)) {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  ntres;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} fat32_dirent_t;

typedef struct __attribute__((packed)) {
    uint8_t  ord;
    uint16_t name1[5];
    uint8_t  attr;
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t first_cluster_low;
    uint16_t name3[2];
} fat32_lfn_t;

typedef struct {
    int      valid;
    int      node_idx;
    uint32_t offset;
    int      flags;
} fat_fd_t;

typedef struct {
    int      found;
    int      is_dir;
    uint32_t first_slot_cluster;
    int      first_slot_index;
    uint32_t short_slot_cluster;
    int      short_slot_index;
    uint32_t first_cluster;
    uint32_t size;
    fat32_dirent_t dirent;
    char     name[VFS_MAX_NAME];
} fat_lookup_t;

vfs_node_t vfs_nodes[VFS_MAX_FILES];
int        vfs_node_count = 0;

static fat_fd_t fds[VFS_MAX_FD];
static uint8_t *fat_disk = NULL;
static uint32_t fat_first_data_sector = 0;
static uint32_t fat_cluster_count = 0;
static uint32_t fat_total_bytes = 0;

#define ATA_PRIMARY_IO            0x1F0u
#define ATA_PRIMARY_CTRL          0x3F6u
#define ATA_SECONDARY_IO          0x170u
#define ATA_SECONDARY_CTRL        0x376u
#define ATA_SECTOR_WORDS          256u
#define FAT_PERSIST_TIMEOUT_MS    1500u
#define FAT_DIRTY_BITMAP_BYTES    ((FAT_TOTAL_SECTORS + 7u) / 8u)

typedef struct {
    uint16_t io;
    uint16_t ctrl;
    uint8_t  drive;
    int      present;
    char     label[32];
} fat_ata_device_t;

static fat_ata_device_t fat_persist_dev = {0};
static int fat_autosave_enabled = 1;
static int fat_loaded_from_persistent_disk = 0;
static uint8_t fat_dirty_bitmap[FAT_DIRTY_BITMAP_BYTES];
static int fat_has_dirty_data = 0;
static void fat_dirty_reset(void);
static uint8_t *fat_sector_ptr(uint32_t sector);

static inline void fat_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port));
}

static inline uint8_t fat_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0":"=a"(value):"Nd"(port));
    return value;
}

static inline void fat_outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0,%1"::"a"(value),"Nd"(port));
}

static inline uint16_t fat_inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1,%0":"=a"(value):"Nd"(port));
    return value;
}

static void fat_io_wait(void) { fat_outb(0x80, 0); }

static void fat_make_label(char *out, int max, const char *prefix, int channel, int drive) {
    int pos = 0;
    if (!out || max <= 0) return;
    while (prefix && *prefix && pos < max - 1) out[pos++] = *prefix++;
    if (pos < max - 1) out[pos++] = '0' + channel;
    if (pos < max - 1) out[pos++] = ':';
    if (pos < max - 1) out[pos++] = drive ? '1' : '0';
    out[pos] = 0;
}

static int fat_ata_poll(uint16_t io, uint8_t mask, uint8_t value, uint32_t timeout_ms) {
    uint32_t start = timer_ms();
    while (timer_ms() - start < timeout_ms) {
        uint8_t status = fat_inb(io + 7);
        if (status != 0x00u && status != 0xFFu && (status & mask) == value) return 1;
    }
    return 0;
}

static int fat_ata_wait_ready(uint16_t io, uint32_t timeout_ms) {
    uint32_t start = timer_ms();
    while (timer_ms() - start < timeout_ms) {
        uint8_t status = fat_inb(io + 7);
        if (status == 0x00u || status == 0xFFu) return 0;
        if (!(status & 0x80u)) {
            if (status & 0x01u) return 0;
            return 1;
        }
    }
    return 0;
}

static int fat_ata_identify(uint16_t io, uint16_t ctrl, uint8_t drive) {
    (void)ctrl;
    fat_outb(io + 6, (uint8_t)(0xA0u | (drive << 4)));
    fat_io_wait();
    fat_outb(io + 2, 0);
    fat_outb(io + 3, 0);
    fat_outb(io + 4, 0);
    fat_outb(io + 5, 0);
    fat_outb(io + 7, 0xECu);

    {
        uint8_t status = fat_inb(io + 7);
        if (status == 0x00u || status == 0xFFu) return 0;
    }

    if (!fat_ata_poll(io, 0x80u, 0x00u, FAT_PERSIST_TIMEOUT_MS)) return 0;
    {
        uint8_t cl = fat_inb(io + 4);
        uint8_t ch = fat_inb(io + 5);
        if (cl != 0 || ch != 0) return 0;
    }
    if (!fat_ata_poll(io, 0x08u, 0x08u, FAT_PERSIST_TIMEOUT_MS)) return 0;
    for (uint32_t i = 0; i < ATA_SECTOR_WORDS; i++) (void)fat_inw(io + 0);
    return 1;
}

static int fat_ata_rw_sector(const fat_ata_device_t *dev, uint32_t lba, uint8_t *buffer, int is_write) {
    if (!dev || !dev->present || !buffer) return 0;
    if (!fat_ata_wait_ready(dev->io, FAT_PERSIST_TIMEOUT_MS)) return 0;

    fat_outb(dev->ctrl, 0x00);
    fat_outb(dev->io + 6, (uint8_t)(0xE0u | (dev->drive << 4) | ((lba >> 24) & 0x0Fu)));
    fat_outb(dev->io + 1, 0x00);
    fat_outb(dev->io + 2, 0x01);
    fat_outb(dev->io + 3, (uint8_t)(lba & 0xFFu));
    fat_outb(dev->io + 4, (uint8_t)((lba >> 8) & 0xFFu));
    fat_outb(dev->io + 5, (uint8_t)((lba >> 16) & 0xFFu));
    fat_outb(dev->io + 7, (uint8_t)(is_write ? 0x30u : 0x20u));

    if (!fat_ata_poll(dev->io, 0x88u, 0x08u, FAT_PERSIST_TIMEOUT_MS)) return 0;

    if (is_write) {
        for (uint32_t i = 0; i < ATA_SECTOR_WORDS; i++) {
            uint16_t word = (uint16_t)buffer[i * 2] | ((uint16_t)buffer[i * 2 + 1] << 8);
            fat_outw(dev->io + 0, word);
        }
        fat_outb(dev->io + 7, 0xE7u);
        if (!fat_ata_wait_ready(dev->io, FAT_PERSIST_TIMEOUT_MS)) return 0;
    } else {
        for (uint32_t i = 0; i < ATA_SECTOR_WORDS; i++) {
            uint16_t word = fat_inw(dev->io + 0);
            buffer[i * 2] = (uint8_t)(word & 0xFFu);
            buffer[i * 2 + 1] = (uint8_t)((word >> 8) & 0xFFu);
        }
    }
    return 1;
}

static int fat_probe_persistent_disk(void) {
    struct probe_item { uint16_t io; uint16_t ctrl; uint8_t drive; int channel; };
    static const struct probe_item probes[] = {
        { ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   1, 0 },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0, 1 },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 1, 1 }
    };

    fat_persist_dev.present = 0;
    for (int i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
        if (fat_ata_identify(probes[i].io, probes[i].ctrl, probes[i].drive)) {
            fat_persist_dev.io = probes[i].io;
            fat_persist_dev.ctrl = probes[i].ctrl;
            fat_persist_dev.drive = probes[i].drive;
            fat_persist_dev.present = 1;
            fat_make_label(fat_persist_dev.label, sizeof(fat_persist_dev.label), "ata", probes[i].channel, probes[i].drive);
            return 1;
        }
    }
    return 0;
}

static int fat_validate_loaded_volume(void) {
    fat32_bpb_t *bpb = (fat32_bpb_t *)fat_sector_ptr(0);
    if (!fat_disk) return 0;
    if (bpb->signature != 0xAA55u) return 0;
    if (k_memcmp(bpb->oem_name, "NOVAFS  ", 8) != 0) return 0;
    if (bpb->bytes_per_sector != FAT_BYTES_PER_SECTOR) return 0;
    if (bpb->total_sectors_32 != FAT_TOTAL_SECTORS) return 0;
    if (bpb->root_cluster != FAT_ROOT_CLUSTER) return 0;
    return 1;
}

static int fat_load_from_persistent_disk(void) {
    fat_total_bytes = FAT_TOTAL_SECTORS * FAT_BYTES_PER_SECTOR;
    fat_first_data_sector = FAT_RESERVED_SECTORS + FAT_NUM_FATS * FAT_SECTORS_PER_FAT;
    fat_cluster_count = (FAT_TOTAL_SECTORS - fat_first_data_sector) / FAT_SECTORS_PER_CLUSTER;

    if (!fat_disk) fat_disk = (uint8_t *)kmalloc(fat_total_bytes);
    if (!fat_disk) return 0;
    if (!fat_probe_persistent_disk()) return 0;

    for (uint32_t sector = 0; sector < FAT_TOTAL_SECTORS; sector++) {
        if (!fat_ata_rw_sector(&fat_persist_dev, sector, fat_sector_ptr(sector), 0)) return 0;
    }
    fat_loaded_from_persistent_disk = fat_validate_loaded_volume();
    if (fat_loaded_from_persistent_disk) fat_dirty_reset();
    return fat_loaded_from_persistent_disk;
}

static int fat_save_to_persistent_disk(void) {
    if (!fat_disk) return 0;
    if (!fat_has_dirty_data) return 1;
    if (!fat_persist_dev.present && !fat_probe_persistent_disk()) return 0;
    for (uint32_t sector = 0; sector < FAT_TOTAL_SECTORS; sector++) {
        uint8_t mask = (uint8_t)(1u << (sector & 7u));
        if (!(fat_dirty_bitmap[sector >> 3] & mask)) continue;
        if (!fat_ata_rw_sector(&fat_persist_dev, sector, fat_sector_ptr(sector), 1)) return 0;
        fat_dirty_bitmap[sector >> 3] &= (uint8_t)~mask;
    }
    fat_has_dirty_data = 0;
    fat_loaded_from_persistent_disk = 1;
    return 1;
}

static void fat_persist_commit(void) {
    if (fat_autosave_enabled) (void)fat_save_to_persistent_disk();
}

static int vfs_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static void vfs_strcpy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    if (!src) { dst[0] = 0; return; }
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void vfs_strcat(char *dst, const char *src, int max) {
    int n = vfs_strlen(dst);
    int i = 0;
    if (!dst || !src || n >= max - 1) return;
    while (src[i] && n + i < max - 1) { dst[n + i] = src[i]; i++; }
    dst[n + i] = 0;
}

static int ascii_upper(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

static int ascii_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static int vfs_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = ascii_lower((unsigned char)*a);
        int cb = ascii_lower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return ascii_lower((unsigned char)*a) - ascii_lower((unsigned char)*b);
}

static int vfs_namecmp(const char *a, const char *b) {
    return vfs_stricmp(a, b);
}

static void get_parent_path(const char *path, char *parent) {
    int len;
    vfs_strcpy(parent, path, VFS_MAX_PATH);
    len = vfs_strlen(parent);
    if (len > 1 && parent[len - 1] == '/') { parent[len - 1] = 0; len--; }
    for (int i = len - 1; i >= 0; i--) {
        if (parent[i] == '/') {
            if (i == 0) parent[1] = 0;
            else parent[i] = 0;
            return;
        }
    }
    parent[0] = '/'; parent[1] = 0;
}

static void get_basename(const char *path, char *name) {
    int len = vfs_strlen(path);
    int start = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { start = i + 1; break; }
    }
    vfs_strcpy(name, path + start, VFS_MAX_NAME);
}

static void split_parent_and_name(const char *path, char *parent, char *name) {
    get_parent_path(path, parent);
    get_basename(path, name);
}

static int path_is_root(const char *path) {
    return path && path[0] == '/' && path[1] == 0;
}

static uint16_t fat_mk16(uint8_t lo, uint8_t hi) { return (uint16_t)lo | ((uint16_t)hi << 8); }

static uint32_t fat_first_sector_of_cluster(uint32_t cluster) {
    return fat_first_data_sector + (cluster - 2u) * FAT_SECTORS_PER_CLUSTER;
}

static uint8_t *fat_sector_ptr(uint32_t sector) {
    return fat_disk + (sector * FAT_BYTES_PER_SECTOR);
}

static uint8_t *fat_cluster_ptr(uint32_t cluster) {
    return fat_sector_ptr(fat_first_sector_of_cluster(cluster));
}

static uint32_t fat_cluster_size(void) {
    return FAT_BYTES_PER_SECTOR * FAT_SECTORS_PER_CLUSTER;
}

static void fat_dirty_reset(void) {
    __builtin_memset(fat_dirty_bitmap, 0, sizeof(fat_dirty_bitmap));
    fat_has_dirty_data = 0;
}

static void fat_mark_sector_dirty(uint32_t sector) {
    if (sector >= FAT_TOTAL_SECTORS) return;
    fat_dirty_bitmap[sector >> 3] |= (uint8_t)(1u << (sector & 7u));
    fat_has_dirty_data = 1;
}

static void fat_mark_sector_range_dirty(uint32_t start_sector, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) fat_mark_sector_dirty(start_sector + i);
}

static void fat_mark_cluster_dirty(uint32_t cluster) {
    if (cluster < 2u) return;
    fat_mark_sector_range_dirty(fat_first_sector_of_cluster(cluster), FAT_SECTORS_PER_CLUSTER);
}

static void fat_mark_dir_slots_dirty(uint32_t cluster, int start_index, int count) {
    uint32_t first_sector;
    uint32_t start_byte;
    uint32_t end_byte;
    uint32_t start_sector;
    uint32_t end_sector;
    if (cluster < 2u || count <= 0 || start_index < 0) return;
    first_sector = fat_first_sector_of_cluster(cluster);
    start_byte = (uint32_t)start_index * (uint32_t)sizeof(fat32_dirent_t);
    end_byte = (uint32_t)(start_index + count) * (uint32_t)sizeof(fat32_dirent_t) - 1u;
    start_sector = first_sector + (start_byte / FAT_BYTES_PER_SECTOR);
    end_sector = first_sector + (end_byte / FAT_BYTES_PER_SECTOR);
    fat_mark_sector_range_dirty(start_sector, end_sector - start_sector + 1u);
}

static uint32_t *fat_primary_fat(void) {
    return (uint32_t *)fat_sector_ptr(FAT_RESERVED_SECTORS);
}

static uint32_t fat_get(uint32_t cluster) {
    uint32_t *fat = fat_primary_fat();
    return fat[cluster] & 0x0FFFFFFFu;
}

static void fat_set(uint32_t cluster, uint32_t value) {
    uint32_t masked = value & 0x0FFFFFFFu;
    uint32_t sector_offset = (cluster * (uint32_t)sizeof(uint32_t)) / FAT_BYTES_PER_SECTOR;
    for (uint32_t i = 0; i < FAT_NUM_FATS; i++) {
        uint32_t sector = FAT_RESERVED_SECTORS + i * FAT_SECTORS_PER_FAT;
        uint32_t *fat = (uint32_t *)fat_sector_ptr(sector);
        fat[cluster] = masked;
        fat_mark_sector_dirty(sector + sector_offset);
    }
}

static int fat_is_eoc(uint32_t value) {
    return value >= FAT_EOC;
}

static void fat_clear_cluster(uint32_t cluster) {
    __builtin_memset(fat_cluster_ptr(cluster), 0, fat_cluster_size());
    fat_mark_cluster_dirty(cluster);
}

static uint32_t fat_chain_last(uint32_t first) {
    uint32_t cur = first;
    if (cur < 2) return 0;
    while (!fat_is_eoc(fat_get(cur))) cur = fat_get(cur);
    return cur;
}

static uint32_t fat_alloc_cluster(uint32_t prev_cluster) {
    for (uint32_t cl = 3; cl < fat_cluster_count + 2; cl++) {
        if (fat_get(cl) == FAT_FREE_CLUSTER) {
            fat_set(cl, FAT_EOC);
            fat_clear_cluster(cl);
            if (prev_cluster >= 2) fat_set(prev_cluster, cl);
            return cl;
        }
    }
    return 0;
}

static void fat_free_chain(uint32_t first_cluster) {
    uint32_t cur = first_cluster;
    while (cur >= 2 && !fat_is_eoc(cur) && cur != FAT_BAD_CLUSTER) {
        uint32_t next = fat_get(cur);
        fat_set(cur, FAT_FREE_CLUSTER);
        if (next == cur) break;
        if (fat_is_eoc(next)) break;
        cur = next;
    }
    if (cur >= 2 && cur != FAT_BAD_CLUSTER && fat_is_eoc(fat_get(cur))) {
        fat_set(cur, FAT_FREE_CLUSTER);
    }
}

static uint32_t fat_file_first_cluster(const fat32_dirent_t *e) {
    return ((uint32_t)e->fst_clus_hi << 16) | e->fst_clus_lo;
}

static void fat_set_first_cluster(fat32_dirent_t *e, uint32_t cluster) {
    e->fst_clus_hi = (uint16_t)((cluster >> 16) & 0xFFFFu);
    e->fst_clus_lo = (uint16_t)(cluster & 0xFFFFu);
}

static uint8_t fat_short_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = ((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + short_name[i];
    return sum;
}

static int fat_is_short_char(int c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    switch (c) {
        case '$': case '%': case '\'': case '-': case '_': case '@':
        case '~': case '`': case '!': case '(': case ')': case '{':
        case '}': case '^': case '#': case '&':
            return 1;
        default:
            return 0;
    }
}

static int fat_name_needs_lfn(const char *name) {
    int len = vfs_strlen(name);
    int dots = 0;
    int base_len = 0, ext_len = 0;
    int seen_dot = 0;
    if (len == 0) return 0;
    for (int i = 0; i < len; i++) {
        int c = name[i];
        if (c == '.') { dots++; seen_dot = 1; continue; }
        if (ascii_upper(c) != c) return 1;
        if (!fat_is_short_char(c)) return 1;
        if (!seen_dot) base_len++;
        else ext_len++;
    }
    if (dots > 1) return 1;
    if (base_len < 1 || base_len > 8) return 1;
    if (ext_len > 3) return 1;
    return 0;
}

static void fat_short_to_text(const uint8_t short_name[11], char *out, int max) {
    int pos = 0;
    int base_end = 7;
    int ext_end = 10;
    while (base_end >= 0 && short_name[base_end] == ' ') base_end--;
    while (ext_end >= 8 && short_name[ext_end] == ' ') ext_end--;
    for (int i = 0; i <= base_end && pos < max - 1; i++) out[pos++] = (char)short_name[i];
    if (ext_end >= 8 && pos < max - 1) out[pos++] = '.';
    for (int i = 8; i <= ext_end && pos < max - 1; i++) out[pos++] = (char)short_name[i];
    out[pos] = 0;
}

static int fat_short_name_exists(uint32_t dir_cluster, const uint8_t short_name[11]) {
    uint32_t cluster = dir_cluster;
    int entries_per_cluster = (int)(fat_cluster_size() / sizeof(fat32_dirent_t));
    while (cluster >= 2) {
        fat32_dirent_t *entries = (fat32_dirent_t *)fat_cluster_ptr(cluster);
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dirent_t *e = &entries[i];
            if (e->name[0] == 0x00) return 0;
            if (e->name[0] == 0xE5 || e->attr == FAT_ATTR_LFN) continue;
            if (k_memcmp(e->name, short_name, 11) == 0) return 1;
        }
        {
            uint32_t next = fat_get(cluster);
            if (fat_is_eoc(next)) break;
            cluster = next;
        }
    }
    return 0;
}

static void fat_build_short_alias(uint32_t dir_cluster, const char *name, uint8_t out[11]) {
    char base[64] = {0};
    char ext[16] = {0};
    int base_len = 0, ext_len = 0;
    int dot = -1;
    int len = vfs_strlen(name);

    for (int i = len - 1; i >= 0; i--) if (name[i] == '.') { dot = i; break; }
    if (dot > 0) {
        for (int i = 0; i < dot && base_len < (int)sizeof(base) - 1; i++) base[base_len++] = name[i];
        for (int i = dot + 1; i < len && ext_len < (int)sizeof(ext) - 1; i++) ext[ext_len++] = name[i];
    } else {
        for (int i = 0; i < len && base_len < (int)sizeof(base) - 1; i++) base[base_len++] = name[i];
    }
    base[base_len] = 0; ext[ext_len] = 0;

    for (int tail = 0; tail < 100; tail++) {
        for (int i = 0; i < 11; i++) out[i] = ' ';
        if (tail == 0 && !fat_name_needs_lfn(name)) {
            int bi = 0, ei = 0;
            for (int i = 0; i < base_len && bi < 8; i++) {
                int c = ascii_upper((unsigned char)base[i]);
                if (fat_is_short_char(c)) out[bi++] = (uint8_t)c;
                else out[bi++] = '_';
            }
            for (int i = 0; i < ext_len && ei < 3; i++) {
                int c = ascii_upper((unsigned char)ext[i]);
                if (fat_is_short_char(c)) out[8 + ei++] = (uint8_t)c;
                else out[8 + ei++] = '_';
            }
        } else {
            int bi = 0, ei = 0;
            for (int i = 0; i < base_len && bi < 6; i++) {
                int c = ascii_upper((unsigned char)base[i]);
                if (c == ' ') continue;
                out[bi++] = fat_is_short_char(c) ? (uint8_t)c : '_';
            }
            out[bi++] = '~';
            out[bi++] = (uint8_t)('1' + (tail % 9));
            for (int i = 0; i < ext_len && ei < 3; i++) {
                int c = ascii_upper((unsigned char)ext[i]);
                out[8 + ei++] = fat_is_short_char(c) ? (uint8_t)c : '_';
            }
        }
        if (!fat_short_name_exists(dir_cluster, out)) return;
    }

    for (int i = 0; i < 11; i++) out[i] = 'X';
}

static void fat_lfn_fill_chars(fat32_lfn_t *lfn, const char *src, int start_index, int total_len) {
    uint16_t *slots[13] = {
        &lfn->name1[0], &lfn->name1[1], &lfn->name1[2], &lfn->name1[3], &lfn->name1[4],
        &lfn->name2[0], &lfn->name2[1], &lfn->name2[2], &lfn->name2[3], &lfn->name2[4], &lfn->name2[5],
        &lfn->name3[0], &lfn->name3[1]
    };
    for (int i = 0; i < 13; i++) {
        int idx = start_index + i;
        if (idx < total_len) *slots[i] = (uint16_t)(uint8_t)src[idx];
        else if (idx == total_len) *slots[i] = 0x0000u;
        else *slots[i] = 0xFFFFu;
    }
}

static int fat_lfn_extract_chars(const fat32_lfn_t *lfn, char *out, int max) {
    int pos = 0;
    uint16_t chars[13] = {
        lfn->name1[0], lfn->name1[1], lfn->name1[2], lfn->name1[3], lfn->name1[4],
        lfn->name2[0], lfn->name2[1], lfn->name2[2], lfn->name2[3], lfn->name2[4], lfn->name2[5],
        lfn->name3[0], lfn->name3[1]
    };
    for (int i = 0; i < 13 && pos < max - 1; i++) {
        uint16_t ch = chars[i];
        if (ch == 0x0000u || ch == 0xFFFFu) break;
        out[pos++] = (char)(ch & 0x00FFu);
    }
    out[pos] = 0;
    return pos;
}

static void fat_write_boot(void) {
    fat32_bpb_t *bpb = (fat32_bpb_t *)fat_sector_ptr(0);
    fat32_fsinfo_t *fsi = (fat32_fsinfo_t *)fat_sector_ptr(1);
    fat32_bpb_t *backup = (fat32_bpb_t *)fat_sector_ptr(6);

    __builtin_memset(bpb, 0, sizeof(fat32_bpb_t));
    bpb->jmp_boot[0] = 0xEB; bpb->jmp_boot[1] = 0x58; bpb->jmp_boot[2] = 0x90;
    k_memcpy(bpb->oem_name, "NOVAFS  ", 8);
    bpb->bytes_per_sector = FAT_BYTES_PER_SECTOR;
    bpb->sectors_per_cluster = FAT_SECTORS_PER_CLUSTER;
    bpb->reserved_sector_count = FAT_RESERVED_SECTORS;
    bpb->num_fats = FAT_NUM_FATS;
    bpb->root_entry_count = 0;
    bpb->total_sectors_16 = 0;
    bpb->media = FAT_MEDIA_TYPE;
    bpb->fat_size_16 = 0;
    bpb->sectors_per_track = 63;
    bpb->num_heads = 255;
    bpb->hidden_sectors = 0;
    bpb->total_sectors_32 = FAT_TOTAL_SECTORS;
    bpb->fat_size_32 = FAT_SECTORS_PER_FAT;
    bpb->ext_flags = 0;
    bpb->fs_version = 0;
    bpb->root_cluster = FAT_ROOT_CLUSTER;
    bpb->fs_info = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_number = 0x80;
    bpb->boot_signature = 0x29;
    bpb->volume_id = 0x20260324u;
    k_memcpy(bpb->volume_label, "NOVA FAT32 ", 11);
    k_memcpy(bpb->fs_type, "FAT32   ", 8);
    bpb->signature = 0xAA55u;

    __builtin_memset(fsi, 0, sizeof(fat32_fsinfo_t));
    fsi->lead_sig = 0x41615252u;
    fsi->struct_sig = 0x61417272u;
    fsi->free_count = fat_cluster_count - 1;
    fsi->next_free = 3;
    fsi->trail_sig = 0xAA550000u;

    __builtin_memcpy(backup, bpb, sizeof(fat32_bpb_t));
    fat_mark_sector_dirty(0);
    fat_mark_sector_dirty(1);
    fat_mark_sector_dirty(6);
}

static void fat_format_empty_volume(void) {
    fat_total_bytes = FAT_TOTAL_SECTORS * FAT_BYTES_PER_SECTOR;
    fat_first_data_sector = FAT_RESERVED_SECTORS + FAT_NUM_FATS * FAT_SECTORS_PER_FAT;
    fat_cluster_count = (FAT_TOTAL_SECTORS - fat_first_data_sector) / FAT_SECTORS_PER_CLUSTER;

    if (!fat_disk) fat_disk = (uint8_t *)kmalloc(fat_total_bytes);
    if (!fat_disk) return;
    __builtin_memset(fat_disk, 0, fat_total_bytes);
    fat_dirty_reset();
    fat_mark_sector_range_dirty(0, FAT_TOTAL_SECTORS);

    fat_write_boot();

    for (uint32_t i = 0; i < FAT_NUM_FATS; i++) {
        uint32_t *fat = (uint32_t *)fat_sector_ptr(FAT_RESERVED_SECTORS + i * FAT_SECTORS_PER_FAT);
        fat[0] = 0x0FFFFFF0u | FAT_MEDIA_TYPE;
        fat[1] = 0x0FFFFFFFu;
        fat[2] = FAT_EOC;
    }
    fat_clear_cluster(FAT_ROOT_CLUSTER);
    fat_loaded_from_persistent_disk = 0;
}

static int fat_find_free_run(uint32_t dir_cluster, int needed, uint32_t *out_cluster, int *out_index) {
    uint32_t cluster = dir_cluster;
    int entries_per_cluster = (int)(fat_cluster_size() / sizeof(fat32_dirent_t));

    while (cluster >= 2) {
        fat32_dirent_t *entries = (fat32_dirent_t *)fat_cluster_ptr(cluster);
        int run = 0;
        int run_start = 0;
        for (int i = 0; i < entries_per_cluster; i++) {
            uint8_t b = entries[i].name[0];
            if (b == 0x00 || b == 0xE5) {
                if (run == 0) run_start = i;
                run++;
                if (run >= needed) {
                    *out_cluster = cluster;
                    *out_index = run_start;
                    return 1;
                }
            } else {
                run = 0;
            }
        }
        {
            uint32_t next = fat_get(cluster);
            if (fat_is_eoc(next)) {
                uint32_t new_cluster = fat_alloc_cluster(cluster);
                if (!new_cluster) return 0;
                *out_cluster = new_cluster;
                *out_index = 0;
                return 1;
            }
            cluster = next;
        }
    }
    return 0;
}

static int fat_list_name_from_entry_chain(uint32_t dir_cluster, int entry_index, char *out_name, int max) {
    (void)dir_cluster;
    (void)entry_index;
    if (max > 0) out_name[0] = 0;
    return 0;
}

static int fat_dir_find_child(uint32_t dir_cluster, const char *target_name, fat_lookup_t *out) {
    uint32_t cluster = dir_cluster;
    int entries_per_cluster = (int)(fat_cluster_size() / sizeof(fat32_dirent_t));
    char lfn_parts[FAT_LAST_LONG_PARTS][14];
    int lfn_valid[FAT_LAST_LONG_PARTS];
    int lfn_count = 0;
    uint32_t first_slot_cluster = 0;
    int first_slot_index = 0;

    if (out) __builtin_memset(out, 0, sizeof(fat_lookup_t));

    while (cluster >= 2) {
        fat32_dirent_t *entries = (fat32_dirent_t *)fat_cluster_ptr(cluster);
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dirent_t *e = &entries[i];
            if (e->name[0] == 0x00) return 0;
            if (e->name[0] == 0xE5) {
                lfn_count = 0;
                __builtin_memset(lfn_valid, 0, sizeof(lfn_valid));
                continue;
            }
            if (e->attr == FAT_ATTR_LFN) {
                fat32_lfn_t *lfn = (fat32_lfn_t *)e;
                int ord = lfn->ord & 0x1Fu;
                if (lfn->ord & 0x40u) {
                    lfn_count = ord;
                    __builtin_memset(lfn_valid, 0, sizeof(lfn_valid));
                    first_slot_cluster = cluster;
                    first_slot_index = i;
                }
                if (ord >= 1 && ord <= FAT_LAST_LONG_PARTS) {
                    fat_lfn_extract_chars(lfn, lfn_parts[ord - 1], sizeof(lfn_parts[ord - 1]));
                    lfn_valid[ord - 1] = 1;
                }
                continue;
            }

            char visible[VFS_MAX_NAME];
            visible[0] = 0;
            if (lfn_count > 0) {
                for (int p = 0; p < lfn_count && p < FAT_LAST_LONG_PARTS; p++) {
                    if (lfn_valid[p]) vfs_strcat(visible, lfn_parts[p], VFS_MAX_NAME);
                }
            }
            if (!visible[0]) fat_short_to_text(e->name, visible, VFS_MAX_NAME);

            if (vfs_namecmp(visible, target_name) == 0) {
                if (out) {
                    out->found = 1;
                    out->is_dir = (e->attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
                    out->first_slot_cluster = lfn_count > 0 ? first_slot_cluster : cluster;
                    out->first_slot_index = lfn_count > 0 ? first_slot_index : i;
                    out->short_slot_cluster = cluster;
                    out->short_slot_index = i;
                    out->first_cluster = fat_file_first_cluster(e);
                    out->size = e->file_size;
                    out->dirent = *e;
                    vfs_strcpy(out->name, visible, VFS_MAX_NAME);
                }
                return 1;
            }

            lfn_count = 0;
            __builtin_memset(lfn_valid, 0, sizeof(lfn_valid));
        }
        {
            uint32_t next = fat_get(cluster);
            if (fat_is_eoc(next)) break;
            cluster = next;
        }
    }
    return 0;
}

static int fat_path_lookup(const char *path, fat_lookup_t *out) {
    char component[VFS_MAX_NAME];
    int ci = 0;
    uint32_t current_cluster = FAT_ROOT_CLUSTER;
    const char *p = path;

    if (out) __builtin_memset(out, 0, sizeof(fat_lookup_t));
    if (!path || !path[0]) return 0;
    if (path_is_root(path)) {
        if (out) {
            out->found = 1;
            out->is_dir = 1;
            out->first_cluster = FAT_ROOT_CLUSTER;
            vfs_strcpy(out->name, "/", VFS_MAX_NAME);
        }
        return 1;
    }

    if (*p == '/') p++;
    while (1) {
        ci = 0;
        while (*p && *p != '/' && ci < VFS_MAX_NAME - 1) component[ci++] = *p++;
        component[ci] = 0;
        if (!component[0]) break;

        fat_lookup_t step;
        if (!fat_dir_find_child(current_cluster, component, &step)) return 0;

        if (*p == '/') {
            if (!step.is_dir) return 0;
            current_cluster = step.first_cluster ? step.first_cluster : FAT_ROOT_CLUSTER;
            p++;
            if (!*p) {
                if (out) *out = step;
                return 1;
            }
        } else {
            if (out) *out = step;
            return 1;
        }
    }
    return 0;
}

static void fat_write_dir_record(uint32_t cluster, int index, const void *record) {
    fat32_dirent_t *entries = (fat32_dirent_t *)fat_cluster_ptr(cluster);
    __builtin_memcpy(&entries[index], record, sizeof(fat32_dirent_t));
    fat_mark_dir_slots_dirty(cluster, index, 1);
}

static int fat_create_entry(const char *path, int is_dir, const char *data, uint32_t len) {
    char parent[VFS_MAX_PATH], name[VFS_MAX_NAME];
    fat_lookup_t parent_lu;
    uint32_t dir_cluster;
    uint8_t short_name[11];
    int name_len;
    int lfn_count;
    uint32_t slot_cluster;
    int slot_index;
    uint32_t first_cluster = 0;

    if (!path || !path[0] || path_is_root(path)) return -1;
    split_parent_and_name(path, parent, name);
    if (!name[0]) return -1;
    if (!fat_path_lookup(parent, &parent_lu) || !parent_lu.is_dir) return -1;
    dir_cluster = parent_lu.first_cluster ? parent_lu.first_cluster : FAT_ROOT_CLUSTER;

    if (fat_dir_find_child(dir_cluster, name, NULL)) return -1;

    fat_build_short_alias(dir_cluster, name, short_name);
    name_len = vfs_strlen(name);
    lfn_count = fat_name_needs_lfn(name) ? (name_len + 12) / 13 : 0;

    if (!fat_find_free_run(dir_cluster, lfn_count + 1, &slot_cluster, &slot_index)) return -1;

    if (is_dir) {
        first_cluster = fat_alloc_cluster(0);
        if (!first_cluster) return -1;
        fat32_dirent_t *dot = (fat32_dirent_t *)fat_cluster_ptr(first_cluster);
        fat32_dirent_t *dotdot = dot + 1;
        __builtin_memset(dot, 0, sizeof(fat32_dirent_t) * 2);
        for (int i = 0; i < 11; i++) { dot->name[i] = ' '; dotdot->name[i] = ' '; }
        dot->name[0] = '.';
        dot->attr = FAT_ATTR_DIRECTORY;
        fat_set_first_cluster(dot, first_cluster);
        dotdot->name[0] = '.'; dotdot->name[1] = '.';
        dotdot->attr = FAT_ATTR_DIRECTORY;
        fat_set_first_cluster(dotdot, dir_cluster);
        fat_mark_cluster_dirty(first_cluster);
    } else if (len > 0) {
        uint32_t remaining = len;
        const uint8_t *src = (const uint8_t *)data;
        uint32_t prev = 0;
        while (remaining > 0) {
            uint32_t cl = fat_alloc_cluster(prev);
            uint32_t copy = remaining < fat_cluster_size() ? remaining : fat_cluster_size();
            if (!cl) {
                if (first_cluster) fat_free_chain(first_cluster);
                return -1;
            }
            if (!first_cluster) first_cluster = cl;
            __builtin_memcpy(fat_cluster_ptr(cl), src, copy);
            if (copy < fat_cluster_size()) __builtin_memset(fat_cluster_ptr(cl) + copy, 0, fat_cluster_size() - copy);
            fat_mark_cluster_dirty(cl);
            src += copy;
            remaining -= copy;
            prev = cl;
        }
    }

    for (int i = 0; i < lfn_count; i++) {
        fat32_lfn_t lfn;
        int ord = lfn_count - i;
        int start = (ord - 1) * 13;
        __builtin_memset(&lfn, 0xFF, sizeof(lfn));
        lfn.ord = (uint8_t)ord;
        if (ord == lfn_count) lfn.ord |= 0x40u;
        lfn.attr = FAT_ATTR_LFN;
        lfn.type = 0;
        lfn.checksum = fat_short_checksum(short_name);
        lfn.first_cluster_low = 0;
        fat_lfn_fill_chars(&lfn, name, start, name_len);
        fat_write_dir_record(slot_cluster, slot_index + i, &lfn);
    }

    {
        fat32_dirent_t entry;
        __builtin_memset(&entry, 0, sizeof(entry));
        __builtin_memcpy(entry.name, short_name, 11);
        entry.attr = is_dir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
        entry.file_size = is_dir ? 0 : len;
        fat_set_first_cluster(&entry, first_cluster);
        fat_write_dir_record(slot_cluster, slot_index + lfn_count, &entry);
    }

    return 0;
}

static int fat_mark_deleted(const fat_lookup_t *lu) {
    if (!lu || !lu->found) return -1;
    if (lu->first_slot_cluster != lu->short_slot_cluster) return -1;
    fat32_dirent_t *entries = (fat32_dirent_t *)fat_cluster_ptr(lu->first_slot_cluster);
    for (int i = lu->first_slot_index; i <= lu->short_slot_index; i++) entries[i].name[0] = 0xE5;
    fat_mark_dir_slots_dirty(lu->first_slot_cluster, lu->first_slot_index, lu->short_slot_index - lu->first_slot_index + 1);
    if (lu->first_cluster >= 2) fat_free_chain(lu->first_cluster);
    return 0;
}

static int fat_dir_is_empty(uint32_t dir_cluster) {
    uint32_t cluster = dir_cluster;
    int entries_per_cluster = (int)(fat_cluster_size() / sizeof(fat32_dirent_t));
    while (cluster >= 2) {
        fat32_dirent_t *entries = (fat32_dirent_t *)fat_cluster_ptr(cluster);
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dirent_t *e = &entries[i];
            char short_txt[16];
            if (e->name[0] == 0x00) return 1;
            if (e->name[0] == 0xE5 || e->attr == FAT_ATTR_LFN) continue;
            fat_short_to_text(e->name, short_txt, sizeof(short_txt));
            if (vfs_namecmp(short_txt, ".") != 0 && vfs_namecmp(short_txt, "..") != 0) return 0;
        }
        {
            uint32_t next = fat_get(cluster);
            if (fat_is_eoc(next)) break;
            cluster = next;
        }
    }
    return 1;
}

static uint32_t fat_read_file_to_buf(uint32_t first_cluster, uint32_t size, uint8_t *buf, uint32_t maxlen) {
    uint32_t copied = 0;
    uint32_t cluster = first_cluster;
    uint32_t remaining = size;
    if (!buf || maxlen == 0) return 0;
    if (size == 0 || first_cluster < 2) {
        buf[0] = 0;
        return 0;
    }
    while (cluster >= 2 && remaining > 0 && copied < maxlen) {
        uint32_t chunk = remaining < fat_cluster_size() ? remaining : fat_cluster_size();
        if (chunk > maxlen - copied) chunk = maxlen - copied;
        __builtin_memcpy(buf + copied, fat_cluster_ptr(cluster), chunk);
        copied += chunk;
        remaining -= chunk;
        if (remaining == 0) break;
        {
            uint32_t next = fat_get(cluster);
            if (fat_is_eoc(next)) break;
            cluster = next;
        }
    }
    if (copied < maxlen) buf[copied] = 0;
    else buf[maxlen - 1] = 0;
    return copied;
}

static int cache_find_node(const char *path) {
    for (int i = 0; i < vfs_node_count; i++) {
        if (vfs_nodes[i].valid && k_strcmp(vfs_nodes[i].path, path) == 0) return i;
    }
    return -1;
}

static int cache_alloc_node(void) {
    if (vfs_node_count >= VFS_MAX_FILES) return -1;
    return vfs_node_count++;
}

static void cache_add_node(const char *path, const char *name, int parent_idx, vfs_node_type_t type,
                           uint32_t size, uint32_t perms, uint32_t first_cluster) {
    int idx = cache_alloc_node();
    if (idx < 0) return;
    vfs_node_t *n = &vfs_nodes[idx];
    __builtin_memset(n, 0, sizeof(vfs_node_t));
    n->valid = 1;
    n->type = type;
    n->size = size;
    n->permissions = perms;
    n->owner_uid = 0;
    n->owner_gid = 0;
    n->parent_idx = parent_idx;
    n->created = first_cluster;
    n->modified = first_cluster;
    vfs_strcpy(n->name, name, VFS_MAX_NAME);
    vfs_strcpy(n->path, path, VFS_MAX_PATH);
}

static void fat_cache_walk_dir(uint32_t dir_cluster, const char *dir_path, int parent_idx) {
    uint32_t cluster = dir_cluster;
    int entries_per_cluster = (int)(fat_cluster_size() / sizeof(fat32_dirent_t));
    char lfn_parts[FAT_LAST_LONG_PARTS][14];
    int lfn_valid[FAT_LAST_LONG_PARTS];
    int lfn_count = 0;

    while (cluster >= 2) {
        fat32_dirent_t *entries = (fat32_dirent_t *)fat_cluster_ptr(cluster);
        for (int i = 0; i < entries_per_cluster; i++) {
            fat32_dirent_t *e = &entries[i];
            if (e->name[0] == 0x00) return;
            if (e->name[0] == 0xE5) {
                lfn_count = 0;
                __builtin_memset(lfn_valid, 0, sizeof(lfn_valid));
                continue;
            }
            if (e->attr == FAT_ATTR_LFN) {
                fat32_lfn_t *lfn = (fat32_lfn_t *)e;
                int ord = lfn->ord & 0x1Fu;
                if (lfn->ord & 0x40u) {
                    lfn_count = ord;
                    __builtin_memset(lfn_valid, 0, sizeof(lfn_valid));
                }
                if (ord >= 1 && ord <= FAT_LAST_LONG_PARTS) {
                    fat_lfn_extract_chars(lfn, lfn_parts[ord - 1], sizeof(lfn_parts[ord - 1]));
                    lfn_valid[ord - 1] = 1;
                }
                continue;
            }

            char name[VFS_MAX_NAME];
            char path[VFS_MAX_PATH];
            uint32_t first_cluster = fat_file_first_cluster(e);
            uint32_t size = e->file_size;
            int is_dir = (e->attr & FAT_ATTR_DIRECTORY) ? 1 : 0;

            name[0] = 0;
            if (lfn_count > 0) {
                for (int p = 0; p < lfn_count && p < FAT_LAST_LONG_PARTS; p++) {
                    if (lfn_valid[p]) vfs_strcat(name, lfn_parts[p], VFS_MAX_NAME);
                }
            }
            if (!name[0]) fat_short_to_text(e->name, name, VFS_MAX_NAME);
            lfn_count = 0;
            __builtin_memset(lfn_valid, 0, sizeof(lfn_valid));

            if (vfs_namecmp(name, ".") == 0 || vfs_namecmp(name, "..") == 0) continue;

            if (path_is_root(dir_path)) {
                path[0] = '/'; path[1] = 0;
                vfs_strcat(path, name, VFS_MAX_PATH);
            } else {
                vfs_strcpy(path, dir_path, VFS_MAX_PATH);
                vfs_strcat(path, "/", VFS_MAX_PATH);
                vfs_strcat(path, name, VFS_MAX_PATH);
            }

            cache_add_node(path, name, parent_idx, is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE,
                           size, is_dir ? 0755u : 0644u, first_cluster);
            if (vfs_node_count <= 0) continue;
            {
                int idx = vfs_node_count - 1;
                if (!is_dir) {
                    uint32_t to_read = size < FAT_CACHE_FILE_READ_MAX ? size : FAT_CACHE_FILE_READ_MAX;
                    fat_read_file_to_buf(first_cluster, to_read, vfs_nodes[idx].data, FAT_CACHE_FILE_READ_MAX + 1);
                } else {
                    fat_cache_walk_dir(first_cluster ? first_cluster : FAT_ROOT_CLUSTER, path, idx);
                }
            }
        }
        {
            uint32_t next = fat_get(cluster);
            if (fat_is_eoc(next)) break;
            cluster = next;
        }
    }
}

static void fat_rebuild_cache(void) {
    __builtin_memset(vfs_nodes, 0, sizeof(vfs_nodes));
    __builtin_memset(fds, 0, sizeof(fds));
    vfs_node_count = 0;
    cache_add_node("/", "/", -1, VFS_TYPE_DIR, 0, 0755u, FAT_ROOT_CLUSTER);
    fat_cache_walk_dir(FAT_ROOT_CLUSTER, "/", 0);
}

static uint32_t vfs_parse_u32(const char *s) {
    uint32_t value = 0;
    while (s && *s >= '0' && *s <= '9') {
        value = value * 10u + (uint32_t)(*s - '0');
        s++;
    }
    return value;
}

static void vfs_u32_to_str(uint32_t value, char *buf, int max) {
    char tmp[16];
    int pos = 0;
    int i = 0;
    if (!buf || max <= 0) return;
    if (value == 0) {
        buf[0] = '0';
        if (max > 1) buf[1] = 0;
        return;
    }
    while (value > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (i > 0 && pos < max - 1) buf[pos++] = tmp[--i];
    buf[pos] = 0;
}

static void vfs_create_or_replace_text(const char *path, const char *text) {
    fat_lookup_t lu;
    if (fat_path_lookup(path, &lu) && !lu.is_dir) fat_mark_deleted(&lu);
    fat_create_entry(path, 0, text, (uint32_t)vfs_strlen(text));
}

static void vfs_ensure_dir(const char *path) {
    fat_lookup_t lu;
    if (!fat_path_lookup(path, &lu)) (void)fat_create_entry(path, 1, NULL, 0);
}

static void vfs_ensure_file(const char *path, const char *text) {
    fat_lookup_t lu;
    if (!fat_path_lookup(path, &lu)) (void)fat_create_entry(path, 0, text, (uint32_t)vfs_strlen(text));
}

static void vfs_refresh_persistence_info(void) {
    char info[256];
    info[0] = 0;
    vfs_strcpy(info, "backend=", sizeof(info));
    vfs_strcat(info, fat_persist_dev.present ? fat_persist_dev.label : "ram-only", sizeof(info));
    vfs_strcat(info, "\nstatus=", sizeof(info));
    vfs_strcat(info, fat_loaded_from_persistent_disk ? "loaded-from-disk" : (fat_persist_dev.present ? "fresh-volume-saved" : "volatile-ram"), sizeof(info));
    vfs_strcat(info, "\nvolume=FAT32\nsize_bytes=4194304\n", sizeof(info));
    vfs_create_or_replace_text("/proc/persistence", info);
}

static void vfs_update_boot_counter(void) {
    char current[64];
    char next[64];
    uint32_t boot_count = 0;
    current[0] = 0;
    fat_rebuild_cache();
    vfs_get_contents("/var/log/boot.count", current, sizeof(current));
    boot_count = vfs_parse_u32(current) + 1u;
    vfs_u32_to_str(boot_count, next, sizeof(next));
    vfs_strcat(next, "\n", sizeof(next));
    vfs_create_or_replace_text("/var/log/boot.count", next);
}

void vfs_init(void) {
    fat_autosave_enabled = 0;
    if (!fat_load_from_persistent_disk()) fat_format_empty_volume();
    if (!fat_disk) return;

    fat_rebuild_cache();

    vfs_ensure_dir("/home");
    vfs_ensure_dir("/home/user");
    vfs_ensure_dir("/home/user/Desktop");
    vfs_ensure_dir("/home/user/Documents");
    vfs_ensure_dir("/home/user/Downloads");
    vfs_ensure_dir("/home/user/Images");
    vfs_ensure_dir("/home/user/Music");
    vfs_ensure_dir("/home/user/Videos");
    vfs_ensure_dir("/home/user/Projects");
    vfs_ensure_dir("/etc");
    vfs_ensure_dir("/bin");
    vfs_ensure_dir("/usr");
    vfs_ensure_dir("/usr/share");
    vfs_ensure_dir("/lib");
    vfs_ensure_dir("/lib/modules");
    vfs_ensure_dir("/tmp");
    vfs_ensure_dir("/dev");
    vfs_ensure_dir("/proc");
    vfs_ensure_dir("/var");
    vfs_ensure_dir("/var/log");
    vfs_ensure_dir("/media");
    vfs_ensure_dir("/mnt");
    vfs_ensure_dir("/system");
    vfs_ensure_dir("/system/packages");
    vfs_ensure_dir("/system/menu");
    vfs_ensure_dir("/system/store");
    vfs_ensure_dir("/system/store/packages");
    vfs_ensure_dir("/webcache");

    vfs_ensure_file("/etc/hostname", "nova-desktop\n");
    vfs_ensure_file("/etc/install.done", "done\n");
    vfs_ensure_file("/etc/os-release",
        "NAME=NovaOS\nVERSION=6.0\nARCH=x86_64\nFILESYSTEM=FAT32\nKERNEL=6.0.0\n");
    vfs_ensure_file("/etc/network.conf",
        "# Network Configuration\nINTERFACE=eth0\nDHCP=yes\nIP=10.0.2.15\nGATEWAY=10.0.2.2\nDNS=8.8.8.8\n");
    vfs_ensure_file("/etc/activation.key", "\n");
    vfs_ensure_file("/etc/activation.state", "status=inactive\nedition=Aucune\nchannel=none\n");
    vfs_ensure_file("/system/packages/org.mozilla.firefox.nova",
        "NOVA-PKG-1\n"
        "id=org.mozilla.firefox\n"
        "name=Firefox\n"
        "version=1.0.0\n"
        "launch_type=url\n"
        "launch_target=https://www.mozilla.org/firefox/new/\n"
        "description=Firefox preinstalle par defaut avec raccourcis Bureau + Demarrer et ouverture dans le navigateur live.\n"
        "desktop_shortcut=true\n"
        "menu_shortcut=true\n");
    vfs_ensure_file("/system/menu/org.mozilla.firefox.nlink",
        "NOVA-LINK-1\n"
        "id=org.mozilla.firefox\n"
        "name=Firefox\n"
        "launch_type=url\n"
        "launch_target=https://www.mozilla.org/firefox/new/\n");
    vfs_ensure_file("/home/user/Desktop/org.mozilla.firefox.nlink",
        "NOVA-LINK-1\n"
        "id=org.mozilla.firefox\n"
        "name=Firefox\n"
        "launch_type=url\n"
        "launch_target=https://www.mozilla.org/firefox/new/\n");
    vfs_ensure_file("/home/user/readme.txt", "");
    vfs_ensure_file("/home/user/notes.txt",
        "# Notes\n\n- [ ] Verifier la session locale\n- [ ] Vérifier le Centre Système (F7)\n- [ ] Verifier le tableau de bord (F9)\n- [ ] Mettre a jour les notes (F11)\n- [ ] Relire la checklist et le plan\n");
    vfs_ensure_file("/home/user/CHANGELOG.txt", "");
    vfs_ensure_file("/home/user/Desktop/Welcome.txt", "");
    vfs_ensure_file("/home/user/Documents/Commandes Terminal.txt",
        "Commandes utiles: help, sysinfo, mem, heap, monitor, tree /home, open dashboard, open notes, open commands, cmd ouvre monitor, release, qa, livrable, plan, logs, beep\n");
    vfs_ensure_file("/home/user/Projects/TODO.txt",
        "- Tester le Centre Système\n- Tester le tableau de bord\n- Verifier les notes\n- Valider l'audio et le navigateur\n- Verifier la checklist et le plan\n");
    vfs_ensure_file("/home/user/Tutoriel.txt", "");
    vfs_ensure_file("/var/log/system.log", "");
    vfs_ensure_file("/etc/users",
        "root:x:0:0:root:/root:/bin/shell\nuser:x:1000:1000:User:/home/user:/bin/shell\n");
    vfs_ensure_file("/tmp/session.tmp", "session_start=true\nuser=user\n");
    vfs_ensure_file("/proc/version", "x86_64 SMP FAT32\n");
    vfs_ensure_file("/proc/cpuinfo",
        "processor\t: 0\nvendor_id\t: GenuineIntel\n"
        "model name\t: NovaOS Virtual CPU\n"
        "filesystem\t: FAT32 + optional persistent ATA backend\n");
    vfs_ensure_file("/proc/meminfo",
        "MemTotal\t: 262144 kB\nMemFree\t\t: 196608 kB\nBuffers\t\t: 8192 kB\nCached\t\t: 32768 kB\n");
    {
        char audio_info[512];
        sound_fill_audioinfo(audio_info, sizeof(audio_info));
        vfs_create_or_replace_text("/proc/audioinfo", audio_info);
    }
    vfs_ensure_file("/home/user/Documents/System Monitor.txt", "");
    vfs_ensure_file("/home/user/Documents/QEMU-Validation.txt", "");
    vfs_ensure_file("/home/user/Commandes.txt", "");

    vfs_ensure_file("/home/user/LIVRABLE.txt", "");
    vfs_ensure_file("/home/user/Documents/QA-Checklist.txt", "");
    vfs_ensure_file("/home/user/Documents/Plan.txt", "");

    vfs_ensure_file("/home/user/Documents/Checklist-Diffusion.txt", "");
    vfs_ensure_file("/system/store/catalog.txt", "");
    vfs_ensure_file("/system/store/packages/org.mozilla.firefox.nova",
        "NOVA-PKG-1\n"
        "id=org.mozilla.firefox\n"
        "name=Firefox\n"
        "version=1.1.0\n"
        "launch_type=url\n"
        "launch_target=https://www.mozilla.org/firefox/new/\n"
        "description=Firefox accessible depuis le navigateur integre.\n"
        "desktop_shortcut=true\n"
        "menu_shortcut=true\n");
    vfs_ensure_file("/system/store/packages/org.nova.portal.activation.nova",
        "NOVA-PKG-1\n"
        "id=org.nova.portal.activation\n"
        "name=Gestion des editions\n"
        "version=1.0.0\n"
        "launch_type=url\n"
        "launch_target=file:///activation_portal/index.html\n"
        "description=Outil local pour generer une cle d'edition.\n"
        "desktop_shortcut=false\n"
        "menu_shortcut=true\n");
    vfs_ensure_file("/system/store/packages/org.nova.docs.bundle.nova",
        "NOVA-PKG-1\n"
        "id=org.nova.docs.bundle\n"
        "name=Livrable\n"
        "version=1.0.0\n"
        "launch_type=url\n"
        "launch_target=file:///home/user/LIVRABLE.txt\n"
        "description=Documents de livraison et notes techniques.\n"
        "desktop_shortcut=false\n"
        "menu_shortcut=true\n");
    vfs_ensure_file("/system/store/packages/org.nova.files.nova",
        "NOVA-PKG-1\n"
        "id=org.nova.files\n"
        "name=Fichiers\n"
        "version=1.0.0\n"
        "launch_type=url\n"
        "launch_target=nova://files\n"
        "description=Ouvre le gestionnaire de fichiers local.\n"
        "desktop_shortcut=false\n"
        "menu_shortcut=true\n");
    vfs_ensure_file("/system/store/packages/org.nova.terminal.nova",
        "NOVA-PKG-1\n"
        "id=org.nova.terminal\n"
        "name=Terminal\n"
        "version=1.0.0\n"
        "launch_type=url\n"
        "launch_target=file:///home/user/Documents/Commandes Terminal.txt\n"
        "description=Ouvre le terminal local.\n"
        "desktop_shortcut=false\n"
        "menu_shortcut=true\n");
    vfs_ensure_file("/system/store/packages/org.nova.settings.nova",
        "NOVA-PKG-1\n"
        "id=org.nova.settings\n"
        "name=Parametres\n"
        "version=1.0.0\n"
        "launch_type=url\n"
        "launch_target=file:///home/user/readme.txt\n"
        "description=Acces aux themes, a l'edition et aux informations systeme.\n"
        "desktop_shortcut=false\n"
        "menu_shortcut=true\n");
    vfs_ensure_file("/system/store/packages/org.nova.monitor.nova",
        "NOVA-PKG-1\n"
        "id=org.nova.monitor\n"
        "name=Centre Systeme\n"
        "version=1.0.0\n"
        "launch_type=url\n"
        "launch_target=file:///home/user/Documents/System Monitor.txt\n"
        "description=Vue temps reel des metriques systeme.\n"
        "desktop_shortcut=false\n"
        "menu_shortcut=true\n");
    vfs_ensure_file("/system/store/packages/org.nova.hub.nova",
        "NOVA-PKG-1\n"
        "id=org.nova.hub\n"
        "name=Tableau de bord\n"
        "version=1.0.0\n"
        "launch_type=url\n"
        "launch_target=nova://release-notes\n"
        "description=Vue d'ensemble de la session et acces rapides.\n"
        "desktop_shortcut=false\n"
        "menu_shortcut=true\n");
    vfs_ensure_file("/system/store/packages/org.nova.notes.nova",
        "NOVA-PKG-1\n"
        "id=org.nova.notes\n"
        "name=Notes\n"
        "version=1.0.0\n"
        "launch_type=url\n"
        "launch_target=file:///home/user/notes.txt\n"
        "description=Capture taches, idees et feedback.\n"
        "desktop_shortcut=false\n"
        "menu_shortcut=true\n");
    vfs_ensure_file("/system/store/packages/org.nova.symera.nova",
        "NOVA-PKG-1\n"
        "id=org.nova.symera\n"
        "name=Commandes\n"
        "version=1.0.0\n"
        "launch_type=url\n"
        "launch_target=file:///home/user/Commandes.txt\n"
        "description=Interface locale pour ouvrir les applications et documents.\n"
        "desktop_shortcut=false\n"
        "menu_shortcut=true\n");
    vfs_ensure_file("/system/store/packages/org.nova.qa.nova",
        "NOVA-PKG-1\n"
        "id=org.nova.qa\n"
        "name=QA Checklist\n"
        "version=1.0.0\n"
        "launch_type=url\n"
        "launch_target=file:///home/user/Documents/QA-Checklist.txt\n"
        "description=Checklist de verification locale.\n"
        "desktop_shortcut=false\n"
        "menu_shortcut=true\n");

    for (int i = 0; i < nova_webcache_entry_count; i++) {
        vfs_ensure_file(nova_webcache_entries[i].path, nova_webcache_entries[i].content);
    }

    vfs_refresh_persistence_info();
    vfs_update_boot_counter();
    fat_rebuild_cache();
    fat_autosave_enabled = 1;
    fat_persist_commit();
}

int vfs_mkdir(const char *path) {
    int rc = fat_create_entry(path, 1, NULL, 0);
    fat_rebuild_cache();
    fat_persist_commit();
    return rc;
}

int vfs_create(const char *path, const char *data, uint32_t len) {
    fat_lookup_t lu;
    if (fat_path_lookup(path, &lu) && !lu.is_dir) fat_mark_deleted(&lu);
    {
        int rc = fat_create_entry(path, 0, data, len);
        fat_rebuild_cache();
        fat_persist_commit();
        return rc;
    }
}

int vfs_write_file(const char *path, const char *data, uint32_t len) {
    return vfs_create(path, data, len);
}

int vfs_open(const char *path, int flags) {
    int idx = cache_find_node(path);
    if (idx < 0) return -1;
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (!fds[i].valid) {
            fds[i].valid = 1;
            fds[i].node_idx = idx;
            fds[i].offset = 0;
            fds[i].flags = flags;
            return i;
        }
    }
    return -1;
}

void vfs_close(int fd) {
    if (fd >= 0 && fd < VFS_MAX_FD) fds[fd].valid = 0;
}

int vfs_read(int fd, uint8_t *buf, uint32_t len) {
    if (fd < 0 || fd >= VFS_MAX_FD || !fds[fd].valid) return -1;
    vfs_node_t *n = &vfs_nodes[fds[fd].node_idx];
    uint32_t avail = (n->size > fds[fd].offset) ? (n->size - fds[fd].offset) : 0;
    uint32_t to_read = len < avail ? len : avail;
    if (!to_read) return 0;
    __builtin_memcpy(buf, n->data + fds[fd].offset, to_read);
    fds[fd].offset += to_read;
    return (int)to_read;
}

int vfs_write(int fd, const uint8_t *buf, uint32_t len) {
    char path[VFS_MAX_PATH];
    uint8_t tmp[VFS_MAX_DATA];
    uint32_t old_size, new_size;
    if (fd < 0 || fd >= VFS_MAX_FD || !fds[fd].valid) return -1;
    vfs_node_t *n = &vfs_nodes[fds[fd].node_idx];
    if (n->type != VFS_TYPE_FILE) return -1;
    vfs_strcpy(path, n->path, VFS_MAX_PATH);
    old_size = n->size < VFS_MAX_DATA ? n->size : VFS_MAX_DATA;
    __builtin_memset(tmp, 0, sizeof(tmp));
    __builtin_memcpy(tmp, n->data, old_size);
    if (fds[fd].offset > VFS_MAX_DATA) return 0;
    new_size = fds[fd].offset + len;
    if (new_size > VFS_MAX_DATA) new_size = VFS_MAX_DATA;
    __builtin_memcpy(tmp + fds[fd].offset, buf, new_size - fds[fd].offset);
    vfs_create(path, (const char *)tmp, new_size);
    fds[fd].node_idx = cache_find_node(path);
    fds[fd].offset = new_size;
    return (int)(new_size - old_size >= len ? len : (new_size > fds[fd].offset ? new_size - fds[fd].offset : len));
}

int vfs_exists(const char *path) {
    return cache_find_node(path) >= 0;
}

int vfs_delete(const char *path) {
    fat_lookup_t lu;
    if (!fat_path_lookup(path, &lu)) return -1;
    if (lu.is_dir && !fat_dir_is_empty(lu.first_cluster)) return -1;
    {
        int rc = fat_mark_deleted(&lu);
        fat_rebuild_cache();
        fat_persist_commit();
        return rc;
    }
}

int vfs_list(const char *dir, char names[][VFS_MAX_NAME], int *types, int max) {
    int count = 0;
    int dir_len;
    if (!dir || max <= 0) return 0;
    dir_len = vfs_strlen(dir);
    for (int i = 0; i < vfs_node_count && count < max; i++) {
        if (!vfs_nodes[i].valid) continue;
        if (k_strcmp(vfs_nodes[i].path, dir) == 0) continue;
        if (path_is_root(dir)) {
            const char *p = vfs_nodes[i].path;
            if (p[0] != '/' || !p[1]) continue;
            p++;
            int has_more = 0;
            for (int j = 0; p[j]; j++) if (p[j] == '/') { has_more = 1; break; }
            if (has_more) continue;
        } else {
            if (k_strncmp(vfs_nodes[i].path, dir, dir_len) != 0) continue;
            if (vfs_nodes[i].path[dir_len] != '/') continue;
            const char *rest = vfs_nodes[i].path + dir_len + 1;
            int has_more = 0;
            for (int j = 0; rest[j]; j++) if (rest[j] == '/') { has_more = 1; break; }
            if (has_more) continue;
        }
        vfs_strcpy(names[count], vfs_nodes[i].name, VFS_MAX_NAME);
        if (types) types[count] = vfs_nodes[i].type;
        count++;
    }
    return count;
}

vfs_node_t *vfs_get_node(const char *path) {
    int idx = cache_find_node(path);
    return idx >= 0 ? &vfs_nodes[idx] : (vfs_node_t *)0;
}

uint32_t vfs_get_size(const char *path) {
    vfs_node_t *n = vfs_get_node(path);
    return n ? n->size : 0;
}

void vfs_get_contents(const char *path, char *buf, uint32_t maxlen) {
    vfs_node_t *n = vfs_get_node(path);
    uint32_t sz;
    if (!buf || maxlen == 0) return;
    if (!n || n->type != VFS_TYPE_FILE) { buf[0] = 0; return; }
    sz = n->size < maxlen - 1 ? n->size : maxlen - 1;
    __builtin_memcpy(buf, n->data, sz);
    buf[sz] = 0;
}

int vfs_set_permissions(const char *path, uint32_t mode) {
    vfs_node_t *n = vfs_get_node(path);
    if (!n) return -1;
    n->permissions = mode;
    return 0;
}

int vfs_set_owner(const char *path, uint32_t uid, uint32_t gid) {
    vfs_node_t *n = vfs_get_node(path);
    if (!n) return -1;
    n->owner_uid = uid;
    n->owner_gid = gid;
    return 0;
}

void vfs_format_mode(const char *path, char *buf, int max) {
    vfs_node_t *n = vfs_get_node(path);
    uint32_t mode;
    if (!buf || max <= 0) return;
    if (!n) { buf[0] = 0; return; }
    mode = n->permissions;
    if (max < 11) { buf[0] = 0; return; }
    buf[0] = (n->type == VFS_TYPE_DIR) ? 'd' : '-';
    buf[1] = (mode & 0400u) ? 'r' : '-';
    buf[2] = (mode & 0200u) ? 'w' : '-';
    buf[3] = (mode & 0100u) ? ((mode & 04000u) ? 's' : 'x') : ((mode & 04000u) ? 'S' : '-');
    buf[4] = (mode & 0040u) ? 'r' : '-';
    buf[5] = (mode & 0020u) ? 'w' : '-';
    buf[6] = (mode & 0010u) ? ((mode & 02000u) ? 's' : 'x') : ((mode & 02000u) ? 'S' : '-');
    buf[7] = (mode & 0004u) ? 'r' : '-';
    buf[8] = (mode & 0002u) ? 'w' : '-';
    buf[9] = (mode & 0001u) ? 'x' : '-';
    buf[10] = 0;
}
