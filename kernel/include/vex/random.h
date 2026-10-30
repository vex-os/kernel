#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

bool random_ready(void) __nodiscard;

/// Mixes noise into the entropy pool
/// @param credit_bits How many bits of entropy the caller vouches for, at most the size of the data
void random_add(const void* data, size_t size, unsigned credit_bits);

/// Fills a buffer with unpredictable bytes
/// @retval false No seed yet, and the buffer is untouched
bool random_bytes(void* out, size_t size) __nodiscard;
