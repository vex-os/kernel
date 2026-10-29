#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

int format_size_iec(char* restrict buffer, size_t size, size_t value);
int format_size_si(char* restrict buffer, size_t size, size_t value);

/// Reads a number with an optional unit, as K, KiB, M or GB: all of them are powers of 1024, in either case
/// @retval -EINVAL The text is not a size, or the size does not fit
int parse_size_iec(const char* restrict string, size_t* restrict out);
