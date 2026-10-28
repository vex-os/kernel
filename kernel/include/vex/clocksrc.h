#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

#define CLOCK_SOURCE_MASK_1  UINT64_C(0x0000000000000001)
#define CLOCK_SOURCE_MASK_2  UINT64_C(0x0000000000000003)
#define CLOCK_SOURCE_MASK_4  UINT64_C(0x000000000000000F)
#define CLOCK_SOURCE_MASK_8  UINT64_C(0x00000000000000FF)
#define CLOCK_SOURCE_MASK_16 UINT64_C(0x000000000000FFFF)
#define CLOCK_SOURCE_MASK_24 UINT64_C(0x0000000000FFFFFF)
#define CLOCK_SOURCE_MASK_32 UINT64_C(0x00000000FFFFFFFF)
#define CLOCK_SOURCE_MASK_48 UINT64_C(0x0000FFFFFFFFFFFF)
#define CLOCK_SOURCE_MASK_64 UINT64_C(0xFFFFFFFFFFFFFFFF)

typedef struct clock_source_s clock_source_t;
typedef struct clock_source_ops_s clock_source_ops_t;
typedef struct clock_source_private_s clock_source_private_t;

struct clock_source_ops_s {
    uint64_t (*read)(clock_source_t* restrict source);
};

struct clock_source_private_s {
    clock_source_t* next;
    uint64_t last;
    uint64_t base;
    uint64_t mult;
    uint64_t shift;
};

struct clock_source_s {
    clock_source_private_t priv;
    const clock_source_ops_t* ops;
    const char* name;
    unsigned rating;
    uint64_t mask;
    void* ctxdata;
};

int clock_source_add_raw(clock_source_t* restrict source, uint64_t scale, uint64_t frequency);
int clock_source_calibrate_raw(clock_source_t* restrict source, uint64_t scale, uint64_t frequency);

/// Takes a source out, with the time going on from the best one left if it was active
/// @retval -ENOENT The source isn't added
/// @retval -EBUSY The source is active and the only one, as time can't go on without it
int clock_source_remove(clock_source_t* restrict source);

clock_source_t* clock_source_active(void) __nodiscard;

uint64_t clock_source_now_ns(void) __nodiscard;

/// The length of one tick of the active source in nanoseconds, rounded up
/// @retval 1 No source is active, or the tick is shorter than a nanosecond
uint64_t clock_source_resolution_ns(void) __nodiscard;

/// clock_source_now_ns that never waits for the lock: while someone holds it,
/// this returns the time of the last update instead, which is all panic output can afford
uint64_t clock_source_peek_ns(void) __nodiscard;

void clock_source_update(void);

static __always_inline int clock_source_add_hz(clock_source_t* restrict source, uint64_t hz)
{
    return clock_source_add_raw(source, UINT64_C(1), hz);
}

static __always_inline int clock_source_add_khz(clock_source_t* restrict source, uint64_t khz)
{
    return clock_source_add_raw(source, UINT64_C(1000), khz);
}

static __always_inline int clock_source_add_mhz(clock_source_t* restrict source, uint64_t mhz)
{
    return clock_source_add_raw(source, UINT64_C(1000000), mhz);
}

static __always_inline int clock_source_calibrate_hz(clock_source_t* restrict source, uint64_t hz)
{
    return clock_source_calibrate_raw(source, UINT64_C(1), hz);
}

static __always_inline int clock_source_calibrate_khz(clock_source_t* restrict source, uint64_t khz)
{
    return clock_source_calibrate_raw(source, UINT64_C(1000), khz);
}

static __always_inline int clock_source_calibrate_mhz(clock_source_t* restrict source, uint64_t mhz)
{
    return clock_source_calibrate_raw(source, UINT64_C(1000000), mhz);
}
