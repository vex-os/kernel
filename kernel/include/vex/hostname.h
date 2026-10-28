#pragma once

#include <usr/vex/utsname.h>
#include <vex/types.h>

#define HOSTNAME_SIZE __k_UNAME_SIZE
#define HOSTNAME_MAX  (HOSTNAME_SIZE - 1)

/// @pre size is not zero
/// @post buffer holds a terminated name, cut to size
/// @note Takes a spinlock, so any context can call it
void hostname_get(char* restrict buffer, size_t size);

/// @param name Not terminated; only its first length bytes count
/// @retval -EINVAL The name has more than HOSTNAME_MAX bytes
int hostname_set(const char* restrict name, size_t length);
