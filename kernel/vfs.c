/*
 * vfs.c — Virtual File System layer
 *
 * Provides path resolution, a node operation dispatch layer, and a global
 * file descriptor table.  The actual filesystem logic lives in drivers
 * (e.g. ramfs.c) that populate the vtable pointers on each vfs_node_t.
 */

#include "include/kernel/vfs.h"
#include "include/kernel/task.h"
#include "include/kernel/kheap.h"
#include <string.h>
#include <stddef.h>

/* ---- Global state ------------------------------------------------------ */

static vfs_node_t* vfs_root = NULL;

#define VFS_MAX_MOUNTS 8

typedef struct {
    char path[VFS_PATH_MAX];
    vfs_node_t* root;
    uint8_t in_use;
} vfs_mount_entry_t;

static vfs_mount_entry_t vfs_mounts[VFS_MAX_MOUNTS];

static uint32_t vfs_effective_uid(void) {
    task_t* t = task_current();
    return t ? t->uid : 0u;
}

static uint32_t vfs_effective_gid(void) {
    task_t* t = task_current();
    return t ? t->gid : 0u;
}

static int vfs_perm_triplet(uint8_t perm, uint16_t* owner, uint16_t* group, uint16_t* other) {
    if (!owner || !group || !other) return -1;
    if (perm == VFS_MODE_IROTH) {
        *owner = VFS_MODE_IRUSR;
        *group = VFS_MODE_IRGRP;
        *other = VFS_MODE_IROTH;
        return 0;
    }
    if (perm == VFS_MODE_IWOTH) {
        *owner = VFS_MODE_IWUSR;
        *group = VFS_MODE_IWGRP;
        *other = VFS_MODE_IWOTH;
        return 0;
    }
    if (perm == VFS_MODE_IXOTH) {
        *owner = VFS_MODE_IXUSR;
        *group = VFS_MODE_IXGRP;
        *other = VFS_MODE_IXOTH;
        return 0;
    }
    return -1;
}

int vfs_node_allows(const vfs_node_t* node, uint8_t perm) {
    if (!node) return 0;
    if (node->mode == 0) return 1; /* backwards-compatible default */

    uint16_t owner_bit = 0;
    uint16_t group_bit = 0;
    uint16_t other_bit = 0;
    if (vfs_perm_triplet(perm, &owner_bit, &group_bit, &other_bit) < 0) return 0;

    uint32_t uid = vfs_effective_uid();
    uint32_t gid = vfs_effective_gid();

    if (uid == 0u) return 1;
    if (uid == node->uid) return (node->mode & owner_bit) != 0;
    if (gid == node->gid) return (node->mode & group_bit) != 0;
    return (node->mode & other_bit) != 0;
}

static int vfs_name_is_valid(const char* name) {
    if (!name || *name == '\0') return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    for (size_t i = 0; name[i]; ++i) {
        if (name[i] == '/') return 0;
    }
    return 1;
}

static vfs_node_t* vfs_mount_lookup_by_path(const char* path, const char** out_rest) {
    size_t best_len = 0;
    vfs_node_t* best = NULL;

    for (size_t i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (!vfs_mounts[i].in_use || !vfs_mounts[i].root) continue;
        const char* mpath = vfs_mounts[i].path;
        size_t mlen = strlen(mpath);
        if (mlen == 0) continue;

        if (strcmp(path, mpath) == 0 ||
            (strncmp(path, mpath, mlen) == 0 && path[mlen] == '/')) {
            if (mlen > best_len) {
                best_len = mlen;
                best = vfs_mounts[i].root;
            }
        }
    }

    if (best) {
        if (out_rest) {
            if (path[best_len] == '\0') *out_rest = "";
            else *out_rest = path + best_len + 1;
        }
        return best;
    }

    if (out_rest) {
        *out_rest = NULL;
    }
    return NULL;
}

static vfs_node_t* vfs_mount_lookup_start(const char* path, const char** out_rest) {
    vfs_node_t* mounted = vfs_mount_lookup_by_path(path, out_rest);
    if (mounted) {
        return mounted;
    }

    if (out_rest) {
        *out_rest = (*path == '/') ? path + 1 : path;
    }
    return vfs_root;
}

/* ---- Initialisation ---------------------------------------------------- */

void vfs_initialize(void) {
    vfs_root = NULL;
    memset(vfs_mounts, 0, sizeof(vfs_mounts));
}

vfs_node_t* vfs_get_root(void) {
    return vfs_root;
}

void vfs_set_root(vfs_node_t* root) {
    vfs_root = root;
}

/* ---- Path resolution --------------------------------------------------- */

/*
 * vfs_namei — resolve an absolute path (e.g. "/dir/file") to a vfs_node_t*.
 *
 * Rules:
 *   • Leading '/' is stripped → resolution always starts at root.
 *   • Components are split on '/'.
 *   • Each component is looked up via the current node's finddir callback.
 *   • Returns NULL if any component is not found or root is unset.
 */
int vfs_normalize_path(const char* cwd, const char* path, char* out, size_t out_size) {
    if (!out || out_size < 2) return -1;

    char input[VFS_PATH_MAX];
    const char* src = path;

    if (!src || *src == '\0') {
        src = (cwd && *cwd) ? cwd : "/";
    }

    if (src[0] == '/') {
        strncpy(input, src, sizeof(input) - 1);
        input[sizeof(input) - 1] = '\0';
    } else {
        const char* base = (cwd && cwd[0] == '/') ? cwd : "/";
        size_t base_len = strlen(base);
        size_t src_len = strlen(src);
        if (base_len + 1 + src_len + 1 > sizeof(input)) return -1;
        strcpy(input, base);
        if (base_len == 0 || input[base_len - 1] != '/') {
            input[base_len] = '/';
            input[base_len + 1] = '\0';
        }
        strncat(input, src, sizeof(input) - strlen(input) - 1);
    }

    size_t out_len = 1;
    out[0] = '/';
    out[1] = '\0';

    size_t i = 0;
    while (input[i]) {
        while (input[i] == '/') i++;
        if (!input[i]) break;

        size_t start = i;
        while (input[i] && input[i] != '/') i++;
        size_t len = i - start;
        if (len == 0) continue;

        if (len == 1 && input[start] == '.') {
            continue;
        }

        if (len == 2 && input[start] == '.' && input[start + 1] == '.') {
            if (out_len > 1) {
                while (out_len > 1 && out[out_len - 1] != '/') out_len--;
                if (out_len > 1) out_len--;
            }
            out[out_len] = '\0';
            continue;
        }

        if (len >= VFS_NAME_MAX) return -1;
        if (out_len + len + 1 >= out_size) return -1;

        if (out_len > 1) out[out_len++] = '/';
        memcpy(out + out_len, input + start, len);
        out_len += len;
        out[out_len] = '\0';
    }

    if (out_len == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return 0;
}

vfs_node_t* vfs_namei(const char* path) {
    if (!path || !vfs_root) return NULL;

    char normalized[VFS_PATH_MAX];
    if (vfs_normalize_path("/", path, normalized, sizeof(normalized)) < 0) return NULL;
    if (strcmp(normalized, "/") == 0) return vfs_root;

    const char* remainder = NULL;
    vfs_node_t* cur = vfs_mount_lookup_start(normalized, &remainder);
    if (!cur) return NULL;
    if (!remainder || *remainder == '\0') return cur;

    char component[VFS_NAME_MAX];
    char built_path[VFS_PATH_MAX];
    built_path[0] = '\0';

    while (*remainder) {
        /* Skip slashes */
        while (*remainder == '/') remainder++;
        if (*remainder == '\0') break;

        /* Extract next component */
        size_t len = 0;
        while (remainder[len] && remainder[len] != '/' && len < VFS_NAME_MAX - 1) {
            component[len] = remainder[len];
            len++;
        }
        component[len] = '\0';
        remainder += len;

        /* Build the path as we traverse */
        if (strlen(built_path) + 1 + len < sizeof(built_path)) {
            if (strlen(built_path) == 0) {
                strcpy(built_path, "/");
            }
            if (built_path[strlen(built_path) - 1] != '/') {
                strcat(built_path, "/");
            }
            strncat(built_path, component, sizeof(built_path) - strlen(built_path) - 1);
        }

        /* Traversal requires search/execute permission on each directory. */
        if (!(cur->type & VFS_DIRECTORY) || !cur->finddir) return NULL;
        if (!vfs_node_allows(cur, VFS_MODE_IXOTH)) return NULL;

        cur = cur->finddir(cur, component);
        if (!cur) return NULL;

        /* Check if we've hit a mount point */
        const char* mount_remainder = NULL;
        vfs_node_t* mounted = vfs_mount_lookup_by_path(built_path, &mount_remainder);
        if (mounted) {
            cur = mounted;
            if (mount_remainder && *mount_remainder != '\0') {
                remainder = mount_remainder;
            } else {
                remainder = (remainder && *remainder == '/') ? remainder + 1 : "";
            }
        }
    }

    return cur;
}

/* ---- Helper: extract parent path + basename from a full path ----------- */

/*
 * Given "/foo/bar/baz", sets parent_path="/foo/bar" and basename="baz".
 * Given "/file", sets parent_path="/" and basename="file".
 */
int vfs_split_path(const char* path, char* parent_path, size_t pp_size,
                   char* basename, size_t bn_size) {
    if (!path || !parent_path || !basename || pp_size == 0 || bn_size == 0) return -1;

    char normalized[VFS_PATH_MAX];
    if (vfs_normalize_path("/", path, normalized, sizeof(normalized)) < 0) return -1;
    if (strcmp(normalized, "/") == 0) return -1;

    const char* last_slash = strrchr(normalized, '/');
    if (!last_slash || !last_slash[1]) return -1;

    strncpy(basename, last_slash + 1, bn_size - 1);
    basename[bn_size - 1] = '\0';
    if (!vfs_name_is_valid(basename)) return -1;

    if (last_slash == normalized) {
        strncpy(parent_path, "/", pp_size - 1);
        parent_path[pp_size - 1] = '\0';
        return 0;
    }

    size_t plen = (size_t)(last_slash - normalized);
    if (plen >= pp_size) return -1;
    memcpy(parent_path, normalized, plen);
    parent_path[plen] = '\0';
    return 0;
}

int vfs_mount(const char* mount_path, vfs_node_t* root_node) {
    if (!mount_path || !root_node) return -1;

    char normalized[VFS_PATH_MAX];
    if (vfs_normalize_path("/", mount_path, normalized, sizeof(normalized)) < 0) return -1;

    if (strcmp(normalized, "/") == 0) {
        vfs_set_root(root_node);
        return 0;
    }

    for (size_t i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (!vfs_mounts[i].in_use) continue;
        if (strcmp(vfs_mounts[i].path, normalized) == 0) {
            vfs_mounts[i].root = root_node;
            return 0;
        }
    }

    for (size_t i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (vfs_mounts[i].in_use) continue;
        strncpy(vfs_mounts[i].path, normalized, sizeof(vfs_mounts[i].path) - 1);
        vfs_mounts[i].path[sizeof(vfs_mounts[i].path) - 1] = '\0';
        vfs_mounts[i].root = root_node;
        vfs_mounts[i].in_use = 1;
        return 0;
    }

    return -1;
}

/* ---- Anonymous pipes ---------------------------------------------------- */

#define VFS_PIPE_CAPACITY 1024u

typedef struct {
    uint8_t  buffer[VFS_PIPE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t size;
    uint32_t readers;
    uint32_t writers;
} vfs_pipe_t;

typedef struct {
    vfs_pipe_t* pipe;
    uint8_t     is_writer;
    uint32_t    refs;
} vfs_pipe_end_t;

static int pipe_open(vfs_node_t* node, uint32_t flags) {
    (void)flags;
    if (!node || !node->impl) return -1;
    vfs_pipe_end_t* end = (vfs_pipe_end_t*)node->impl;
    end->refs++;
    if (end->is_writer) end->pipe->writers++;
    else end->pipe->readers++;
    return 0;
}

static void pipe_close(vfs_node_t* node) {
    if (!node || !node->impl) return;
    vfs_pipe_end_t* end = (vfs_pipe_end_t*)node->impl;
    vfs_pipe_t* pipe = end->pipe;

    if (end->is_writer) {
        if (pipe->writers > 0) pipe->writers--;
    } else {
        if (pipe->readers > 0) pipe->readers--;
    }

    if (end->refs > 0) end->refs--;

    if (end->refs == 0) {
        node->impl = NULL;
        kfree(end);
        kfree(node);

        if (pipe->readers == 0 && pipe->writers == 0) {
            kfree(pipe);
        }
    }
}

static int32_t pipe_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    (void)offset;
    if (!node || !node->impl || !buffer) return -1;

    vfs_pipe_end_t* end = (vfs_pipe_end_t*)node->impl;
    if (end->is_writer) return -1;

    vfs_pipe_t* pipe = end->pipe;
    uint32_t out = 0;

    while (out < size) {
        if (pipe->size == 0) {
            if (pipe->writers == 0) break; /* EOF */
            task_sleep_ticks(1);
            continue;
        }

        buffer[out++] = pipe->buffer[pipe->head];
        pipe->head = (pipe->head + 1u) % VFS_PIPE_CAPACITY;
        pipe->size--;
    }

    return (int32_t)out;
}

static int32_t pipe_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    (void)offset;
    if (!node || !node->impl || !buffer) return -1;

    vfs_pipe_end_t* end = (vfs_pipe_end_t*)node->impl;
    if (!end->is_writer) return -1;

    vfs_pipe_t* pipe = end->pipe;
    uint32_t written = 0;

    while (written < size) {
        if (pipe->readers == 0) return (written > 0) ? (int32_t)written : -1;

        if (pipe->size >= VFS_PIPE_CAPACITY) {
            task_sleep_ticks(1);
            continue;
        }

        pipe->buffer[pipe->tail] = buffer[written++];
        pipe->tail = (pipe->tail + 1u) % VFS_PIPE_CAPACITY;
        pipe->size++;
    }

    return (int32_t)written;
}

static vfs_node_t* pipe_make_end_node(vfs_pipe_t* pipe, uint8_t is_writer) {
    if (!pipe) return NULL;

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    vfs_pipe_end_t* end = (vfs_pipe_end_t*)kmalloc(sizeof(vfs_pipe_end_t));
    if (!node || !end) {
        if (node) kfree(node);
        if (end) kfree(end);
        return NULL;
    }

    memset(node, 0, sizeof(vfs_node_t));
    memset(end, 0, sizeof(vfs_pipe_end_t));
    strncpy(node->name, is_writer ? "pipew" : "piper", VFS_NAME_MAX - 1);
    node->type = VFS_FILE;
    node->mode = VFS_MODE_FILE_DEFAULT;
    node->uid = vfs_effective_uid();
    node->gid = vfs_effective_gid();
    node->read = pipe_read;
    node->write = pipe_write;
    node->open = pipe_open;
    node->close = pipe_close;

    end->pipe = pipe;
    end->is_writer = is_writer;
    node->impl = end;
    return node;
}

/* ---- Node-level operations --------------------------------------------- */

int32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !node->read) return -1;
    return node->read(node, offset, size, buffer);
}

int32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    if (!node || !node->write) return -1;
    return node->write(node, offset, size, buffer);
}

int vfs_open_node(vfs_node_t* node, uint32_t flags) {
    if (!node) return -1;

    uint32_t access = flags & (VFS_O_WRONLY | VFS_O_RDWR);
    int wants_read = (access != VFS_O_WRONLY);
    int wants_write = (access == VFS_O_WRONLY || access == VFS_O_RDWR);

    if ((node->type & VFS_DIRECTORY) &&
        (wants_write || (flags & (VFS_O_TRUNC | VFS_O_APPEND)))) {
        return -1;
    }

    if (wants_read && !vfs_node_allows(node, VFS_MODE_IROTH)) return -1;
    if (wants_write && !vfs_node_allows(node, VFS_MODE_IWOTH)) return -1;

    if (node->open) return node->open(node, flags);
    return 0; /* success by default */
}

void vfs_close_node(vfs_node_t* node) {
    if (!node) return;
    if (node->close) node->close(node);
}

int vfs_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out) {
    if (!node || !node->readdir) return -1;
    return node->readdir(node, index, out);
}

vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name) {
    if (!node || !node->finddir) return NULL;
    return node->finddir(node, name);
}

int vfs_create(vfs_node_t* parent, const char* name, uint32_t type) {
    if (!vfs_name_is_valid(name)) return -1;
    if (!parent || !parent->create) return -1;
    if (!(parent->type & VFS_DIRECTORY)) return -1;
    if (!vfs_node_allows(parent, VFS_MODE_IWOTH) || !vfs_node_allows(parent, VFS_MODE_IXOTH)) return -1;
    return parent->create(parent, name, type);
}

int vfs_unlink(vfs_node_t* parent, const char* name) {
    if (!vfs_name_is_valid(name)) return -1;
    if (!parent || !parent->unlink) return -1;
    if (!(parent->type & VFS_DIRECTORY)) return -1;
    if (!vfs_node_allows(parent, VFS_MODE_IWOTH) || !vfs_node_allows(parent, VFS_MODE_IXOTH)) return -1;
    return parent->unlink(parent, name);
}

int vfs_create_path(const char* path, uint32_t type) {
    char parent_path[VFS_PATH_MAX];
    char base_name[VFS_NAME_MAX];
    if (vfs_split_path(path, parent_path, sizeof(parent_path), base_name, sizeof(base_name)) < 0) return -1;

    vfs_node_t* parent = vfs_namei(parent_path);
    if (!parent) return -1;
    return vfs_create(parent, base_name, type);
}

int vfs_unlink_path(const char* path) {
    char parent_path[VFS_PATH_MAX];
    char base_name[VFS_NAME_MAX];
    if (vfs_split_path(path, parent_path, sizeof(parent_path), base_name, sizeof(base_name)) < 0) return -1;

    vfs_node_t* parent = vfs_namei(parent_path);
    if (!parent) return -1;
    return vfs_unlink(parent, base_name);
}

int vfs_chmod_path(const char* path, uint16_t mode) {
    if (!path) return -1;
    vfs_node_t* node = vfs_namei(path);
    if (!node) return -1;

    uint32_t uid = vfs_effective_uid();
    if (uid != 0u && uid != node->uid) return -1;

    node->mode = (uint16_t)(mode & 0777u);
    return 0;
}

int vfs_chown_path(const char* path, uint32_t uid, uint32_t gid) {
    if (!path) return -1;
    vfs_node_t* node = vfs_namei(path);
    if (!node) return -1;

    if (vfs_effective_uid() != 0u) return -1;

    node->uid = uid;
    node->gid = gid;
    return 0;
}

int vfs_stat(vfs_node_t* node, vfs_stat_t* st) {
    if (!node || !st) return -1;
    st->inode = node->inode;
    st->type  = node->type;
    st->size  = node->size;
    st->mode  = node->mode;
    st->uid   = node->uid;
    st->gid   = node->gid;
    return 0;
}

/* ---- File-descriptor API ----------------------------------------------- */

static int fd_alloc(void) {
    task_t* t = task_current();
    if (!t) return -1;

    /* Reserve 0,1,2 for stdin/stdout/stderr used by syscall layer. */
    for (int i = 3; i < TASK_MAX_FDS; ++i) {
        if (!t->fds[i].in_use) return i;
    }
    return -1;  /* no free fds */
}

int vfs_fd_open(const char* path, uint32_t flags) {
    if (!path) return -1;
    task_t* t = task_current();
    if (!t) return -1;

    char normalized[VFS_PATH_MAX];
    if (vfs_normalize_path("/", path, normalized, sizeof(normalized)) < 0) return -1;

    uint32_t open_flags = flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREATE |
                                   VFS_O_TRUNC | VFS_O_APPEND);
    uint32_t fd_flags = (flags & VFS_O_CLOEXEC) ? VFS_FD_CLOEXEC : 0;

    vfs_node_t* node = vfs_namei(normalized);

    /* Create if requested and doesn't exist */
    if (!node && (open_flags & VFS_O_CREATE)) {
        if (vfs_create_path(normalized, VFS_FILE) < 0) return -1;
        node = vfs_namei(normalized);
        if (!node) return -1;
    }

    if (!node) return -1;

    if (vfs_open_node(node, open_flags) < 0)
        return -1;

    int fd = fd_alloc();
    if (fd < 0) {
        vfs_close_node(node);
        return -1;
    }

    t->fds[fd].node = node;
    t->fds[fd].offset = (open_flags & VFS_O_APPEND) ? node->size : 0;
    t->fds[fd].open_flags = open_flags;
    t->fds[fd].fd_flags = fd_flags;
    t->fds[fd].in_use = 1;


    return fd;
}

int32_t vfs_fd_read(int fd, uint8_t* buffer, uint32_t size) {
    task_t* t = task_current();
    if (!t) return -1;
    if (fd < 0 || fd >= TASK_MAX_FDS || !t->fds[fd].in_use) return -1;

    task_fd_t* f = &t->fds[fd];
    if ((f->open_flags & VFS_O_WRONLY) == VFS_O_WRONLY) return -1;
    if (!vfs_node_allows(f->node, VFS_MODE_IROTH)) return -1;
    int32_t n = vfs_read(f->node, f->offset, size, buffer);
    if (n > 0) f->offset += (uint32_t)n;
    return n;
}

int32_t vfs_fd_write(int fd, const uint8_t* buffer, uint32_t size) {
    task_t* t = task_current();
    if (!t) return -1;
    if (fd < 0 || fd >= TASK_MAX_FDS || !t->fds[fd].in_use) return -1;

    task_fd_t* f = &t->fds[fd];
    uint32_t access = f->open_flags & (VFS_O_WRONLY | VFS_O_RDWR);
    if (!(access == VFS_O_WRONLY || access == VFS_O_RDWR)) return -1;
    if (!vfs_node_allows(f->node, VFS_MODE_IWOTH)) return -1;
    int32_t n = vfs_write(f->node, f->offset, size, buffer);
    if (n > 0) f->offset += (uint32_t)n;
    return n;
}

int vfs_fd_close(int fd) {
    task_t* t = task_current();
    if (!t) return -1;
    if (fd < 0 || fd >= TASK_MAX_FDS || !t->fds[fd].in_use) return -1;

    vfs_close_node(t->fds[fd].node);
    t->fds[fd].node = NULL;
    t->fds[fd].offset = 0;
    t->fds[fd].open_flags = 0;
    t->fds[fd].fd_flags = 0;
    t->fds[fd].in_use = 0;
    return 0;
}

int vfs_fd_stat(int fd, vfs_stat_t* st) {
    task_t* t = task_current();
    if (!t) return -1;
    if (fd < 0 || fd >= TASK_MAX_FDS || !t->fds[fd].in_use) return -1;
    return vfs_stat(t->fds[fd].node, st);
}

int vfs_fd_fcntl(int fd, uint32_t cmd, uint32_t arg) {
    task_t* t = task_current();
    if (!t) return -1;
    if (fd < 0 || fd >= TASK_MAX_FDS || !t->fds[fd].in_use) return -1;

    task_fd_t* f = &t->fds[fd];
    switch (cmd) {
        case VFS_F_GETFD:
            return (int)(f->fd_flags & VFS_FD_CLOEXEC);
        case VFS_F_SETFD:
            if (arg & VFS_FD_CLOEXEC) {
                f->fd_flags |= TASK_FD_CLOEXEC;
            } else {
                f->fd_flags &= ~TASK_FD_CLOEXEC;
            }
            return 0;
        default:
            return -1;
    }
}

int vfs_fd_pipe(int out_fds[2]) {
    if (!out_fds) return -1;

    task_t* t = task_current();
    if (!t) return -1;

    int read_fd = fd_alloc();
    if (read_fd < 0) return -1;
    t->fds[read_fd].in_use = 1; /* reserve slot */

    int write_fd = fd_alloc();
    if (write_fd < 0) {
        t->fds[read_fd].in_use = 0;
        return -1;
    }

    vfs_pipe_t* pipe = (vfs_pipe_t*)kmalloc(sizeof(vfs_pipe_t));
    if (!pipe) {
        t->fds[read_fd].in_use = 0;
        t->fds[write_fd].in_use = 0;
        return -1;
    }
    memset(pipe, 0, sizeof(vfs_pipe_t));

    vfs_node_t* read_node = pipe_make_end_node(pipe, 0);
    vfs_node_t* write_node = pipe_make_end_node(pipe, 1);
    if (!read_node || !write_node) {
        if (read_node) kfree(read_node);
        if (write_node) kfree(write_node);
        kfree(pipe);
        t->fds[read_fd].in_use = 0;
        t->fds[write_fd].in_use = 0;
        return -1;
    }

    t->fds[read_fd].node = read_node;
    t->fds[read_fd].offset = 0;
    t->fds[read_fd].open_flags = VFS_O_RDONLY;
    t->fds[read_fd].fd_flags = 0;
    t->fds[read_fd].in_use = 1;

    t->fds[write_fd].node = write_node;
    t->fds[write_fd].offset = 0;
    t->fds[write_fd].open_flags = VFS_O_WRONLY;
    t->fds[write_fd].fd_flags = 0;
    t->fds[write_fd].in_use = 1;

    if (vfs_open_node(read_node, VFS_O_RDONLY) < 0 ||
        vfs_open_node(write_node, VFS_O_WRONLY) < 0) {
        vfs_fd_close(read_fd);
        vfs_fd_close(write_fd);
        return -1;
    }

    out_fds[0] = read_fd;
    out_fds[1] = write_fd;
    return 0;
}

