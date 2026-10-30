#include <vex/blake2s.h>
#include <vex/chacha20.h>
#include <vex/clocksrc.h>
#include <vex/kprintf.h>
#include <vex/random.h>
#include <vex/random_source.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/time.h>

#define RANDOM_MIN_POOL_BYTES 32
#define RANDOM_MIN_RESEED_MS  100
#define RANDOM_CHUNK_SIZE     256

#define MIN_RESEED_INTERVAL (RANDOM_MIN_RESEED_MS * NSEC_PER_MSEC)
#define MAX_CREDIT_BITS     (BLAKE2S_MAX_DIGEST_SIZE * 8)
#define MIN_CREDIT_BITS     (RANDOM_MIN_POOL_BYTES * 8)

static_assert(RANDOM_MIN_POOL_BYTES <= BLAKE2S_MAX_DIGEST_SIZE);
static_assert(RANDOM_CHUNK_SIZE % CHACHA20_BLOCK_SIZE == 0);

static const chacha20_nonce_t zero_nonce = {};

// Everything below belongs to the lock. The key is new for each request, so the nonce stays zero.
static spinlock_t lock = SPINLOCK_INIT;
static blake2s_t pool;
static bool pool_started;
static unsigned credit_bits;
static chacha20_key_t key;
static bool seeded;
static uint64_t last_reseed_ns;

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

    credit_bits = 0;
    last_reseed_ns = now_ns;
    seeded = true;
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
static void generate(uint8_t* out, size_t size)
{
    chacha20_t stream;
    chacha20_block_t block;

    chacha20_init(&stream, key, zero_nonce, 0);

    memset(out, 0, size);
    chacha20_xor_next_blocks(&stream, out, out, size);
    chacha20_next_block(&stream, block);

    memcpy(key, block, sizeof(key));

    memzero_explicit(block, sizeof(block));
    memzero_explicit(&stream, sizeof(stream));
}

static bool needs_noise(void)
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

bool random_bytes(void* out, size_t size)
{
    uint8_t* dest = out;

    if(needs_noise()) {
        random_source_poll_all();
    }

    if(!random_ready()) {
        return false;
    }

    while(size) {
        size_t chunk = RANDOM_CHUNK_SIZE;

        if(size < chunk) {
            chunk = size;
        }

        bool intr_state = spinlock_acquire_intr(&lock);

        reseed_if_due();
        generate(dest, chunk);
        spinlock_release_intr(&lock, intr_state);

        dest += chunk;
        size -= chunk;
    }

    return true;
}
