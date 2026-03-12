/*
 * vfs.c — Virtual File System layer
 *
 * Provides path resolution, a node operation dispatch layer, and a global
 * file descriptor table.  The actual filesystem logic lives in drivers
 * (e.g. ramfs.c) that populate the vtable pointers on each vfs_node_t.
 */

#include "include/kernel/vfs.h"
#include "include/kernel/log.h"
#include <string.h>
#include <stddef.h>

/* ---- Global state ------------------------------------------------------ */

static vfs_node_t* vfs_root = NULL;
static vfs_fd_t    fd_table[VFS_MAX_FDS];

/* ---- Initialisation ---------------------------------------------------- */

void vfs_initialize(void) {
    vfs_root = NULL;
    for (int i = 0; i < VFS_MAX_FDS; ++i) {
        fd_table[i].node   = NULL;
        fd_table[i].offset = 0;
        fd_table[i].flags  = 0;
        fd_table[i].in_use = 0;
    }
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
vfs_node_t* vfs_namei(const char* path) {
    if (!path || !vfs_root) return NULL;

    /* "/" alone → root */
    while (*path == '/') path++;
    if (*path == '\0') return vfs_root;

    vfs_node_t* cur = vfs_root;
    char component[VFS_NAME_MAX];

    while (*path) {
        /* Skip slashes */
        while (*path == '/') path++;
        if (*path == '\0') break;

        /* Extract next component */
        size_t len = 0;
        while (path[len] && path[len] != '/' && len < VFS_NAME_MAX - 1) {
            component[len] = path[len];
            len++;
        }
        component[len] = '\0';
        path += len;

        /* Current node must be a directory with finddir */
        if (!(cur->type & VFS_DIRECTORY) || !cur->finddir) return NULL;

        cur = cur->finddir(cur, component);
        if (!cur) return NULL;
    }

    return cur;
}

/* ---- Helper: extract parent path + basename from a full path ----------- */

/*
 * Given "/foo/bar/baz", sets parent_path="/foo/bar" and basename="baz".
 * Given "/file", sets parent_path="/" and basename="file".
 */
static int split_path(const char* path, char* parent_path, size_t pp_size,
                      char* basename, size_t bn_size)
{
    if (!path || !parent_path || !basename) return -1;

    size_t plen = strlen(path);
    if (plen == 0) return -1;

    /* Find last '/' */
    const char* last_slash = NULL;
    for (size_t i = 0; i < plen; ++i) {
        if (path[i] == '/') last_slash = &path[i];
    }

    if (!last_slash) {
        /* No slash → parent is root */
        strncpy(parent_path, "/", pp_size);
        strncpy(basename, path, bn_size);
    } else if (last_slash == path) {
        /* Slash is at position 0, e.g. "/file" */
        strncpy(parent_path, "/", pp_size);
        strncpy(basename, last_slash + 1, bn_size);
    } else {
        size_t dir_len = (size_t)(last_slash - path);
        if (dir_len >= pp_size) dir_len = pp_size - 1;
        memcpy(parent_path, path, dir_len);
        parent_path[dir_len] = '\0';
        strncpy(basename, last_slash + 1, bn_size);
    }

    basename[bn_size - 1] = '\0';
    parent_path[pp_size - 1] = '\0';
    return 0;
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
    if (!parent || !parent->create) return -1;
    return parent->create(parent, name, type);
}

int vfs_unlink(vfs_node_t* parent, const char* name) {
    if (!parent || !parent->unlink) return -1;
    return parent->unlink(parent, name);
}

int vfs_stat(vfs_node_t* node, vfs_stat_t* st) {
    if (!node || !st) return -1;
    st->inode = node->inode;
    st->type  = node->type;
    st->size  = node->size;
    return 0;
}

/* ---- File-descriptor API ----------------------------------------------- */

static int fd_alloc(void) {
    for (int i = 0; i < VFS_MAX_FDS; ++i) {
        if (!fd_table[i].in_use) return i;
    }
    return -1;  /* no free fds */
}

int vfs_fd_open(const char* path, uint32_t flags) {
    if (!path) return -1;

    vfs_node_t* node = vfs_namei(path);

    /* Create if requested and doesn't exist */
    if (!node && (flags & VFS_O_CREATE)) {
        char parent_path[VFS_PATH_MAX];
        char base_name[VFS_NAME_MAX];
        if (split_path(path, parent_path, sizeof(parent_path),
                        base_name, sizeof(base_name)) < 0)
            return -1;

        vfs_node_t* parent = vfs_namei(parent_path);
        if (!parent) return -1;

        if (vfs_create(parent, base_name, VFS_FILE) < 0)
            return -1;

        node = vfs_finddir(parent, base_name);
        if (!node) return -1;
    }

    if (!node) return -1;

    if (vfs_open_node(node, flags) < 0)
        return -1;

    int fd = fd_alloc();
    if (fd < 0) {
        vfs_close_node(node);
        return -1;
    }

    fd_table[fd].node   = node;
    fd_table[fd].offset = (flags & VFS_O_APPEND) ? node->size : 0;
    fd_table[fd].flags  = flags;
    fd_table[fd].in_use = 1;

    /* Truncate if requested */
    if ((flags & VFS_O_TRUNC) && node->write) {
        node->size = 0;
    }

    return fd;
}

int32_t vfs_fd_read(int fd, uint8_t* buffer, uint32_t size) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -1;

    vfs_fd_t* f = &fd_table[fd];
    int32_t n = vfs_read(f->node, f->offset, size, buffer);
    if (n > 0) f->offset += (uint32_t)n;
    return n;
}

int32_t vfs_fd_write(int fd, const uint8_t* buffer, uint32_t size) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -1;

    vfs_fd_t* f = &fd_table[fd];
    int32_t n = vfs_write(f->node, f->offset, size, buffer);
    if (n > 0) f->offset += (uint32_t)n;
    return n;
}

int vfs_fd_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -1;

    vfs_close_node(fd_table[fd].node);
    fd_table[fd].node   = NULL;
    fd_table[fd].offset = 0;
    fd_table[fd].flags  = 0;
    fd_table[fd].in_use = 0;
    return 0;
}

int vfs_fd_stat(int fd, vfs_stat_t* st) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].in_use) return -1;
    return vfs_stat(fd_table[fd].node, st);
}

