#pragma once

#include <vex/types.h>

typedef struct process_s process_t;

typedef struct itimer_s itimer_t;
typedef struct itimer_state_s itimer_state_t;

struct itimer_s {
    process_t* next;   // The next armed timer, under the lock of the list
    uint64_t deadline; // On the clock_source_now_ns scale; zero while the timer is off
    uint64_t interval; // Nanoseconds; zero for a timer that fires once
};

struct itimer_state_s {
    uint64_t value;    // Nanoseconds until the next expiry; zero for a timer that is off
    uint64_t interval; // Nanoseconds; zero for a timer that fires once
};

/// Reads and changes the ITIMER_REAL timer of the process, which sends SIGALRM
/// @param next The new setting, or nullptr to only read; a zero value turns the timer off
/// @param previous Receives the setting from before, or nullptr
/// @note A process that has finished its exit gets no timer
void itimer_set(process_t* restrict process, const itimer_state_t* restrict next, itimer_state_t* restrict previous);

/// Turns the timer of the process off for good
/// @pre The process has set PROCESS_EXIT_FINISHED
void itimer_cancel(process_t* restrict process);

/// Sends SIGALRM for the timers that expired, and starts the next period of those with an interval
/// @pre Called by the scheduler tick, with interrupts disabled
void itimer_run_expired(void);
