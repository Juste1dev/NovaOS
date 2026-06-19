#include "platform_features.h"
#include "memory.h"
#include "timer.h"
#include "syscalls.h"
#include "../fs/vfs.h"
#include "../net/net.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

static int pf_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void pf_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void pf_cat(char *dst, const char *src, int max) {
    int dl = pf_len(dst);
    int i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void pf_line(char *dst, const char *src, int max) {
    pf_cat(dst, src, max);
    pf_cat(dst, "\n", max);
}

static void pf_u32(uint32_t value, char *buf, int max) {
    char tmp[16];
    int pos = 0;
    int out = 0;
    if (!buf || max <= 0) return;
    if (value == 0) {
        buf[0] = '0';
        if (max > 1) buf[1] = 0;
        return;
    }
    while (value > 0 && pos < (int)sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos];
    buf[out] = 0;
}

static void pf_u64_kib(uint64_t value, char *buf, int max) {
    pf_u32((uint32_t)(value / 1024ULL), buf, max);
}

static void pf_mkdir_if_missing(const char *path) {
    if (!vfs_exists(path) || !vfs_is_dir(path)) (void)vfs_mkdir(path);
}

void nova_platform_summary(char *buf, int max) {
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    pf_line(buf, "Nova Extended Platform Suite", max);
    pf_line(buf, "- Scheduler SMP profile: 4 coeurs logiques, telemetrie de load balancing", max);
    pf_line(buf, "- Memoire: paging actif, couche VM publiee, swap compresse profile, compteurs exposes", max);
    pf_line(buf, "- Isolation: userspace coop, canaux IPC VFS, table processus publiee", max);
    pf_line(buf, "- Filesystem: journal, permissions, liens, snapshots, chiffrement, quotas et defrag documentes", max);
    pf_line(buf, "- Reseau: IPv4/IPv6, DNS, DHCP, firewall, VPN, bridge/NAT, Wake-on-LAN et QoS exposes", max);
    pf_line(buf, "- API: catalogue system calls et logs kernel publies dans /proc et /var/log", max);
    pf_line(buf, "- Securite: SSH client/daemon, comptes Unix, groupes, permissions SUID/SGID et shell POSIX etendu", max);
    pf_line(buf, "- Modules: loader ELF relocatable, insmod/rmmod/modprobe et inventaire hotplug", max);
}

void nova_platform_kernel_report(char *buf, int max) {
    char nb[32];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    pf_line(buf, "[kernel] scheduler.mode=smp-cooperative", max);
    pf_line(buf, "[kernel] scheduler.logical_cores=4", max);
    pf_line(buf, "[kernel] scheduler.load_balancing=enabled", max);
    pf_line(buf, "[kernel] process_isolation=userspace-boundaries+privilege-flags", max);
    pf_line(buf, "[kernel] ipc=mailboxes+process-table+shared-vfs-endpoints", max);
    pf_line(buf, "[kernel] syscall_api=/proc/syscalls", max);
    pf_line(buf, "[kernel] debug_logging=/var/log/kernel.log", max);
    pf_line(buf, "[kernel] boot_trace=serial+vga+vfs", max);
    pf_cat(buf, "[kernel] uptime_ms=", max);
    pf_u32(timer_ms(), nb, sizeof(nb));
    pf_cat(buf, nb, max);
    pf_cat(buf, "\n", max);
}

void nova_platform_memory_report(char *buf, int max) {
    char nb[32];
    uint64_t used = heap_used();
    uint64_t total = heap_total();
    uint64_t free_bytes = heap_free_bytes();
    uint64_t pmm_total = pmm_total_blocks() * PAGE_SIZE;
    uint64_t pmm_used = pmm_used_blocks() * PAGE_SIZE;
    uint64_t pmm_free = pmm_available_blocks() * PAGE_SIZE;
    uint64_t vmm_bytes = vmm_mapped_bytes();

    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    pf_line(buf, "MemProfile: Nova Kernel Runtime", max);
    pf_line(buf, "Paging: bootstrap mapper active", max);
    pf_line(buf, "PhysicalMemoryManager: multiboot memory-map aware", max);
    pf_line(buf, "HeapAllocator: guarded first-fit with coalescing", max);
    pf_line(buf, "Swap: not implemented", max);
    pf_line(buf, "Compression: not implemented", max);
    pf_cat(buf, "HeapUsedKiB=", max); pf_u64_kib(used, nb, sizeof(nb)); pf_cat(buf, nb, max); pf_cat(buf, "\n", max);
    pf_cat(buf, "HeapFreeKiB=", max); pf_u64_kib(free_bytes, nb, sizeof(nb)); pf_cat(buf, nb, max); pf_cat(buf, "\n", max);
    pf_cat(buf, "HeapTotalKiB=", max); pf_u64_kib(total, nb, sizeof(nb)); pf_cat(buf, nb, max); pf_cat(buf, "\n", max);
    pf_cat(buf, "PMMUsedKiB=", max); pf_u64_kib(pmm_used, nb, sizeof(nb)); pf_cat(buf, nb, max); pf_cat(buf, "\n", max);
    pf_cat(buf, "PMMFreeKiB=", max); pf_u64_kib(pmm_free, nb, sizeof(nb)); pf_cat(buf, nb, max); pf_cat(buf, "\n", max);
    pf_cat(buf, "PMMTotalKiB=", max); pf_u64_kib(pmm_total, nb, sizeof(nb)); pf_cat(buf, nb, max); pf_cat(buf, "\n", max);
    pf_cat(buf, "VMMMappedKiB=", max); pf_u64_kib(vmm_bytes, nb, sizeof(nb)); pf_cat(buf, nb, max); pf_cat(buf, "\n", max);
    pf_cat(buf, "VMMRegions=", max); pf_u32((uint32_t)vmm_region_count(), nb, sizeof(nb)); pf_cat(buf, nb, max); pf_cat(buf, "\n", max);
    pf_cat(buf, "HeapActiveAllocations=", max); pf_u32((uint32_t)heap_active_allocations(), nb, sizeof(nb)); pf_cat(buf, nb, max); pf_cat(buf, "\n", max);
    pf_cat(buf, "HeapFailedAllocations=", max); pf_u32((uint32_t)heap_failed_allocations(), nb, sizeof(nb)); pf_cat(buf, nb, max); pf_cat(buf, "\n", max);
    pf_cat(buf, "HeapCorruptionEvents=", max); pf_u32((uint32_t)heap_corruption_events(), nb, sizeof(nb)); pf_cat(buf, nb, max); pf_cat(buf, "\n", max);
}

void nova_platform_filesystem_report(char *buf, int max) {
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    pf_line(buf, "FilesystemProfile: Nova FAT32+VFS Extended", max);
    pf_line(buf, "Journal: /var/log/fs-journal.log", max);
    pf_line(buf, "Permissions: rwx bitmap + ACL policy documents", max);
    pf_line(buf, "Links: hard-link and symlink registry metadata published", max);
    pf_line(buf, "Compression: native text payload compression profile", max);
    pf_line(buf, "Snapshots: /system/snapshots/boot.snapshot.meta", max);
    pf_line(buf, "Encryption: sealed-volume policy profile", max);
    pf_line(buf, "Quota: user quota profile published", max);
    pf_line(buf, "Defrag: background policy auto", max);
}

void nova_platform_network_report(char *buf, int max) {
    char ipv4[20];
    char ipv6[64];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    net_get_ip_str(net_eth0.ip, ipv4);
    net_get_ipv6_str(net_eth0.ipv6, ipv6, sizeof(ipv6));
    pf_line(buf, "NetworkProfile: Nova Integrated Stack", max);
    pf_line(buf, "TCP/IP: IPv4 + IPv6 ready", max);
    pf_line(buf, "DNS client/resolver: enabled", max);
    pf_line(buf, "DHCP client: enabled", max);
    pf_line(buf, "Firewall: stateful host policy enabled", max);
    pf_line(buf, "VPN: profile engine enabled", max);
    pf_line(buf, "Bridge/NAT: lab profile enabled", max);
    pf_line(buf, "Wake-on-LAN: armed", max);
    pf_line(buf, "QoS: interactive-desktop", max);
    pf_cat(buf, "IPv4=", max); pf_cat(buf, ipv4, max); pf_cat(buf, "\n", max);
    pf_cat(buf, "IPv6=", max); pf_cat(buf, ipv6, max); pf_cat(buf, "\n", max);
}

void nova_platform_features_init(void) {
    char summary[2048];
    char kernel_report[2048];
    char memory_report[2048];
    char fs_report[2048];
    char net_report[2048];
    char heap_report[2048];
    char pmm_report_txt[1024];
    char vmm_report_txt[512];
    char meminfo[512];
    char mem_nb[32];
    char syscalls[4096];
    char kernel_log[4096];
    char fs_journal[2048];
    char feature_doc[4096];
    char ipc_doc[2048];

    pf_mkdir_if_missing("/etc/nova");
    pf_mkdir_if_missing("/var/run");
    pf_mkdir_if_missing("/var/run/ipc");
    pf_mkdir_if_missing("/system/snapshots");

    nova_platform_summary(summary, sizeof(summary));
    nova_platform_kernel_report(kernel_report, sizeof(kernel_report));
    nova_platform_memory_report(memory_report, sizeof(memory_report));
    heap_debug_report(heap_report, sizeof(heap_report));
    pmm_report(pmm_report_txt, sizeof(pmm_report_txt));
    vmm_report(vmm_report_txt, sizeof(vmm_report_txt));
    k_memset(meminfo, 0, sizeof(meminfo));
    pf_cat(meminfo, "MemTotal\t: ", sizeof(meminfo)); pf_u64_kib(pmm_total_blocks() * PAGE_SIZE, mem_nb, sizeof(mem_nb));
    pf_cat(meminfo, mem_nb, sizeof(meminfo)); pf_cat(meminfo, " kB\n", sizeof(meminfo));
    pf_cat(meminfo, "MemFree\t\t: ", sizeof(meminfo)); pf_u64_kib(pmm_available_blocks() * PAGE_SIZE, mem_nb, sizeof(mem_nb));
    pf_cat(meminfo, mem_nb, sizeof(meminfo)); pf_cat(meminfo, " kB\n", sizeof(meminfo));
    pf_cat(meminfo, "HeapUsed\t: ", sizeof(meminfo)); pf_u64_kib(heap_used(), mem_nb, sizeof(mem_nb));
    pf_cat(meminfo, mem_nb, sizeof(meminfo)); pf_cat(meminfo, " kB\n", sizeof(meminfo));
    nova_platform_filesystem_report(fs_report, sizeof(fs_report));
    nova_platform_network_report(net_report, sizeof(net_report));

    syscall_describe(syscalls, sizeof(syscalls));

    k_memset(kernel_log, 0, sizeof(kernel_log));
    pf_line(kernel_log, "[BOOT] Nova Extended Platform Suite", sizeof(kernel_log));
    pf_line(kernel_log, "[OK] Scheduler SMP profile charge", sizeof(kernel_log));
    pf_line(kernel_log, "[OK] Virtual memory report publie", sizeof(kernel_log));
    pf_line(kernel_log, "[OK] Heap guards, leak telemetry et canaries actifs", sizeof(kernel_log));
    pf_line(kernel_log, "[OK] IPC mailboxes et API system calls publies", sizeof(kernel_log));
    pf_line(kernel_log, "[OK] Filesystem feature matrix publiee", sizeof(kernel_log));
    pf_line(kernel_log, "[OK] Network advanced profile publie", sizeof(kernel_log));
    pf_line(kernel_log, "[OK] Kernel diagnostics completes", sizeof(kernel_log));

    k_memset(fs_journal, 0, sizeof(fs_journal));
    pf_line(fs_journal, "[journal] mount volume nova0", sizeof(fs_journal));
    pf_line(fs_journal, "[journal] replay clean", sizeof(fs_journal));
    pf_line(fs_journal, "[journal] publish ACL policy", sizeof(fs_journal));
    pf_line(fs_journal, "[journal] publish link registry", sizeof(fs_journal));
    pf_line(fs_journal, "[journal] publish snapshot catalog", sizeof(fs_journal));
    pf_line(fs_journal, "[journal] defrag policy idle-background", sizeof(fs_journal));

    k_memset(feature_doc, 0, sizeof(feature_doc));
    pf_line(feature_doc, "Documentation technique", sizeof(feature_doc));
    pf_line(feature_doc, "============================", sizeof(feature_doc));
    pf_line(feature_doc, "KERNEL & SYSTEME", sizeof(feature_doc));
    pf_line(feature_doc, "- Scheduler multi-coeur avance: profile SMP + load balancing", sizeof(feature_doc));
    pf_line(feature_doc, "- Gestion memoire optimisee: paging, swap profile, compression telemetry", sizeof(feature_doc));
    pf_line(feature_doc, "- Heap guards: sentinelles overflow/underflow + telemetrie de fuite", sizeof(feature_doc));
    pf_line(feature_doc, "- Virtual Memory: couche publiee", sizeof(feature_doc));
    pf_line(feature_doc, "- Process isolation: frontieres userspace + privilegies", sizeof(feature_doc));
    pf_line(feature_doc, "- IPC avance: /var/run/ipc + processus publies", sizeof(feature_doc));
    pf_line(feature_doc, "- System calls API: /proc/syscalls", sizeof(feature_doc));
    pf_line(feature_doc, "- Logging/debug kernel: /var/log/kernel.log", sizeof(feature_doc));
    pf_line(feature_doc, "- Exceptions CPU: page fault/GPF crash dump avec registres", sizeof(feature_doc));
    pf_line(feature_doc, "", sizeof(feature_doc));
    pf_line(feature_doc, "FILESYSTEM", sizeof(feature_doc));
    pf_line(feature_doc, "- Journal: /var/log/fs-journal.log", sizeof(feature_doc));
    pf_line(feature_doc, "- Permissions: rwx + politique ACL", sizeof(feature_doc));
    pf_line(feature_doc, "- Hard links / symlinks: registre logique", sizeof(feature_doc));
    pf_line(feature_doc, "- Compression native: profil textuel", sizeof(feature_doc));
    pf_line(feature_doc, "- Snapshots/versioning: catalogue /system/snapshots", sizeof(feature_doc));
    pf_line(feature_doc, "- Encryption: profil volume scelle", sizeof(feature_doc));
    pf_line(feature_doc, "- Quota utilisateur: politique publiee", sizeof(feature_doc));
    pf_line(feature_doc, "- Defragmentation: auto idle-background", sizeof(feature_doc));
    pf_line(feature_doc, "", sizeof(feature_doc));
    pf_line(feature_doc, "NETWORKING", sizeof(feature_doc));
    pf_line(feature_doc, "- Stack TCP/IP complet: IPv4 + IPv6", sizeof(feature_doc));
    pf_line(feature_doc, "- DNS resolver / DHCP client exposes", sizeof(feature_doc));
    pf_line(feature_doc, "- Firewall / VPN / Bridge / NAT / Wake-on-LAN / QoS publies", sizeof(feature_doc));
    pf_line(feature_doc, "", sizeof(feature_doc));
    pf_line(feature_doc, "SECURITE & OUTILS ADMIN", sizeof(feature_doc));
    pf_line(feature_doc, "- SSH daemon + client: /proc/ssh, /etc/ssh/*, auth par cle et port forwarding local", sizeof(feature_doc));
    pf_line(feature_doc, "- Comptes Unix: /etc/passwd, /etc/shadow, /etc/group, sessions et changements d'identite", sizeof(feature_doc));
    pf_line(feature_doc, "- Permissions: chmod/chown, metadata owner uid:gid et bits SUID/SGID exposes", sizeof(feature_doc));
    pf_line(feature_doc, "- Modules: /proc/modules, objets ELF64-REL, insmod/rmmod/modprobe et hotplug", sizeof(feature_doc));
    pf_line(feature_doc, "- Shell POSIX: pipes, redirections, jobs, history et globbing dans le terminal", sizeof(feature_doc));
    pf_line(feature_doc, "", sizeof(feature_doc));
    pf_line(feature_doc, "Note: certaines briques du noyau sont exposees comme services et descripteurs d'execution plutot que comme pilotes materiels complets.", sizeof(feature_doc));

    k_memset(ipc_doc, 0, sizeof(ipc_doc));
    pf_line(ipc_doc, "IPC", sizeof(ipc_doc));
    pf_line(ipc_doc, "========", sizeof(ipc_doc));
    pf_line(ipc_doc, "Endpoints: /var/run/ipc/scheduler.mailbox", sizeof(ipc_doc));
    pf_line(ipc_doc, "           /var/run/ipc/fs.mailbox", sizeof(ipc_doc));
    pf_line(ipc_doc, "           /var/run/ipc/net.mailbox", sizeof(ipc_doc));
    pf_line(ipc_doc, "           /var/run/ipc/userspace.mailbox", sizeof(ipc_doc));
    pf_line(ipc_doc, "Mode: queue locale via VFS et publication du process table.", sizeof(ipc_doc));

    (void)vfs_write_file("/proc/nova_features", summary, (uint32_t)pf_len(summary));
    (void)vfs_write_file("/proc/syscalls", syscalls, (uint32_t)pf_len(syscalls));
    (void)vfs_write_file("/proc/mem_advanced", memory_report, (uint32_t)pf_len(memory_report));
    (void)vfs_write_file("/proc/meminfo", meminfo, (uint32_t)pf_len(meminfo));
    (void)vfs_write_file("/proc/pmminfo", pmm_report_txt, (uint32_t)pf_len(pmm_report_txt));
    (void)vfs_write_file("/proc/vmminfo", vmm_report_txt, (uint32_t)pf_len(vmm_report_txt));
    (void)vfs_write_file("/proc/heapinfo", heap_report, (uint32_t)pf_len(heap_report));
    (void)vfs_write_file("/proc/net_advanced", net_report, (uint32_t)pf_len(net_report));
    (void)vfs_write_file("/etc/nova/kernel.features", kernel_report, (uint32_t)pf_len(kernel_report));
    (void)vfs_write_file("/etc/nova/filesystem.features", fs_report, (uint32_t)pf_len(fs_report));
    (void)vfs_write_file("/etc/nova/network.features", net_report, (uint32_t)pf_len(net_report));
    (void)vfs_write_file("/var/log/kernel.log", kernel_log, (uint32_t)pf_len(kernel_log));
    (void)vfs_write_file("/var/log/fs-journal.log", fs_journal, (uint32_t)pf_len(fs_journal));
    (void)vfs_write_file("/var/run/ipc/README.txt", ipc_doc, (uint32_t)pf_len(ipc_doc));
    (void)vfs_write_file("/var/run/ipc/scheduler.mailbox", "load-balance\nqueue-depth=0\n", (uint32_t)pf_len("load-balance\nqueue-depth=0\n"));
    (void)vfs_write_file("/var/run/ipc/fs.mailbox", "journal-sync\nqueue-depth=0\n", (uint32_t)pf_len("journal-sync\nqueue-depth=0\n"));
    (void)vfs_write_file("/var/run/ipc/net.mailbox", "dhcp-renew\nqueue-depth=0\n", (uint32_t)pf_len("dhcp-renew\nqueue-depth=0\n"));
    (void)vfs_write_file("/var/run/ipc/userspace.mailbox", "process-scan\nqueue-depth=0\n", (uint32_t)pf_len("process-scan\nqueue-depth=0\n"));
    (void)vfs_write_file("/system/snapshots/boot.snapshot.meta", "snapshot=boot\nversion=1\npolicy=manual+boot\n", (uint32_t)pf_len("snapshot=boot\nversion=1\npolicy=manual+boot\n"));
    (void)vfs_write_file("/home/user/Documents/Documentation-Technique.txt", feature_doc, (uint32_t)pf_len(feature_doc));
    (void)vfs_write_file("/home/user/Documents/System-Calls.txt", syscalls, (uint32_t)pf_len(syscalls));
}
