#pragma once

#include <vex/time.h>
#include <vex/types.h>

void wallclock_get(timespec_t* restrict out);
int wallclock_set(const timespec_t* restrict time);
time_t wallclock_now(void);

/// Waits until the wall clock reaches the target. A wallclock_set while waiting moves the end of the wait with it
/// @return Zero once the target is reached
/// @retval -ENOMEM No memory to register the thread as a listener
/// @retval -ERESTART Work is pending; a system call returns this
/// as it is and runs again or fails with EINTR on the way out
ssize_t wallclock_wait_until(const timespec_t* restrict target);

/// Checks the time against the RTC that just came into use, or takes the time from it when nothing else gave one
/// @pre Called by rtc_add, without the RTCs' lock
void wallclock_rtc_arrived(void);
