#include <arch/hart.h>
#include <vex/cpucaps.h>
#include <vex/errno.h>
#include <vex/random_source.h>
#include <vex/string.h>

#define VALUES  4
#define RETRIES 10

static bool rdseed_probe(void)
{
    return cpu_has(CPU_FEATURE_RDSEED);
}

static bool rdseed_read(uint64_t* restrict value)
{
    bool ready;

    asm volatile("rdseed %0" : "=r"(*value), "=@ccc"(ready));

    return ready;
}

// The value is not ready when the carry flag stays clear
static bool rdseed_read_retry(uint64_t* restrict value)
{
    for(int attempt = 0; attempt < RETRIES; attempt += 1) {
        if(rdseed_read(value)) {
            return true;
        }

        hart_relax();
    }

    return false;
}

static ssize_t rdseed_poll(void* out, size_t size)
{
    uint8_t* bytes = out;
    size_t count = size / sizeof(uint64_t);

    for(size_t i = 0; i < count; ++i) {
        uint64_t value;

        if(!rdseed_read_retry(&value)) {
            if(i == 0) {
                return -EAGAIN;
            }

            return i * sizeof(value);
        }

        memcpy(&bytes[i * sizeof(value)], &value, sizeof(value));
    }

    return count * sizeof(uint64_t);
}

define_random_source_polled(rdseed, true, VALUES * sizeof(uint64_t), VALUES * 64, rdseed_probe, rdseed_poll);
