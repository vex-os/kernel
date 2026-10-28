#ifndef USR_VEX_TIME_H
#define USR_VEX_TIME_H

#include <vex/types.h>

#define __k_CLOCK_REALTIME  0
#define __k_CLOCK_MONOTONIC 1

/* Flag of the nanosleep call: the request
 * is a point on the clock and not a duration */
#define __k_TIMER_ABSTIME 1

struct __k_timespec {
    __k_time_t tv_sec;
    __k_suseconds_t tv_nsec;
};

#endif /* USR_VEX_TIME_H */
