#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

typedef struct hart_local_s hart_local_t;
typedef struct process_s process_t;
typedef struct thread_s thread_t;
typedef struct __k_rusage rusage_t;

typedef struct cputime_s cputime_t;

struct cputime_s {
    uint64_t user_ns;
    uint64_t system_ns;
};

/// Charges the time since the last mark to the user time of the current thread
/// @pre Interrupts are disabled, and the hart has just come from user mode
void cputime_enter_kernel(void);

/// Charges the time since the last mark to the system time of the current thread
/// @pre Interrupts are disabled, and the hart goes to user mode next
void cputime_exit_kernel(void);

/// Charges the time since the last mark to the system time of curr, which leaves the hart or stays on it
/// @pre In the scheduler, with interrupts disabled
void cputime_switch(hart_local_t* restrict hart, thread_t* restrict curr);

/// Fills the times, and zeroes the fields that the kernel does not count
void cputime_to_rusage(const cputime_t* restrict time, rusage_t* restrict out);

/// @note For the current thread, this includes the time since the last mark
void cputime_of_thread(const thread_t* restrict thread, cputime_t* restrict out);

/// The threads that are reaped and the threads that live
/// @pre The lock of the process is not held
/// @note A thread that runs on another hart lags by its current run
void cputime_of_process(process_t* restrict process, cputime_t* restrict out);

static __always_inline void cputime_add(cputime_t* restrict sum, const cputime_t* restrict value)
{
    sum->user_ns += value->user_ns;
    sum->system_ns += value->system_ns;
}
