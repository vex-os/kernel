#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

bool random_needs_noise(void) __nodiscard;

/// Asks the random thread to drain the event buffers and poll the sources
/// @note Any context; before the thread exists, the request waits for it
void random_source_wake(void);
