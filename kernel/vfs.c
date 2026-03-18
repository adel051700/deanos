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

/* ---- Initialisation ---------------------------------------------------- */

void vfs_initialize(void) {
    vfs_root = NULL;
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

    uint32_t open_flags = flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREATE |
                                   VFS_O_TRUNC | VFS_O_APPEND);
    uint32_t fd_flags = (flags & VFS_O_CLOEXEC) ? VFS_FD_CLOEXEC : 0;

    vfs_node_t* node = vfs_namei(path);

    /* Create if requested and doesn't exist */
    if (!node && (open_flags & VFS_O_CREATE)) {
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

    /* Truncate if requested */
    if ((open_flags & VFS_O_TRUNC) && node->write) {
        node->size = 0;
    }

    return fd;
}

int32_t vfs_fd_read(int fd, uint8_t* buffer, uint32_t size) {
    task_t* t = task_current();
    if (!t) return -1;
    if (fd < 0 || fd >= TASK_MAX_FDS || !t->fds[fd].in_use) return -1;

    task_fd_t* f = &t->fds[fd];
    int32_t n = vfs_read(f->node, f->offset, size, buffer);
    if (n > 0) f->offset += (uint32_t)n;
    return n;
}

int32_t vfs_fd_write(int fd, const uint8_t* buffer, uint32_t size) {
    task_t* t = task_current();
    if (!t) return -1;
    if (fd < 0 || fd >= TASK_MAX_FDS || !t->fds[fd].in_use) return -1;

    task_fd_t* f = &t->fds[fd];
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

