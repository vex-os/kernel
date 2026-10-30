#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

/// Nanoseconds on the clock of this hart, for CPU time accounting
/// @pre Interrupts are disabled, or the caller cannot move to another hart
/// @note Values of two harts can differ, so a value is only compared with a value of the same hart.
/// Before the timer stage, and on a CPU whose counter has no constant rate, this is clock_source_now_ns
uint64_t local_clock_ns(void) __nodiscard;

/// The length of one tick of local_clock_ns, rounded up, and at least 1
uint64_t local_clock_resolution_ns(void) __nodiscard;
