#pragma once

#include <vex/atomic.h>
#include <vex/compiler.h>
#include <vex/fcntl.h>
#include <vex/limits.h>
#include <vex/types.h>
#include <vex/vnode.h>

#define SEEK_SET __k_SEEK_SET
#define SEEK_CUR __k_SEEK_CUR
#define SEEK_END __k_SEEK_END

#define FILE_IO_CHUNK PAGE_SIZE // The most a read or write system call passes to the file layer at once

typedef struct file_s file_t;

struct file_s {
    vnode_t* node;
    unsigned refcount;
    unsigned flags; // F_SETFL changes it under readers, who go through file_flags
    off_t offset;
    void* ctxdata; // The node's, set by its open
};

int file_open(vnode_t* restrict base_dir, const char* restrict path, unsigned flags, mode_t mode, const credentials_t* restrict credentials,
    file_t** restrict out) __nodiscard;
int file_open_node(vnode_t* restrict node, unsigned flags, const credentials_t* restrict credentials, file_t** restrict out) __nodiscard;
file_t* file_dupe(file_t* restrict file) __nodiscard;
void file_close(file_t* restrict file);

ssize_t file_read(file_t* restrict file, void* restrict buffer, size_t size);
ssize_t file_write(file_t* restrict file, const credentials_t* restrict credentials, const void* restrict buffer, size_t size);

int file_truncate(file_t* restrict file, const credentials_t* restrict credentials, off_t size);
int file_seek(file_t* restrict file, off_t offset, int origin);
int file_ioctl(file_t* restrict file, uint32_t request, uintptr_t argument);

static __always_inline unsigned file_flags(const file_t* restrict file)
{
    return atomic_load(&file->flags, ATOMIC_RELAXED);
}

static __always_inline bool file_flags_allow_read(unsigned flags)
{
    return (flags & O_ACCMODE) != O_WRONLY;
}

static __always_inline bool file_flags_allow_write(unsigned flags)
{
    return (flags & O_ACCMODE) != O_RDONLY;
}
