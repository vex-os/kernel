#pragma once

#include <arch/hart.h>
#include <vex/cpucaps.h>

typedef struct thread_s thread_t;

#ifdef CONFIG_SMP
#define MAX_HARTS ((unsigned)(CONFIG_MAX_HARTS))
#else
#define MAX_HARTS 1U
#endif

#define HART_FLAG_BSP     0x0001
#define HART_FLAG_ACTIVE  0x0002
#define HART_FLAG_PRESENT 0x0004
#define HART_FLAG_ONLINE  0x0008

enum {
    HART_BRINGUP_WAIT,
    HART_BRINGUP_GO,
    HART_BRINGUP_PARK,
};

typedef struct hart_local_s hart_local_t;

struct hart_local_s {
    unsigned short index;
    unsigned short flags;

    thread_t* curr_thread;
    thread_t* idle_thread;
    thread_t* dead_thread;

    uint64_t cputime_mark; // local_clock_ns when the time of the hart was last charged to a thread

    bool tlb_online;
    uint64_t tlb_flushed;

    cpucaps_t caps;
    unsigned char bringup;

    arch_hart_t arch;
};

static_assert(MAX_HARTS <= 65536);

extern hart_local_t harts[MAX_HARTS];
extern unsigned long num_harts;

hart_local_t* this_hart(void) __nodiscard;

/// @pre index is below num_harts
bool hart_online(unsigned long index) __nodiscard;

hart_local_t* hart_register(bool is_boot);
void hart_park(hart_local_t* restrict hart) __noreturn;

static __always_inline __nodiscard hart_local_t* boot_hart(void)
{
    return &harts[0];
}
