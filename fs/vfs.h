
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_MAX_FILES     256
#define VFS_MAX_DIRS      64
#define VFS_MAX_PATH      256
#define VFS_MAX_NAME      64
#define VFS_MAX_DATA      8192
#define VFS_MAX_FD        32

typedef enum {
    VFS_TYPE_FILE = 0,
    VFS_TYPE_DIR,
    VFS_TYPE_DEVICE
} vfs_node_type_t;

typedef struct vfs_node {
    char             name[VFS_MAX_NAME];
    char             path[VFS_MAX_PATH];
    vfs_node_type_t  type;
    uint32_t         size;
    uint32_t         created;
    uint32_t         modified;
    uint8_t          data[VFS_MAX_DATA];
    int              parent_idx;
    int              valid;
    uint32_t         permissions;
    uint32_t         owner_uid;
    uint32_t         owner_gid;
} vfs_node_t;

typedef struct {
    int      node_idx;
    uint32_t offset;
    int      flags;
    int      valid;
} vfs_fd_t;

void  vfs_init(void);
int   vfs_mkdir(const char *path);
int   vfs_create(const char *path, const char *data, uint32_t len);
int   vfs_open(const char *path, int flags);
void  vfs_close(int fd);
int   vfs_read(int fd, uint8_t *buf, uint32_t len);
int   vfs_write(int fd, const uint8_t *buf, uint32_t len);
int   vfs_exists(const char *path);
int   vfs_delete(const char *path);
int   vfs_list(const char *dir, char names[][VFS_MAX_NAME], int *types, int max);
vfs_node_t* vfs_get_node(const char *path);
uint32_t vfs_get_size(const char *path);
void  vfs_get_contents(const char *path, char *buf, uint32_t maxlen);
int   vfs_write_file(const char *path, const char *data, uint32_t len);
int   vfs_set_permissions(const char *path, uint32_t mode);
int   vfs_set_owner(const char *path, uint32_t uid, uint32_t gid);
void  vfs_format_mode(const char *path, char *buf, int max);

extern vfs_node_t vfs_nodes[VFS_MAX_FILES];
extern int        vfs_node_count;

static inline int vfs_list_dir(const char *path, char entries[][256], int *is_dir, int max) {
    char names[64][VFS_MAX_NAME];
    int  types[64];
    int n = max < 64 ? max : 64;
    int count = vfs_list(path, names, types, n);
    if (count < 0) return count;
    for (int i = 0; i < count; i++) {
        int j = 0;
        while (names[i][j] && j < 255) { entries[i][j] = names[i][j]; j++; }
        entries[i][j] = 0;
        is_dir[i] = (types[i] == (int)VFS_TYPE_DIR);
    }
    return count;
}

static inline int vfs_is_dir(const char *path) {
    vfs_node_t *n = vfs_get_node(path);
    return n && n->valid && n->type == VFS_TYPE_DIR;
}

#endif
