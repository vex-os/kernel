# Time

Vex keeps time on three independent scales. The clock source counts the time
since boot. The wall clock adds an offset to that count. The cycle counter
counts only the time of one hart, for CPU time accounting. Each scale has its
own path, and its section shows it.

## Clock sources

```
 counter --> clock source --> monotonic ns --> CLOCK_MONOTONIC
```

A clock source wraps a counter that never stops, such as the HPET or the ACPI
power management timer. Its `read` op returns the raw value. The registration
gives the frequency of the counter, and the kernel converts the ticks to
nanoseconds with a fixed-point multiplier and shift. A source declares the
bits of the counter that wrap, so a counter that is narrower than 64 bits keeps
the time across its rollover. The tick also calls `clock_source_update()`, so
a narrow counter that wraps between two reads still converts correctly.

Each source has a rating, and the highest rating is active. A source that
arrives with a higher rating takes over at once. It starts from the time that
the old source reached, so the clock never goes backwards. A source that goes
away hands the time to the best source that is left. The last active source
cannot be removed, because the kernel would have no time at all. The call
fails with `EBUSY`.

The clock source lock is a spinlock with interrupts disabled, so a read is
short. `clock_source_peek_ns()` tries the lock and reads the last value when
another caller holds it, because a panic path must not wait. `delay_nsec()`
and its two larger units spin on the source.

The amd64 sources are the HPET, at rating 200, and the ACPI power management
timer, at rating 100. The TSC is not a clock source, as its rate can change.

## Clock events

A clock event is a timer that raises an interrupt on request. Each hart has
one active timer, the best of the events that serve it. An event with
`CLOCK_EVENT_PER_HART` serves the hart that adds it, and every other event
serves the boot hart, because device interrupts go there.

The caller asks for a delay from now, and the timer clamps it to the range of
the device. With `clock_event_oneshot()` the interrupt comes once. With
`clock_event_periodic()` it repeats until `clock_event_stop()`. A timer with
only one of the two modes uses that mode for both requests.

A switch from one timer to another arms the new timer at its shortest delay
when it serves this hart. When it serves another hart, the kernel sends that
hart an IPI, and the handler arms the timer there. A removal that would leave
a hart without a timer fails with `EBUSY`.

The amd64 timers are the LAPIC timer and the PIT. The LAPIC timer has rating
300 and serves one hart. The PIT has rating 100 and serves the boot hart.

## The tick

```
 clock event --> tick --> scheduler, sleeps, itimers, CPU charge
```

There is no periodic tick. The scheduler arms a one-shot event for the
timeslice of the running thread, and arms it again on every switch. That
interrupt is the tick. It stops the timer and refreshes the clock source. It
wakes the threads whose sleep expired, and runs the expired interval timers
and the deferred events. It charges the CPU time of the thread and picks the
next thread.

An interval timer of a process is a deadline on the clock source, with an
optional interval. The armed timers sit in one list, sorted by deadline. The
tick walks the list from the front. It sends the signal of each timer that
expired, and moves an interval timer to the next point of its grid.

The CPU timers of a process count down in the tick instead. The tick compares
the time that the hart charged to the thread, so such a timer fires at most
one tick late.

## The wall clock

```
 monotonic ns + wall clock offset --> CLOCK_REALTIME
```

The wall clock is the monotonic time plus an offset. The offset is a
`timespec_t` under a seqlock, so a reader retries when a writer is in the
middle of a change.

`wallclock_set()` computes a new offset, publishes it, and writes the RTC. A
failure to write the RTC only logs a warning: the system clock has the new
time all the same. The call needs `CAP_SYS_TIME`.

At boot the kernel takes the date from the Limine date request. The RTC lives
in the device tree, which arrives later. When the first RTC comes into use,
the kernel takes its time if no date is known, or compares it with the boot
date. A difference of more than 2 seconds gets a warning. If the difference is
close to a whole number of 15 minutes, the warning adds that the RTC may hold
local time instead of UTC.

`wallclock_wait_until()` sleeps until a wall clock target. A `wallclock_set()`
moves the target with the clock, so the wait ends on the new scale and not on
the old one.

## CPU time

```
 cycle counter --> local_clock_ns --> CPU time
```

`local_clock_ns()` reads the cycle counter of the hart when the counter has a
constant rate, and the clock source otherwise. Its value is only compared with
a value of the same hart. The scheduler uses it to separate user time from
system time, and `CLOCK_THREAD_CPUTIME_ID` and `CLOCK_PROCESS_CPUTIME_ID`
report the sum.

The amd64 probe takes the TSC frequency from CPUID, or from the hypervisor
leaf. It marks the value as inexact, so the kernel also measures the frequency
against the clock source. The measure is the median of five rounds of 20 ms,
and it keeps the CPUID value when that value is not zero. A difference of more
than 1% logs a warning. The `cputime=clocksrc` command line skips the TSC and
leaves CPU time on the clock source.

## Interfaces

The clock IDs of the system calls reduce to two clocks: realtime and
monotonic. The raw and the coarse IDs read as their fine ID, because the
kernel steps the clock and never slews it. The CPU time IDs read the usage
counters instead. A `nanosleep(2)` with `TIMER_ABSTIME` on the realtime clock
uses `wallclock_wait_until()`, and a sleep on the monotonic clock uses a
deadline on the clock source.

## Limits

- A global timer serves the boot hart alone. A broadcast to harts whose own
  timer stops in idle is not done yet.
- The RTC is read as UTC, and the kernel applies no zone.
- The wall clock cannot be set past the point where the offset and the uptime
  would overflow. The conversion of a `timespec_t` saturates instead.
- The cycle counter is the better CPU clock only on a CPU with a constant
  rate, and only after the calibration.
- The interval timers and the sleeps have the resolution of the tick, not of
  the clock source.

## Source

- [clocksrc.c](https://github.com/vex-os/kernel/blob/main/kernel/time/clocksrc.c#L22) — the registry, the conversion and the active source
- [clockevt.c](https://github.com/vex-os/kernel/blob/main/kernel/time/clockevt.c#L112) — the timers of each hart and the switch
- [wallclock.c](https://github.com/vex-os/kernel/blob/main/kernel/time/wallclock.c#L24) — the offset, the boot date and the RTC check
- [rtc.c](https://github.com/vex-os/kernel/blob/main/kernel/time/rtc.c#L8) — the clocks that keep the time while the system is off
- [cycles.c](https://github.com/vex-os/kernel/blob/main/kernel/time/cycles.c#L75) — the calibration of the cycle counter
- [delay.c](https://github.com/vex-os/kernel/blob/main/kernel/time/delay.c#L5) — the busy waits
- [calendar.c](https://github.com/vex-os/kernel/blob/main/kernel/time/calendar.c#L22) — the proleptic Gregorian calendar
- [hpet.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/time/hpet.c#L50) and [acpi_pmt.c](https://github.com/vex-os/kernel/blob/main/kernel/time/acpi_pmt.c#L27) — the clock sources
- [lapic_timer.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/time/lapic_timer.c#L84) and [pit.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/time/pit.c#L171) — the clock events
- [tsc.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/time/tsc.c#L101) — the rate and the frequency of the cycle counter
- [cmos_rtc.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/time/cmos_rtc.c#L1) — the RTC driver
- [sched.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sched.c#L336) — the one-shot tick
- [itimer.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/itimer.c#L16) — the interval timers
- [sys_time.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_time.c#L22) — the clock IDs and the calls
