/*
 * ramfs.c — In-memory filesystem driver
 *
 * Every file stores its data in a heap-allocated buffer that grows via
 * krealloc.  Directories use the vfs_node_t linked-list children.
 *
 * This driver populates the vtable on each vfs_node_t it creates so
 * the generic VFS layer can dispatch through function pointers.
 */

#include "include/kernel/ramfs.h"
#include "include/kernel/vfs.h"
#include "include/kernel/kheap.h"
#include "include/kernel/log.h"
#include <string.h>
#include <stddef.h>

/* ---- Per-file heap data ------------------------------------------------ */

typedef struct {
    uint8_t* data;
    uint32_t capacity;   /* allocated bytes */
} ramfs_file_data_t;

/* ---- Inode counter ----------------------------------------------------- */

static uint32_t next_inode = 1;

/* ---- Forward declarations of vtable callbacks -------------------------- */

static int32_t  ramfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buf);
static int32_t  ramfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buf);
static int      ramfs_open(vfs_node_t* node, uint32_t flags);
static void     ramfs_close(vfs_node_t* node);
static int      ramfs_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out);
static vfs_node_t* ramfs_finddir(vfs_node_t* node, const char* name);
static int      ramfs_create(vfs_node_t* parent, const char* name, uint32_t type);
static int      ramfs_unlink(vfs_node_t* parent, const char* name);

/* ---- Helper: allocate a new node --------------------------------------- */

static vfs_node_t* ramfs_alloc_node(const char* name, uint32_t type) {
    vfs_node_t* n = (vfs_node_t*)kcalloc(1, sizeof(vfs_node_t));
    if (!n) return NULL;

    strncpy(n->name, name, VFS_NAME_MAX - 1);
    n->name[VFS_NAME_MAX - 1] = '\0';
    n->type  = type;
    n->size  = 0;
    n->inode = next_inode++;

    /* Wire up the vtable */
    n->read     = ramfs_read;
    n->write    = ramfs_write;
    n->open     = ramfs_open;
    n->close    = ramfs_close;
    n->readdir  = ramfs_readdir;
    n->finddir  = ramfs_finddir;
    n->create   = ramfs_create;
    n->unlink   = ramfs_unlink;

    n->parent   = NULL;
    n->children = NULL;
    n->next     = NULL;
    n->impl     = NULL;

    if (type == VFS_FILE) {
        /* Allocate a small initial buffer */
        ramfs_file_data_t* fd = (ramfs_file_data_t*)kcalloc(1, sizeof(ramfs_file_data_t));
        if (fd) {
            fd->data     = NULL;
            fd->capacity = 0;
        }
        n->impl = fd;
    }

    return n;
}

/* ---- vtable callbacks -------------------------------------------------- */

static int32_t ramfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buf) {
    if (!node || !(node->type & VFS_FILE) || !buf) return -1;

    ramfs_file_data_t* fd = (ramfs_file_data_t*)node->impl;
    if (!fd) return -1;

    if (offset >= node->size) return 0;   /* EOF */

    uint32_t avail = node->size - offset;
    if (size > avail) size = avail;

    if (fd->data)
        memcpy(buf, fd->data + offset, size);

    return (int32_t)size;
}

static int ramfs_ensure_capacity(ramfs_file_data_t* fd, uint32_t needed) {
    if (needed <= fd->capacity) return 0;
    if (needed > VFS_MAX_FILE) return -1;   /* cap at 64 KiB */

    uint32_t new_cap = fd->capacity ? fd->capacity : 256;
    while (new_cap < needed) {
        new_cap *= 2;
        if (new_cap > VFS_MAX_FILE) new_cap = VFS_MAX_FILE;
    }

    uint8_t* new_data = (uint8_t*)krealloc(fd->data, new_cap);
    if (!new_data) return -1;

    /* Zero newly allocated region */
    if (new_cap > fd->capacity)
        memset(new_data + fd->capacity, 0, new_cap - fd->capacity);

    fd->data     = new_data;
    fd->capacity = new_cap;
    return 0;
}

static int32_t ramfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buf) {
    if (!node || !(node->type & VFS_FILE) || !buf) return -1;

    ramfs_file_data_t* fd = (ramfs_file_data_t*)node->impl;
    if (!fd) return -1;

    uint32_t end = offset + size;
    if (end > VFS_MAX_FILE) {
        /* Clamp */
        if (offset >= VFS_MAX_FILE) return -1;
        size = VFS_MAX_FILE - offset;
        end = VFS_MAX_FILE;
    }

    if (ramfs_ensure_capacity(fd, end) < 0) return -1;

    memcpy(fd->data + offset, buf, size);
    if (end > node->size) node->size = end;

    return (int32_t)size;
}

static int ramfs_open(vfs_node_t* node, uint32_t flags) {
    (void)node; (void)flags;
    return 0;  /* always succeeds */
}

static void ramfs_close(vfs_node_t* node) {
    (void)node;
    /* nothing to do — data stays in memory */
}

static int ramfs_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out) {
    if (!node || !(node->type & VFS_DIRECTORY) || !out) return -1;

    vfs_node_t* child = node->children;
    uint32_t i = 0;
    while (child) {
        if (i == index) {
            strncpy(out->name, child->name, VFS_NAME_MAX - 1);
            out->name[VFS_NAME_MAX - 1] = '\0';
            out->inode = child->inode;
            out->type  = child->type;
            return 0;
        }
        child = child->next;
        i++;
    }

    return -1;  /* index out of range */
}

static vfs_node_t* ramfs_finddir(vfs_node_t* node, const char* name) {
    if (!node || !(node->type & VFS_DIRECTORY) || !name) return NULL;

    vfs_node_t* child = node->children;
    while (child) {
        if (strcmp(child->name, name) == 0) return child;
        child = child->next;
    }
    return NULL;
}

static int ramfs_create(vfs_node_t* parent, const char* name, uint32_t type) {
    if (!parent || !(parent->type & VFS_DIRECTORY) || !name) return -1;
    if (strlen(name) == 0 || strlen(name) >= VFS_NAME_MAX) return -1;

    /* Check for duplicates */
    if (ramfs_finddir(parent, name)) return -1;  /* already exists */

    vfs_node_t* child = ramfs_alloc_node(name, type);
    if (!child) return -1;

    child->parent = parent;

    /* Append to end of children list */
    if (!parent->children) {
        parent->children = child;
    } else {
        vfs_node_t* tail = parent->children;
        while (tail->next) tail = tail->next;
        tail->next = child;
    }

    parent->size++;   /* child count */
    return 0;
}

static int ramfs_unlink(vfs_node_t* parent, const char* name) {
    if (!parent || !(parent->type & VFS_DIRECTORY) || !name) return -1;

    vfs_node_t* prev  = NULL;
    vfs_node_t* child = parent->children;

    while (child) {
        if (strcmp(child->name, name) == 0) {
            /* Don't allow removing non-empty directories */
            if ((child->type & VFS_DIRECTORY) && child->children)
                return -1;

            /* Unlink from sibling list */
            if (prev)
                prev->next = child->next;
            else
                parent->children = child->next;

            parent->size--;

            /* Free file data if applicable */
            if (child->type & VFS_FILE) {
                ramfs_file_data_t* fd = (ramfs_file_data_t*)child->impl;
                if (fd) {
                    if (fd->data) kfree(fd->data);
                    kfree(fd);
                }
            }
            kfree(child);
            return 0;
        }
        prev  = child;
        child = child->next;
    }

    return -1;  /* not found */
}

/* ---- Public init ------------------------------------------------------- */

void ramfs_initialize(void) {
    vfs_node_t* root = ramfs_alloc_node("/", VFS_DIRECTORY);
    if (!root) {
        klog("[ramfs] FATAL: could not allocate root node");
        return;
    }
    vfs_set_root(root);
}

