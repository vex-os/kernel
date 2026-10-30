#include <vex/atomic.h>
#include <vex/errno.h>
#include <vex/page.h>
#include <vex/random.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/thread.h>
#include <vex/uaccess.h>

#include "syscall_internal.h"

#define CHUNK_SIZE  256
#define MAX_SIZE    (INT32_MAX & ~(PAGE_SIZE - 1))
#define KNOWN_FLAGS (GRND_NONBLOCK | GRND_INSECURE)

static intptr_t sys_getrandom(user_addr_t buffer, size_t size, uintptr_t flags)
{
    if(flags & ~KNOWN_FLAGS) {
        return -EINVAL;
    }

    if(size > MAX_SIZE) {
        size = MAX_SIZE;
    }

    if(0 == (flags & GRND_INSECURE)) {
        bool blocking = 0 == (flags & GRND_NONBLOCK);
        ssize_t waited = random_wait_ready(blocking);

        if(waited < 0) {
            return waited;
        }
    }

    thread_t* thread = this_thread();
    uint8_t chunk[CHUNK_SIZE];
    size_t total = 0;
    intptr_t error = 0;

    while(total < size) {
        size_t length = size - total;

        if(length > sizeof(chunk)) {
            length = sizeof(chunk);
        }

        // The generator has its seed by now unless the caller allowed it not to
        random_bytes_insecure(chunk, length);

        user_addr_t destination = address_add(buffer, total);
        ssize_t copied = user_copyout(destination, chunk, length);

        if(copied < 0) {
            error = copied;
            break;
        }

        total += length;

        // A signal or a stop ends a long call early, with what it has given so far
        if(atomic_load(&thread->pending_work, ATOMIC_ACQUIRE)) {
            break;
        }
    }

    memzero_explicit(chunk, sizeof(chunk));

    if(total > 0) {
        return total;
    }

    return error;
}

define_syscall(sys_getrandom, SYS_GETRANDOM, 3);
