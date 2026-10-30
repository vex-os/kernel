#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

char* kstrncat(char* restrict dst, const char* restrict src, size_t size);
char* kstrncpy(char* restrict dst, const char* restrict src, size_t size);
int strcasecmp(const char* restrict sa, const char* restrict sb) __nodiscard;
int strncasecmp(const char* restrict sa, const char* restrict sb, size_t size) __nodiscard;

void memzero_explicit(void* restrict dst, size_t size);
