#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

#define BLAKE2S_BLOCK_SIZE      64
#define BLAKE2S_CHAIN_WORDS     8
#define BLAKE2S_MAX_DIGEST_SIZE 32
#define BLAKE2S_MAX_KEY_SIZE    32

typedef struct blake2s_s blake2s_t;

struct blake2s_s {
    uint32_t chain[BLAKE2S_CHAIN_WORDS];
    uint64_t hashed;
    uint8_t buffer[BLAKE2S_BLOCK_SIZE];
    size_t buffered;
    size_t digest_size;
};

void blake2s_init(blake2s_t* restrict ctx, size_t digest_size);
void blake2s_init_keyed(blake2s_t* restrict ctx, size_t digest_size, const void* key, size_t key_size);
void blake2s_update(blake2s_t* restrict ctx, const void* data, size_t size);
void blake2s_finish(blake2s_t* restrict ctx, void* digest);
