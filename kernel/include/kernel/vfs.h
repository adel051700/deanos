#ifndef _KERNEL_VFS_H
#define _KERNEL_VFS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants --------------------------------------------------------- */
#define VFS_NAME_MAX    64
#define VFS_PATH_MAX    256
#define VFS_MAX_FDS     64
#define VFS_MAX_FILE    (64 * 1024)   /* 64 KiB per ramfs file */

/* Node types */
#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02

/* Open flags */
#define VFS_O_RDONLY    0x00
#define VFS_O_WRONLY    0x01
#define VFS_O_RDWR      0x02
#define VFS_O_CREATE    0x04
#define VFS_O_TRUNC     0x08
#define VFS_O_APPEND    0x10
#define VFS_O_CLOEXEC   0x20

/* FD flags exposed through vfs_fd_fcntl() */
#define VFS_FD_CLOEXEC  0x1

/* fcntl-like commands */
#define VFS_F_GETFD     1
#define VFS_F_SETFD     2

/* ---- Types ------------------------------------------------------------- */

struct vfs_node;

typedef struct vfs_dirent {
    char     name[VFS_NAME_MAX];
    uint32_t inode;
    uint32_t type;
} vfs_dirent_t;

typedef struct vfs_stat {
    uint32_t inode;
    uint32_t type;
    uint32_t size;
} vfs_stat_t;

/* Function pointer types for node operations */
typedef int32_t  (*vfs_read_fn)(struct vfs_node*, uint32_t offset, uint32_t size, uint8_t* buffer);
typedef int32_t  (*vfs_write_fn)(struct vfs_node*, uint32_t offset, uint32_t size, const uint8_t* buffer);
typedef int      (*vfs_open_fn)(struct vfs_node*, uint32_t flags);
typedef void     (*vfs_close_fn)(struct vfs_node*);
typedef int      (*vfs_readdir_fn)(struct vfs_node*, uint32_t index, vfs_dirent_t* out);
typedef struct vfs_node* (*vfs_finddir_fn)(struct vfs_node*, const char* name);
typedef int      (*vfs_create_fn)(struct vfs_node* parent, const char* name, uint32_t type);
typedef int      (*vfs_unlink_fn)(struct vfs_node* parent, const char* name);

/* VFS node (inode-like) */
typedef struct vfs_node {
    char         name[VFS_NAME_MAX];
    uint32_t     type;         /* VFS_FILE or VFS_DIRECTORY */
    uint32_t     size;         /* file: byte count; dir: child count */
    uint32_t     inode;        /* unique id */

    /* Operations vtable — set by the filesystem driver */
    vfs_read_fn    read;
    vfs_write_fn   write;
    vfs_open_fn    open;
    vfs_close_fn   close;
    vfs_readdir_fn readdir;
    vfs_finddir_fn finddir;
    vfs_create_fn  create;
    vfs_unlink_fn  unlink;

    /* Implementation-specific data (e.g. ramfs file data) */
    void* impl;

    /* Tree structure */
    struct vfs_node* parent;
    struct vfs_node* children;   /* first child (directories only) */
    struct vfs_node* next;       /* next sibling */
} vfs_node_t;

/* File descriptor entry */
typedef struct {
    vfs_node_t* node;
    uint32_t    offset;
    uint32_t    flags;
    int         in_use;
} vfs_fd_t;

/* ---- VFS API ----------------------------------------------------------- */

void         vfs_initialize(void);
vfs_node_t*  vfs_get_root(void);
void         vfs_set_root(vfs_node_t* root);

/* Path resolution */
vfs_node_t*  vfs_namei(const char* path);

/* Node-level operations (call through vtable) */
int32_t      vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
int32_t      vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer);
int          vfs_open_node(vfs_node_t* node, uint32_t flags);
void         vfs_close_node(vfs_node_t* node);
int          vfs_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out);
vfs_node_t*  vfs_finddir(vfs_node_t* node, const char* name);
int          vfs_create(vfs_node_t* parent, const char* name, uint32_t type);
int          vfs_unlink(vfs_node_t* parent, const char* name);
int          vfs_stat(vfs_node_t* node, vfs_stat_t* st);

/* FD-based API (uses the current task's fd table) */
int          vfs_fd_open(const char* path, uint32_t flags);
int32_t      vfs_fd_read(int fd, uint8_t* buffer, uint32_t size);
int32_t      vfs_fd_write(int fd, const uint8_t* buffer, uint32_t size);
int          vfs_fd_close(int fd);
int          vfs_fd_stat(int fd, vfs_stat_t* st);
int          vfs_fd_fcntl(int fd, uint32_t cmd, uint32_t arg);
int          vfs_fd_pipe(int out_fds[2]);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_VFS_H */

