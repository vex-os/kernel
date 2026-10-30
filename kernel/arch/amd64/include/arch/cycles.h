#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

typedef struct arch_cycles_info_s arch_cycles_info_t;

struct arch_cycles_info_s {
    bool constant_rate;   // The rate does not change with the clock of the core; the counter can stop while the hart idles
    bool frequency_exact; // frequency comes from the architecture and needs no calibration
    uint64_t frequency;   // Hz, or zero when the arch does not know it
};

/// Tells if the cycle counter of the harts can measure time
/// @pre On the boot hart
/// @note The counters of two harts can differ, so a value is only compared with a value of the same hart
void arch_cycles_probe(arch_cycles_info_t* restrict info);

static __always_inline uint64_t arch_cycles_read(void)
{
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)(high) << 32) | low;
}
