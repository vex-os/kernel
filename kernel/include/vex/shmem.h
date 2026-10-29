#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

typedef struct file_s file_t;

/// Makes a file of memory that no name leads to, for a shared mapping that has no file
/// @param size A multiple of PAGE_SIZE, not zero
/// @param out Open for reading and writing; its pages exist from the start and count against the commit limit
/// @post The memory goes away with the last reference to the file
/// @retval -ENOMEM The commit limit has no room for size
/// @retval -ENOSYS There is no memory filesystem
int shmem_file_create(size_t size, file_t** restrict out) __nodiscard;
