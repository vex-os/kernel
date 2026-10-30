#pragma once

#include <vex/atomic.h>
#include <vex/compiler.h>
#include <vex/types.h>

typedef struct hart_local_s hart_local_t;
typedef struct process_s process_t;
typedef struct thread_s thread_t;
typedef struct __k_rusage rusage_t;

typedef struct usage_s usage_t;

// What getrusage reports, summed over threads and over collected children
struct usage_s {
    uint64_t user_ns;
    uint64_t system_ns;
    uint64_t minor_faults;
    uint64_t voluntary_switches;   // The thread blocked
    uint64_t involuntary_switches; // The thread was preempted or yielded
    uint64_t signals;              // Delivered to a handler
};

/// Charges the time since the last mark to the user time of the current thread
/// @pre Interrupts are disabled, and the hart has just come from user mode
void usage_enter_kernel(void);

/// Charges the time since the last mark to the system time of the current thread
/// @pre Interrupts are disabled, and the hart goes to user mode next
void usage_exit_kernel(void);

/// Charges the time since the last mark to the system time of curr, which leaves the hart or stays on it
/// @pre In the scheduler, with interrupts disabled
void usage_switch(hart_local_t* restrict hart, thread_t* restrict curr);

/// Fills what the kernel counts, and zeroes the other fields
void usage_to_rusage(const usage_t* restrict usage, rusage_t* restrict out);

/// @note For the current thread, this includes the time since the last mark
void usage_of_thread(const thread_t* restrict thread, usage_t* restrict out);

/// The threads that are reaped and the threads that live
/// @pre The lock of the process is not held
/// @note A thread that runs on another hart lags by its current run
void usage_of_process(process_t* restrict process, usage_t* restrict out);

static __always_inline void usage_add(usage_t* restrict sum, const usage_t* restrict value)
{
    sum->user_ns += value->user_ns;
    sum->system_ns += value->system_ns;
    sum->minor_faults += value->minor_faults;
    sum->voluntary_switches += value->voluntary_switches;
    sum->involuntary_switches += value->involuntary_switches;
    sum->signals += value->signals;
}

/// Adds to a counter in the usage of a thread
/// @pre On the hart that runs the thread, or in the scheduler as the thread leaves it
/// @note Only that hart changes the counter, but another hart reads it, so the store is atomic
static __always_inline void usage_increase(uint64_t* restrict counter, uint64_t amount)
{
    uint64_t value = atomic_load_n(counter, ATOMIC_RELAXED);
    atomic_store_n(counter, value + amount, ATOMIC_RELAXED);
}
