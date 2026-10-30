#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

typedef struct vnode_s vnode_t;

#define FILE_LOCK_END INT64_MAX // The end of a range that runs past the end of the file

typedef struct file_lock_request_s file_lock_request_t;

// Locks of one owner never conflict with each other; locks of different owners
// conflict where their ranges overlap and one of them is a write lock
struct file_lock_request_s {
    const void* owner; // The process for a POSIX lock, the file for a lock of an open file
    pid_t pid;         // What F_GETLK reports: the pid of the owner, or -1 for an open file
    unsigned type;     // F_RDLCK, F_WRLCK or F_UNLCK
    off_t start;
    off_t end; // Inclusive, or FILE_LOCK_END
};

/// Takes, changes or drops the locks of the owner in the range
/// @param wait Sleep while a lock of another owner is in the way, instead of failing
/// @retval -EAGAIN A lock of another owner is in the way, and wait is false
/// @retval -ERESTART A signal came while the thread waited
/// @retval -ENOMEM No memory for the ranges
int file_lock_set(vnode_t* restrict node, const file_lock_request_t* restrict request, bool wait);

/// Finds a lock of another owner that the request conflicts with
/// @param request Replaced with that lock when there is one
/// @return Whether there is one
bool file_lock_test(vnode_t* restrict node, file_lock_request_t* restrict request);

/// Drops every lock of the owner on the node
void file_lock_release(vnode_t* restrict node, const void* restrict owner);

/// @pre No file of the node is open anymore, so no lock is left
void file_locks_destroy(vnode_t* restrict node);
