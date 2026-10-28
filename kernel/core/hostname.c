#include <vex/errno.h>
#include <vex/hostname.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/strings.h>

static_assert(sizeof(CONFIG_DEFAULT_HOSTNAME) <= HOSTNAME_SIZE);

static spinlock_t hostname_lock = SPINLOCK_INIT;
static char hostname[HOSTNAME_SIZE] = CONFIG_DEFAULT_HOSTNAME;

void hostname_get(char* restrict buffer, size_t size)
{
    bool intr_state = spinlock_acquire_intr(&hostname_lock);
    kstrncpy(buffer, hostname, size);
    spinlock_release_intr(&hostname_lock, intr_state);

    buffer[size - 1] = 0;
}

int hostname_set(const char* restrict name, size_t length)
{
    if(length > HOSTNAME_MAX) {
        return -EINVAL;
    }

    bool intr_state = spinlock_acquire_intr(&hostname_lock);
    memcpy(hostname, name, length);
    hostname[length] = 0;
    spinlock_release_intr(&hostname_lock, intr_state);

    return 0;
}
