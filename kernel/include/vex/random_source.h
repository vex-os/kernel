#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

#define RANDOM_SOURCE_SAMPLE_MAX   64
#define RANDOM_SOURCE_CREDIT_UNITS 64 // An event credits in fractions of a bit, so events_per_bit divides this

typedef struct random_source_s random_source_t;
typedef struct random_source_private_s random_source_private_t;

struct random_source_private_s {
    uint64_t next_poll_ns;
    unsigned failures;
    bool usable;
    bool credited;
};

struct random_source_s {
    random_source_private_t priv;

    const char* name;
    bool trusted;            // Credited by default; random.trust_name overrides
    size_t sample_size;      // Polled: bytes of a full poll, at most RANDOM_SOURCE_SAMPLE_MAX
    unsigned sample_bits;    // Polled: bits of entropy that a full poll is worth
    unsigned events_per_bit; // Event: how many events earn one bit, at most RANDOM_SOURCE_CREDIT_UNITS

    /// Tells whether the source exists on this machine
    /// @note Polled sources only; an event source exists once its code runs
    bool (*probe)(void);

    /// Fills the buffer with up to sample_size bytes
    /// @note Polled sources only; an event source has none and pushes its events instead
    /// @return The number of bytes written. A short poll earns a share of sample_bits
    /// @retval -EAGAIN The hardware has nothing now, which is not a failure
    /// @retval -ENODEV The source is gone for good and the registry stops asking
    /// @retval -EIO The source failed; a few in a row take it out of the registry
    ssize_t (*poll)(void* out, size_t size);
};

#define define_random_source_polled(rs_name, rs_trusted, rs_sample_size, rs_sample_bits, rs_probe, rs_poll) \
    static __used __section(".random_sources") random_source_t __random_source_##rs_name = {                \
        .name = #rs_name,                                                                                   \
        .trusted = (rs_trusted),                                                                            \
        .sample_size = (rs_sample_size),                                                                    \
        .sample_bits = (rs_sample_bits),                                                                    \
        .probe = &(rs_probe),                                                                               \
        .poll = &(rs_poll),                                                                                 \
    }

#define define_random_source_event(rs_name, rs_trusted, rs_events_per_bit)                   \
    static __used __section(".random_sources") random_source_t __random_source_##rs_name = { \
        .name = #rs_name,                                                                    \
        .trusted = (rs_trusted),                                                             \
        .events_per_bit = (rs_events_per_bit),                                               \
    }

#define random_add_event(rs_name, rs_value) random_source_add_event(&__random_source_##rs_name, (rs_value))

extern random_source_t random_sources_start[];
extern random_source_t random_sources_end[];

/// Asks every source that is due for noise and mixes the answers into the pool
/// @pre Thread context, as a poll can take a while
/// @note Skips the call when another thread is already polling
void random_source_poll_all(void);

/// Takes an event of a source into the buffer of this hart, with the time it came
/// @pre Interrupts are disabled
/// @note Drops the event before the services stage, when the buffers do not exist yet
void random_source_add_event(random_source_t* restrict source, uint64_t value);
