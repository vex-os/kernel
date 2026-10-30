#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

typedef struct credentials_s credentials_t;
typedef struct file_s file_t;
typedef struct vmspace_s vmspace_t;

#define BINFMT_ARGS_MAX 32 // Of argv and of envp each

typedef struct binfmt_entry_s binfmt_entry_t;
typedef struct binfmt_ops_s binfmt_ops_t;
typedef struct binfmt_s binfmt_t;
typedef struct binfmt_private_s binfmt_private_t;

struct binfmt_entry_s {
    uintptr_t address;
    uintptr_t user_stack;
};

struct binfmt_ops_s {
    int (*probe)(file_t* restrict file, const uint8_t* restrict header, size_t header_size);

    /// @param stack_size The size of the user stack in bytes, a multiple of PAGE_SIZE
    int (*setup)(vmspace_t* restrict vmspace, file_t* restrict file, const char** restrict argv, const char** restrict envp,
        const credentials_t* restrict credentials, size_t stack_size, binfmt_entry_t* restrict out);
};

struct binfmt_private_s {
    binfmt_t* next;
};

struct binfmt_s {
    binfmt_private_t priv;
    const binfmt_ops_t* ops;
    const char* name;
};

int binfmt_add(binfmt_t* restrict binfmt);
int binfmt_remove(binfmt_t* restrict binfmt);

/// @param credentials The ones the program will run with, which setuid and setgid bits may have made differ from the caller's
/// @param stack_limit The soft RLIMIT_STACK of the process; the stack may grow that big, within bounds that load_binary sets,
/// and the default one when the limit is RLIM_INFINITY
int load_binary(vmspace_t* restrict vmspace, file_t* restrict file, const char** restrict argv, const char** restrict envp,
    const credentials_t* restrict credentials, uint64_t stack_limit, binfmt_entry_t* restrict out);
