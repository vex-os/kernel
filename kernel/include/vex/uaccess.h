#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

/// A kernel thread gets -EFAULT
ssize_t user_copyin(void* restrict dst, user_addr_t src, size_t size);

/// vms_copyin_str on the calling process's address space; a kernel thread gets -EFAULT
/// @return The length of the string, without the terminator
/// @retval -ENAMETOOLONG No terminator within max_sz; dst holds a truncated, terminated copy
/// @retval -EFAULT Part of the string isn't mapped readable
/// @retval -EINVAL max_sz is zero
ssize_t user_copyin_str(char* restrict dst, user_addr_t src, size_t max_sz);

/// A kernel thread gets -EFAULT
ssize_t user_copyout(user_addr_t dst, const void* restrict src, size_t size);

/// A kernel thread gets -EFAULT
ssize_t user_copyout_str(user_addr_t dst, const char* restrict src, size_t max_sz);
