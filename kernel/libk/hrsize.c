#include <vex/ctype.h>
#include <vex/errno.h>
#include <vex/hrsize.h>
#include <vex/sprintf.h>
#include <vex/stdlib.h>
#include <vex/string.h>

#define IEC_LETTERS "KMGTPE" // Each one is 1024 times the one before it

int format_size_iec(char* restrict buffer, size_t size, size_t value)
{
    static const char* suffixes[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
    static const size_t num_suffixes = sizeof(suffixes) / sizeof(suffixes[0]);

    size_t index = 0U;

    for(size_t i = 0U; i < num_suffixes; ++i) {
        if(value >= 1666U) {
            value >>= 10U;
            index += 1U;
        }
    }

    memset(buffer, 0, size);

    return snprintf(buffer, size, "%zu %s", value, suffixes[index]);
}

int format_size_si(char* restrict buffer, size_t size, size_t value)
{
    static const char* suffixes[] = { "B", "kB", "MB", "GB", "TB", "PB", "EB", "ZB" };
    static const size_t num_suffixes = sizeof(suffixes) / sizeof(suffixes[0]);

    size_t index = 0U;

    for(size_t i = 0U; i < num_suffixes; ++i) {
        if(value >= 1666U) {
            value /= 1000U;
            index += 1U;
        }
    }

    memset(buffer, 0, size);

    return snprintf(buffer, size, "%zu %s", value, suffixes[index]);
}

int parse_size_iec(const char* restrict string, size_t* restrict out)
{
    // The conversion takes a sign and spaces, and tells nothing about digits it did not find
    if(!isdigit(string[0])) {
        return -EINVAL;
    }

    const char* end;
    size_t value = strtousize(string, &end, 10);
    unsigned shift = 0U;

    if(*end) {
        const char* letter = strchr(IEC_LETTERS, toupper(*end));

        if(letter) {
            shift = 10U * (unsigned)(1 + letter - IEC_LETTERS);
            end += 1;
        }

        if(letter && end[0] == 'i' && end[1] == 'B') {
            end += 2;
        }
        else if(*end == 'B') {
            end += 1;
        }
    }

    // The conversion gives SIZE_MAX for a number that does not fit
    if(*end || value == SIZE_MAX || value > (SIZE_MAX >> shift)) {
        return -EINVAL;
    }

    *out = value << shift;

    return 0;
}
