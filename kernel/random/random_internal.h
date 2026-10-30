#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

typedef struct event_s event_t;

bool random_needs_noise(void) __nodiscard;

/// Triggered once, when the generator gets its first seed
event_t* random_ready_event(void) __nodiscard;

/// Asks the random thread to drain the event buffers and poll the sources
/// @note Any context; before the thread exists, the request waits for it
void random_source_wake(void);
