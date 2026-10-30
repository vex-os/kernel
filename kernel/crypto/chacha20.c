#include <vex/chacha20.h>
#include <vex/endian.h>
#include <vex/string.h>
#include <vex/strings.h>

#define CONSTANT_WORDS (4)
#define COUNTER_WORD   (12)
#define DOUBLE_ROUNDS  (10)

#define KEY_WORDS   (CHACHA20_KEY_SIZE / sizeof(uint32_t))
#define NONCE_WORDS (CHACHA20_NONCE_SIZE / sizeof(uint32_t))

static_assert(CONSTANT_WORDS + KEY_WORDS + 1 + NONCE_WORDS == CHACHA20_STATE_WORDS);
static_assert(CHACHA20_BLOCK_SIZE == CHACHA20_STATE_WORDS * sizeof(uint32_t));

static const uint32_t constants[CONSTANT_WORDS] = {
    UINT32_C(0x61707865),
    UINT32_C(0x3320646E),
    UINT32_C(0x79622D32),
    UINT32_C(0x6B206574),
};

static uint32_t rotate_left(uint32_t value, unsigned count)
{
    return (value << count) | (value >> (32 - count));
}

static void quarter_round(uint32_t* restrict state, size_t a, size_t b, size_t c, size_t d)
{
    state[a] += state[b];
    state[d] = rotate_left(state[d] ^ state[a], 16);
    state[c] += state[d];
    state[b] = rotate_left(state[b] ^ state[c], 12);
    state[a] += state[b];
    state[d] = rotate_left(state[d] ^ state[a], 8);
    state[c] += state[d];
    state[b] = rotate_left(state[b] ^ state[c], 7);
}

static void load_words(uint32_t* restrict words, const uint8_t* restrict bytes, size_t count)
{
    for(size_t i = 0; i < count; ++i) {
        uint32_t word;
        memcpy(&word, bytes + i * sizeof(word), sizeof(word));
        words[i] = le32toh(word);
    }
}

void chacha20_init(chacha20_t* restrict ctx, const chacha20_key_t key, const chacha20_nonce_t nonce, uint32_t first_block)
{
    memcpy(ctx->state, constants, sizeof(constants));
    load_words(&ctx->state[CONSTANT_WORDS], key, KEY_WORDS);
    ctx->state[COUNTER_WORD] = first_block;
    load_words(&ctx->state[COUNTER_WORD + 1], nonce, NONCE_WORDS);
}

void chacha20_next_block(chacha20_t* restrict ctx, chacha20_block_t out)
{
    uint32_t work[CHACHA20_STATE_WORDS];

    memcpy(work, ctx->state, sizeof(work));

    for(int i = 0; i < DOUBLE_ROUNDS; ++i) {
        quarter_round(work, 0, 4, 8, 12);
        quarter_round(work, 1, 5, 9, 13);
        quarter_round(work, 2, 6, 10, 14);
        quarter_round(work, 3, 7, 11, 15);
        quarter_round(work, 0, 5, 10, 15);
        quarter_round(work, 1, 6, 11, 12);
        quarter_round(work, 2, 7, 8, 13);
        quarter_round(work, 3, 4, 9, 14);
    }

    for(size_t i = 0; i < CHACHA20_STATE_WORDS; ++i) {
        uint32_t word = htole32(work[i] + ctx->state[i]);

        memcpy(out + i * sizeof(word), &word, sizeof(word));
    }

    ctx->state[COUNTER_WORD] += 1;

    memzero_explicit(work, sizeof(work));
}

void chacha20_xor_next_blocks(chacha20_t* restrict ctx, void* out, const void* in, size_t size)
{
    uint8_t* dst = out;
    const uint8_t* src = in;
    chacha20_block_t stream;

    while(size) {
        chacha20_next_block(ctx, stream);

        size_t chunk = CHACHA20_BLOCK_SIZE;

        if(size < chunk) {
            chunk = size;
        }

        for(size_t i = 0; i < chunk; ++i) {
            dst[i] = src[i] ^ stream[i];
        }

        dst += chunk;
        src += chunk;
        size -= chunk;
    }

    memzero_explicit(stream, sizeof(stream));
}
