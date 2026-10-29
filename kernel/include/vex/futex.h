#pragma once

#include <usr/vex/futex.h>
#include <vex/address.h>
#include <vex/compiler.h>
#include <vex/types.h>

typedef struct vmspace_s vmspace_t;

#define FUTEX_SHARED __k_FUTEX_SHARED

/// Sleeps while the 32-bit word at address holds expected, until futex_wake
/// on the same word, the deadline, or pending work such as a signal
/// @param deadline Absolute time on the clock_source_now_ns scale, zero for none
/// @param shared Whether the word is named by its frame and not by its address in vmspace
/// @retval 0 Woken, possibly spuriously; the caller looks at the word again
/// @retval -EAGAIN The word didn't hold expected
/// @retval -ETIMEDOUT The deadline passed
/// @retval -EINTR Work is pending
/// @retval -EFAULT The address isn't mapped
/// @retval -EINVAL The address isn't 4-byte aligned
/// @note A shared waiter keeps its frame pinned while it sleeps
int futex_wait(vmspace_t* restrict vmspace, virt_addr_t address, uint32_t expected, uint64_t deadline, bool shared);

/// Wakes up to count waiters on the word, the longest waiting first
/// @param shared As for futex_wait; wakes only the waiters that used the same kind of key
/// @retval >=0 The number of threads woken
/// @retval -EFAULT A shared word isn't mapped
ssize_t futex_wake(vmspace_t* restrict vmspace, virt_addr_t address, size_t count, bool shared);
