#pragma once

#include <vex/compiler.h>
#include <vex/cred.h>
#include <vex/types.h>

typedef struct file_s file_t;

/// Makes a pipe: one open file for each end
/// @param flags O_NONBLOCK, which both files get
/// @param credentials The owner that stat shows
/// @post On success each file holds the only reference to its end; the pipe goes away with the last file
/// @retval -ENOSYS Pipes did not set up at boot
int pipe_create(unsigned flags, const credentials_t* restrict credentials, file_t** restrict read_end,
    file_t** restrict write_end) __nodiscard;
