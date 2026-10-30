#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

#define CHACHA20_KEY_SIZE    32
#define CHACHA20_NONCE_SIZE  12
#define CHACHA20_BLOCK_SIZE  64
#define CHACHA20_STATE_WORDS 16

typedef uint8_t chacha20_key_t[CHACHA20_KEY_SIZE];
typedef uint8_t chacha20_nonce_t[CHACHA20_NONCE_SIZE];
typedef uint8_t chacha20_block_t[CHACHA20_BLOCK_SIZE];
typedef struct chacha20_s chacha20_t;

struct chacha20_s {
    uint32_t state[CHACHA20_STATE_WORDS];
};

void chacha20_init(chacha20_t* restrict ctx, const chacha20_key_t key, const chacha20_nonce_t nonce, uint32_t first_block);
void chacha20_next_block(chacha20_t* restrict ctx, chacha20_block_t out);
void chacha20_xor_next_blocks(chacha20_t* restrict ctx, void* out, const void* in, size_t size);
