#include <vex/assert.h>
#include <vex/blake2s.h>
#include <vex/endian.h>
#include <vex/string.h>
#include <vex/strings.h>

#define ROUNDS      10
#define STATE_WORDS 16
#define BLOCK_WORDS (BLAKE2S_BLOCK_SIZE / sizeof(uint32_t))

// Digest length, key length, fanout and depth are packed into the first word (RFC 7693, 2.5)
#define PARAM_FANOUT_DEPTH UINT32_C(0x01010000)
#define PARAM_KEY_SHIFT    8

static_assert(BLAKE2S_MAX_DIGEST_SIZE == BLAKE2S_CHAIN_WORDS * sizeof(uint32_t));
static_assert(STATE_WORDS == 2 * BLAKE2S_CHAIN_WORDS);

static const uint32_t initial_chain[BLAKE2S_CHAIN_WORDS] = {
    UINT32_C(0x6A09E667),
    UINT32_C(0xBB67AE85),
    UINT32_C(0x3C6EF372),
    UINT32_C(0xA54FF53A),
    UINT32_C(0x510E527F),
    UINT32_C(0x9B05688C),
    UINT32_C(0x1F83D9AB),
    UINT32_C(0x5BE0CD19),
};

static const uint8_t sigma[ROUNDS][BLOCK_WORDS] = {
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F },
    { 0x0E, 0x0A, 0x04, 0x08, 0x09, 0x0F, 0x0D, 0x06, 0x01, 0x0C, 0x00, 0x02, 0x0B, 0x07, 0x05, 0x03 },
    { 0x0B, 0x08, 0x0C, 0x00, 0x05, 0x02, 0x0F, 0x0D, 0x0A, 0x0E, 0x03, 0x06, 0x07, 0x01, 0x09, 0x04 },
    { 0x07, 0x09, 0x03, 0x01, 0x0D, 0x0C, 0x0B, 0x0E, 0x02, 0x06, 0x05, 0x0A, 0x04, 0x00, 0x0F, 0x08 },
    { 0x09, 0x00, 0x05, 0x07, 0x02, 0x04, 0x0A, 0x0F, 0x0E, 0x01, 0x0B, 0x0C, 0x06, 0x08, 0x03, 0x0D },
    { 0x02, 0x0C, 0x06, 0x0A, 0x00, 0x0B, 0x08, 0x03, 0x04, 0x0D, 0x07, 0x05, 0x0F, 0x0E, 0x01, 0x09 },
    { 0x0C, 0x05, 0x01, 0x0F, 0x0E, 0x0D, 0x04, 0x0A, 0x00, 0x07, 0x06, 0x03, 0x09, 0x02, 0x08, 0x0B },
    { 0x0D, 0x0B, 0x07, 0x0E, 0x0C, 0x01, 0x03, 0x09, 0x05, 0x00, 0x0F, 0x04, 0x08, 0x06, 0x02, 0x0A },
    { 0x06, 0x0F, 0x0E, 0x09, 0x0B, 0x03, 0x00, 0x08, 0x0C, 0x02, 0x0D, 0x07, 0x01, 0x04, 0x0A, 0x05 },
    { 0x0A, 0x02, 0x08, 0x04, 0x07, 0x06, 0x01, 0x05, 0x0F, 0x0B, 0x09, 0x0E, 0x03, 0x0C, 0x0D, 0x00 },
};

static uint32_t rotate_right(uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32 - count));
}

static void mix(uint32_t* restrict state, size_t a, size_t b, size_t c, size_t d, uint32_t x, uint32_t y)
{
    state[a] += state[b] + x;
    state[d] = rotate_right(state[d] ^ state[a], 16);
    state[c] += state[d];
    state[b] = rotate_right(state[b] ^ state[c], 12);
    state[a] += state[b] + y;
    state[d] = rotate_right(state[d] ^ state[a], 8);
    state[c] += state[d];
    state[b] = rotate_right(state[b] ^ state[c], 7);
}

// The counter must already include the bytes of this block
static void compress(blake2s_t* restrict ctx, const uint8_t* restrict block, bool last)
{
    uint32_t message[BLOCK_WORDS];
    uint32_t state[STATE_WORDS];

    for(size_t i = 0; i < BLOCK_WORDS; ++i) {
        uint32_t word;

        memcpy(&word, block + i * sizeof(word), sizeof(word));

        message[i] = le32toh(word);
    }

    memcpy(state, ctx->chain, sizeof(ctx->chain));
    memcpy(&state[BLAKE2S_CHAIN_WORDS], initial_chain, sizeof(initial_chain));

    state[12] ^= (uint32_t)(ctx->hashed);
    state[13] ^= (uint32_t)(ctx->hashed >> 32);

    if(last) {
        state[14] = ~state[14];
    }

    for(size_t round = 0; round < ROUNDS; round += 1) {
        const uint8_t* order = sigma[round];

        mix(state, 0, 4, 8, 12, message[order[0]], message[order[1]]);
        mix(state, 1, 5, 9, 13, message[order[2]], message[order[3]]);
        mix(state, 2, 6, 10, 14, message[order[4]], message[order[5]]);
        mix(state, 3, 7, 11, 15, message[order[6]], message[order[7]]);
        mix(state, 0, 5, 10, 15, message[order[8]], message[order[9]]);
        mix(state, 1, 6, 11, 12, message[order[10]], message[order[11]]);
        mix(state, 2, 7, 8, 13, message[order[12]], message[order[13]]);
        mix(state, 3, 4, 9, 14, message[order[14]], message[order[15]]);
    }

    for(size_t i = 0; i < BLAKE2S_CHAIN_WORDS; ++i) {
        ctx->chain[i] ^= state[i] ^ state[i + BLAKE2S_CHAIN_WORDS];
    }

    memzero_explicit(message, sizeof(message));
    memzero_explicit(state, sizeof(state));
}

static void init_with_key_size(blake2s_t* restrict ctx, size_t digest_size, size_t key_size)
{
    kassert(digest_size > 0 && digest_size <= BLAKE2S_MAX_DIGEST_SIZE);

    memcpy(ctx->chain, initial_chain, sizeof(initial_chain));

    ctx->chain[0] ^= PARAM_FANOUT_DEPTH ^ (uint32_t)(key_size << PARAM_KEY_SHIFT) ^ (uint32_t)(digest_size);
    ctx->hashed = 0;
    ctx->buffered = 0;
    ctx->digest_size = digest_size;
}

void blake2s_init(blake2s_t* restrict ctx, size_t digest_size)
{
    init_with_key_size(ctx, digest_size, 0);
}

void blake2s_init_keyed(blake2s_t* restrict ctx, size_t digest_size, const void* key, size_t key_size)
{
    kassert(key_size > 0 && key_size <= BLAKE2S_MAX_KEY_SIZE);

    init_with_key_size(ctx, digest_size, key_size);

    memset(ctx->buffer, 0, sizeof(ctx->buffer));
    memcpy(ctx->buffer, key, key_size);

    ctx->buffered = BLAKE2S_BLOCK_SIZE;
}

void blake2s_update(blake2s_t* restrict ctx, const void* data, size_t size)
{
    const uint8_t* source = data;

    // A full buffer waits for more input, as the last block needs its own flag
    size_t space = BLAKE2S_BLOCK_SIZE - ctx->buffered;

    if(size <= space) {
        memcpy(&ctx->buffer[ctx->buffered], source, size);

        ctx->buffered += size;

        return;
    }

    memcpy(&ctx->buffer[ctx->buffered], source, space);

    ctx->hashed += BLAKE2S_BLOCK_SIZE;
    compress(ctx, ctx->buffer, false);

    source += space;
    size -= space;

    while(size > BLAKE2S_BLOCK_SIZE) {
        ctx->hashed += BLAKE2S_BLOCK_SIZE;
        compress(ctx, source, false);

        source += BLAKE2S_BLOCK_SIZE;
        size -= BLAKE2S_BLOCK_SIZE;
    }

    memcpy(ctx->buffer, source, size);

    ctx->buffered = size;
}

void blake2s_finish(blake2s_t* restrict ctx, void* digest)
{
    uint8_t output[BLAKE2S_MAX_DIGEST_SIZE];

    memset(&ctx->buffer[ctx->buffered], 0, BLAKE2S_BLOCK_SIZE - ctx->buffered);

    ctx->hashed += ctx->buffered;
    compress(ctx, ctx->buffer, true);

    for(size_t i = 0; i < BLAKE2S_CHAIN_WORDS; ++i) {
        uint32_t word = htole32(ctx->chain[i]);

        memcpy(&output[i * sizeof(word)], &word, sizeof(word));
    }

    memcpy(digest, output, ctx->digest_size);

    memzero_explicit(output, sizeof(output));
    memzero_explicit(ctx, sizeof(*ctx));
}
