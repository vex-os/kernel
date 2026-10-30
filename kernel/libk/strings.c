#include <vex/bitmap.h>
#include <vex/ctype.h>
#include <vex/string.h>
#include <vex/strings.h>

char* kstrncat(char* restrict dst, const char* restrict src, size_t size)
{
    char* save = dst;

    while(*dst && size--) {
        dst += 1;
    }

    size_t nc = strnlen(src, --size);

    dst[nc] = 0;

    memcpy(dst, src, nc);

    return save;
}

char* kstrncpy(char* restrict dst, const char* restrict src, size_t size)
{
    char* save = dst;

    while(*src && size--) {
        *dst++ = *src++;
    }

    if(size) {
        *dst = 0;
    }

    return save;
}

int strcasecmp(const char* restrict sa, const char* restrict sb)
{
    while(*sa && tolower(*sa) == tolower(*sb)) {
        sa += 1;
        sb += 1;
    }

    return tolower((unsigned char)(*sa)) - tolower((unsigned char)(*sb));
}

int strncasecmp(const char* restrict sa, const char* restrict sb, size_t size)
{
    while(size-- && *sa && tolower(*sa) == tolower(*sb)) {
        sa += 1;
        sb += 1;
    }

    if(size == SIZE_MAX) {
        return 0;
    }

    return tolower(*sa) - tolower(*sb);
}

void memzero_explicit(void* restrict dst, size_t size)
{
    memset(dst, 0, size);

    asm volatile("" ::"r"(dst) : "memory");
}
