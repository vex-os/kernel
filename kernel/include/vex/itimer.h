#pragma once

#include <vex/types.h>

typedef struct hart_local_s hart_local_t;
typedef struct process_s process_t;
typedef struct thread_s thread_t;

typedef struct itimer_s itimer_t;
typedef struct itimer_cpu_s itimer_cpu_t;
typedef struct itimer_cpu_set_s itimer_cpu_set_t;
typedef struct itimer_state_s itimer_state_t;

// ITIMER_VIRTUAL and ITIMER_PROF
#define ITIMER_CPU_COUNT 2

// Counts down in the CPU time of the threads of the process, under the lock of the list
struct itimer_cpu_s {
    uint64_t remaining; // Nanoseconds until the next expiry; zero while the timer is off
    uint64_t interval;  // Nanoseconds; zero for a timer that fires once
};

struct itimer_cpu_set_s {
    itimer_cpu_t timers[ITIMER_CPU_COUNT];
};

struct itimer_s {
    process_t* next;       // The next armed timer, under the lock of the list
    uint64_t deadline;     // On the clock_source_now_ns scale; zero while the timer is off
    uint64_t interval;     // Nanoseconds; zero for a timer that fires once
    itimer_cpu_set_t* cpu; // Made when the first CPU timer is armed, as most processes have none, and kept until the process goes
};

struct itimer_state_s {
    uint64_t value;    // Nanoseconds until the next expiry; zero for a timer that is off
    uint64_t interval; // Nanoseconds; zero for a timer that fires once
};

/// Reads and changes a timer of the process: ITIMER_REAL sends SIGALRM after wall clock time, ITIMER_VIRTUAL sends
/// SIGVTALRM after the user time of the threads, and ITIMER_PROF sends SIGPROF after their user and system time
/// @pre The process is the one of the calling thread, and which is one of the three timers
/// @param next The new setting, or nullptr to only read; a zero value turns the timer off
/// @param previous Receives the setting from before, or nullptr
/// @retval 0 Done
/// @retval -ENOMEM A CPU timer could not be made
/// @note A process that has finished its exit gets no timer. A CPU timer counts down when the hart that runs a thread of the
/// process takes a tick, so it fires a tick late at most, and a read has the time of the caller up to the call
int itimer_set(process_t* restrict process, int which, const itimer_state_t* restrict next, itimer_state_t* restrict previous);

/// Turns the timers of the process off for good
/// @pre The process has set PROCESS_EXIT_FINISHED
void itimer_cancel(process_t* restrict process);

/// Frees what the timers of the process keep
/// @pre The last reference to the process is gone
void itimer_destroy(process_t* restrict process);

/// Counts the CPU time that the hart charged to the thread since the last call against the CPU timers of its process
/// @pre Called by the scheduler tick, with interrupts disabled, before the thread changes
void itimer_count_cpu(hart_local_t* restrict hart, thread_t* restrict thread);

/// Sends SIGALRM for the timers that expired, and starts the next period of those with an interval
/// @pre Called by the scheduler tick, with interrupts disabled
void itimer_run_expired(void);
