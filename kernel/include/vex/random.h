#pragma once

#include <usr/vex/random.h>
#include <vex/compiler.h>
#include <vex/types.h>

#define GRND_NONBLOCK __k_GRND_NONBLOCK
#define GRND_INSECURE __k_GRND_INSECURE

bool random_ready(void) __nodiscard;

/// Mixes noise into the entropy pool
/// @param credit_bits How many bits of entropy the caller vouches for, at most the size of the data
void random_add(const void* data, size_t size, unsigned credit_bits);

/// Fills a buffer with unpredictable bytes
/// @retval false No seed yet, and the buffer is untouched
bool random_bytes(void* out, size_t size) __nodiscard;

/// Fills a buffer even before the first seed
/// @note Before the seed the bytes come from what the pool holds so far, which an attacker can guess
void random_bytes_insecure(void* out, size_t size);

/// Polls the sources until the generator has its first seed
/// @pre Thread context
/// @param blocking Without it, the call polls once and gives up
/// @retval -EAGAIN The call is not blocking and the generator has no seed
/// @retval -ERESTART Work is pending; a system call returns this as it is
ssize_t random_wait_ready(bool blocking);
