#include <arch/cycles.h>
#include <arch/hart.h>
#include <vex/clocksrc.h>
#include <vex/cmdline.h>
#include <vex/cycles.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/string.h>
#include <vex/time.h>

#define CALIBRATE_WINDOW_NS UINT64_C(20000000)
#define CALIBRATE_ROUNDS    5
#define CALIBRATE_SAMPLES   8
#define MULT_SHIFT          32
#define MISMATCH_PERMILLE   10 // A frequency from the arch that is further off the calibration than this gets a warning

typedef struct cycles_sample_s cycles_sample_t;

struct cycles_sample_s {
    uint64_t cycles;
    uint64_t nsec;
};

// Zero while local_clock_ns reads the clock source
static uint64_t cycles_mult = 0;

static uint64_t read_cycles_ns(void)
{
    uint128_t nsec = (uint128_t)(arch_cycles_read()) * cycles_mult;

    return (uint64_t)(nsec >> MULT_SHIFT);
}

// The pair with the narrowest gap between the two
// reads of the counter is the least disturbed one
static void take_sample(cycles_sample_t* restrict sample)
{
    uint64_t narrowest = UINT64_MAX;

    for(int i = 0; i < CALIBRATE_SAMPLES; ++i) {
        bool intr_state = disable_interrupts();
        uint64_t before = arch_cycles_read();
        uint64_t nsec = clock_source_now_ns();
        uint64_t after = arch_cycles_read();
        restore_interrupts(intr_state);

        uint64_t gap = after - before;

        if(gap < narrowest) {
            narrowest = gap;
            sample->cycles = before + gap / 2;
            sample->nsec = nsec;
        }
    }
}

static uint64_t measure_frequency(void)
{
    cycles_sample_t start = {};
    cycles_sample_t end = {};

    take_sample(&start);

    while(clock_source_now_ns() < start.nsec + CALIBRATE_WINDOW_NS) {
        // empty
    }

    take_sample(&end);

    // The window keeps the product far below 2^64, and the kernel has no 128-bit division
    uint64_t cycles = end.cycles - start.cycles;
    return cycles * NSEC_PER_SEC / (end.nsec - start.nsec);
}

static uint64_t calibrate(void)
{
    uint64_t results[CALIBRATE_ROUNDS] = {};

    for(int i = 0; i < CALIBRATE_ROUNDS; ++i) {
        uint64_t frequency = measure_frequency();
        int j = i;

        while(j > 0 && results[j - 1] > frequency) {
            results[j] = results[j - 1];
            j -= 1;
        }

        results[j] = frequency;
    }

    return results[CALIBRATE_ROUNDS / 2];
}

static bool frequencies_match(uint64_t reported, uint64_t measured)
{
    uint64_t difference = reported - measured;

    if(reported < measured) {
        difference = measured - reported;
    }

    return difference * 1000 <= measured * MISMATCH_PERMILLE;
}

static uint64_t pick_frequency(const arch_cycles_info_t* restrict info)
{
    if(info->frequency_exact) {
        return info->frequency;
    }

    uint64_t measured = calibrate();

    if(info->frequency == 0) {
        return measured;
    }

    if(!frequencies_match(info->frequency, measured)) {
        unsigned long long frequency = info->frequency;
        unsigned long long measured_ll = measured;
        pr_warning("cycles: the CPU reports %llu Hz, the calibration gives %llu Hz", frequency, measured_ll);
    }

    return info->frequency;
}

uint64_t local_clock_ns(void)
{
    if(cycles_mult) {
        return read_cycles_ns();
    }

    return clock_source_now_ns();
}

// After the clock sources of the timer
// stage, which the calibration reads
static void init_cycles(void)
{
    const char* argument = cmdline_argument(&sys_cmdline, "cputime");

    if(argument && 0 == strcmp(argument, "clocksrc")) {
        pr_inform("cycles: cputime=clocksrc, the cycle counter is not used");
        return;
    }

    arch_cycles_info_t info = {};
    arch_cycles_probe(&info);

    if(!info.constant_rate) {
        pr_inform("cycles: the cycle counter has no constant rate, CPU time comes from the clock source");
        return;
    }

    if(clock_source_active() == nullptr && !info.frequency_exact) {
        pr_warning("cycles: no clock source to calibrate against, CPU time comes from the clock source");
        return;
    }

    uint64_t frequency = pick_frequency(&info);

    if(frequency == 0) {
        pr_warning("cycles: calibration failed, CPU time comes from the clock source");
        return;
    }

    cycles_mult = ((uint64_t)(NSEC_PER_SEC) << MULT_SHIFT) / frequency;

    pr_inform("cycles: %llu Hz", (unsigned long long)(frequency));
}

initcall_bsp_timer(init_cycles, INIT_LATE);
