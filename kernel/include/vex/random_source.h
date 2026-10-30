#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

#define RANDOM_SOURCE_SAMPLE_MAX 64

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
    bool trusted;         // Credited by default; random.trust_name overrides
    size_t sample_size;   // Bytes of a full poll, at most RANDOM_SOURCE_SAMPLE_MAX
    unsigned sample_bits; // Bits of entropy that a full poll is worth

    /// Tells whether the source exists on this machine
    bool (*probe)(void);

    /// Fills the buffer with up to sample_size bytes
    /// @return The number of bytes written. A short poll earns a share of sample_bits
    /// @retval -EAGAIN The hardware has nothing now, which is not a failure
    /// @retval -ENODEV The source is gone for good and the registry stops asking
    /// @retval -EIO The source failed; a few in a row take it out of the registry
    ssize_t (*poll)(void* out, size_t size);
};

#define define_random_source(rs_name, rs_trusted, rs_sample_size, rs_sample_bits, rs_probe, rs_poll) \
    static __used __section(".random_sources") random_source_t __random_source_##rs_name = {         \
        .name = #rs_name,                                                                            \
        .trusted = (rs_trusted),                                                                     \
        .sample_size = (rs_sample_size),                                                             \
        .sample_bits = (rs_sample_bits),                                                             \
        .probe = &(rs_probe),                                                                        \
        .poll = &(rs_poll),                                                                          \
    }

extern random_source_t random_sources_start[];
extern random_source_t random_sources_end[];

/// Asks every source that is due for noise and mixes the answers into the pool
/// @pre Thread context, as a poll can take a while
/// @note Skips the call when another thread is already polling
void random_source_poll_all(void);
