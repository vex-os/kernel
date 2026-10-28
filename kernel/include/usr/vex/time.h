#ifndef USR_VEX_TIME_H
#define USR_VEX_TIME_H

#include <vex/types.h>

#define __k_CLOCK_REALTIME           0
#define __k_CLOCK_MONOTONIC          1
#define __k_CLOCK_PROCESS_CPUTIME_ID 2
#define __k_CLOCK_THREAD_CPUTIME_ID  3
#define __k_CLOCK_MONOTONIC_RAW      4
#define __k_CLOCK_REALTIME_COARSE    5
#define __k_CLOCK_MONOTONIC_COARSE   6
#define __k_CLOCK_BOOTTIME           7

/* Flag of the nanosleep call: the request
 * is a point on the clock and not a duration */
#define __k_TIMER_ABSTIME 1

struct __k_timespec {
    __k_time_t tv_sec;
    __k_suseconds_t tv_nsec;
};

#endif /* USR_VEX_TIME_H */
