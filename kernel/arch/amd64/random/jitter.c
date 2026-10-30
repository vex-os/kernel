#include <arch/cycles.h>
#include <vex/errno.h>
#include <vex/random_source.h>

#define SAMPLES     64
#define CREDIT_BITS 8  // One bit for eight measurements: the timing of a virtual machine can be close to regular
#define TABLE_SIZE  64 // Entries the workload touches, so that the cache adds to the jitter

// A counter that stops or ticks in steps gives repeated deltas, and then the sample is worth nothing
#define MAX_REPEATS (SAMPLES / 2)

static volatile uint64_t table[TABLE_SIZE];

static bool jitter_probe(void)
{
    return true;
}

static uint64_t measure(uint64_t seed)
{
    uint64_t start = arch_cycles_read();
    size_t rounds = (seed & 7) + 1;

    for(size_t i = 0; i < rounds; ++i) {
        size_t index = (seed + i * 13) % TABLE_SIZE;
        table[index] += start;
    }

    return arch_cycles_read() - start;
}

static uint8_t fold(uint64_t value)
{
    uint8_t result = 0;

    for(unsigned shift = 0; shift < 64; shift += 8) {
        result ^= value >> shift;
    }

    return result;
}

static ssize_t jitter_poll(void* out, size_t size)
{
    uint8_t* bytes = out;
    uint64_t previous = 0;
    size_t repeats = 0;

    for(size_t i = 0; i < size; ++i) {
        uint64_t delta = measure(previous);

        if(delta == previous) {
            repeats += 1;
        }

        bytes[i] = fold(delta);
        previous = delta;
    }

    if(repeats > MAX_REPEATS) {
        return -EAGAIN;
    }

    return size;
}

define_random_source_polled(jitter, true, SAMPLES, CREDIT_BITS, jitter_probe, jitter_poll);
