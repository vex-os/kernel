#include <vex/assert.h>
#include <vex/blake2s.h>
#include <vex/chacha20.h>
#include <vex/clocksrc.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/random.h>
#include <vex/random_source.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/time.h>

#include "random_internal.h"

#define RANDOM_MIN_POOL_BYTES 32
#define RANDOM_MIN_RESEED_MS  100
#define RANDOM_CHUNK_SIZE     256

#define MIN_RESEED_INTERVAL (RANDOM_MIN_RESEED_MS * NSEC_PER_MSEC)
#define MAX_CREDIT_BITS     (BLAKE2S_MAX_DIGEST_SIZE * 8)
#define MIN_CREDIT_BITS     (RANDOM_MIN_POOL_BYTES * 8)

static_assert(RANDOM_MIN_POOL_BYTES <= BLAKE2S_MAX_DIGEST_SIZE);
static_assert(RANDOM_CHUNK_SIZE % CHACHA20_BLOCK_SIZE == 0);

static const chacha20_nonce_t zero_nonce = {};

// Everything below belongs to the lock. The key
// is new for each request, so the nonce stays zero.

static spinlock_t lock = SPINLOCK_INIT;
static blake2s_t pool;
static bool pool_started;
static unsigned credit_bits;
static chacha20_key_t key;
static bool seeded;
static uint64_t last_reseed_ns;
static event_t ready_event; // Triggered under the lock when the first seed arrives, so that a waiter that looked under it cannot miss it

static void start_pool(void)
{
    blake2s_init(&pool, BLAKE2S_MAX_DIGEST_SIZE);

    pool_started = true;
}

static void reseed(uint64_t now_ns)
{
    uint8_t seed[BLAKE2S_MAX_DIGEST_SIZE];

    // The pool takes its own seed back, so
    // the next seed depends on this one
    blake2s_t copy = pool;
    blake2s_finish(&copy, seed);

    blake2s_init(&pool, BLAKE2S_MAX_DIGEST_SIZE);
    blake2s_update(&pool, seed, sizeof(seed));

    // The old key goes into the new one, so a weak
    // seed does not weaken what the key already has
    blake2s_t mixer;
    blake2s_init_keyed(&mixer, sizeof(key), key, sizeof(key));
    blake2s_update(&mixer, seed, sizeof(seed));
    blake2s_finish(&mixer, key);

    memzero_explicit(seed, sizeof(seed));

    bool first = !seeded;

    credit_bits = 0;
    last_reseed_ns = now_ns;
    seeded = true;

    if(first) {
        event_trigger(&ready_event, false);
    }
}

static void reseed_if_due(void)
{
    if(credit_bits < MIN_CREDIT_BITS) {
        return;
    }

    uint64_t now_ns = clock_source_now_ns();
    bool due = !seeded || now_ns - last_reseed_ns >= MIN_RESEED_INTERVAL;

    if(due) {
        reseed(now_ns);
    }
}

// Fast key erasure: the block after the output becomes the next key
static void generate(chacha20_key_t secret, uint8_t* out, size_t size)
{
    chacha20_t stream;
    chacha20_block_t block;

    chacha20_init(&stream, secret, zero_nonce, 0);

    memset(out, 0, size);
    chacha20_xor_next_blocks(&stream, out, out, size);
    chacha20_next_block(&stream, block);

    memcpy(secret, block, CHACHA20_KEY_SIZE);

    memzero_explicit(block, sizeof(block));
    memzero_explicit(&stream, sizeof(stream));
}

// The key comes from what the pool holds so far, and the
// pool takes the next key back so that the next call differs
static void generate_unseeded(uint8_t* out, size_t size)
{
    chacha20_key_t temporary;

    if(!pool_started) {
        start_pool();
    }

    blake2s_t copy = pool;
    blake2s_finish(&copy, temporary);

    generate(temporary, out, size);

    blake2s_update(&pool, temporary, sizeof(temporary));

    memzero_explicit(temporary, sizeof(temporary));
}

bool random_needs_noise(void)
{
    bool intr_state = spinlock_acquire_intr(&lock);
    bool needs = credit_bits < MIN_CREDIT_BITS;
    spinlock_release_intr(&lock, intr_state);

    return needs;
}

bool random_ready(void)
{
    bool intr_state = spinlock_acquire_intr(&lock);
    bool ready = seeded;
    spinlock_release_intr(&lock, intr_state);

    return ready;
}

void random_add(const void* data, size_t size, unsigned bits)
{
    bool intr_state = spinlock_acquire_intr(&lock);

    if(!pool_started) {
        start_pool();
    }

    blake2s_update(&pool, data, size);

    credit_bits += bits;

    if(credit_bits > MAX_CREDIT_BITS) {
        credit_bits = MAX_CREDIT_BITS;
    }

    // The first seed comes as soon as the pool can give one
    if(!seeded) {
        reseed_if_due();
    }

    spinlock_release_intr(&lock, intr_state);
}

static bool fill(void* out, size_t size, bool insecure)
{
    uint8_t* dest = out;

    if(random_needs_noise()) {
        random_source_wake();
        random_source_poll_all();
    }

    if(!insecure && !random_ready()) {
        return false;
    }

    while(size) {
        size_t chunk = RANDOM_CHUNK_SIZE;

        if(size < chunk) {
            chunk = size;
        }

        bool intr_state = spinlock_acquire_intr(&lock);

        reseed_if_due();

        if(seeded) {
            generate(key, dest, chunk);
        }
        else {
            generate_unseeded(dest, chunk);
        }

        spinlock_release_intr(&lock, intr_state);

        dest += chunk;
        size -= chunk;
    }

    return true;
}

bool random_bytes(void* out, size_t size)
{
    return fill(out, size, false);
}

void random_bytes_insecure(void* out, size_t size)
{
    bool filled = fill(out, size, true);

    kassert(filled);
}

ssize_t random_wait_ready(bool blocking)
{
    event_t* events[] = { &ready_event };

    while(!random_ready()) {
        random_source_poll_all();

        bool intr_state = spinlock_acquire_intr(&lock);

        if(seeded) {
            spinlock_release_intr(&lock, intr_state);
            break;
        }

        if(!blocking) {
            spinlock_release_intr(&lock, intr_state);
            return -EAGAIN;
        }

        // Nothing polls the sources while the caller sleeps, so it wakes to do it
        ssize_t result = event_await_unlock(events, 1, &lock, intr_state, MIN_RESEED_INTERVAL);

        if(result < 0 && result != -ETIMEDOUT) {
            return result;
        }
    }

    return 0;
}

static void init_random(void)
{
    event_init(&ready_event);
}

initcall_bsp_early(init_random, INIT_FIRST);
