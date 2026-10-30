#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/errno.h>
#include <vex/limits.h>
#include <vex/process.h>
#include <vex/rlimit.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/string.h>

// One object serves every process that has
// not changed a limit since a fork, so a fork
// copies nothing. Nobody changes an object that
// two processes share: a change makes a new one
struct rlimits_s {
    unsigned refcount;
    rlimit_t limits[RLIMIT_NLIMITS];
};

static_assert(sizeof(rlimits_t) <= SLAB_MAX_SIZE);

static_assert(0 == (RLIMIT_STACK_DEFAULT & (RLIMIT_STACK_DEFAULT - 1)), "CONFIG_USER_STACK_SIZE has to be a power of two");

// The hard limit of the number of descriptors is the size that the table may reach
static bool above_kernel_ceiling(unsigned resource, const rlimit_t* restrict limit)
{
    return resource == RLIMIT_NOFILE && limit->rlim_max > CONFIG_MAX_FDS;
}

rlimits_t* rlimits_create(void)
{
    rlimits_t* rlimits = slab_alloc(sizeof(rlimits_t));

    if(rlimits == nullptr) {
        return nullptr;
    }

    rlimits->refcount = 1;

    for(unsigned resource = 0; resource < RLIMIT_NLIMITS; resource += 1) {
        rlimits->limits[resource].rlim_cur = RLIM_INFINITY;
        rlimits->limits[resource].rlim_max = RLIM_INFINITY;
    }

    rlimits->limits[RLIMIT_NOFILE].rlim_cur = CONFIG_MAX_FDS;
    rlimits->limits[RLIMIT_NOFILE].rlim_max = CONFIG_MAX_FDS;
    rlimits->limits[RLIMIT_STACK].rlim_cur = RLIMIT_STACK_DEFAULT;
    rlimits->limits[RLIMIT_SIGPENDING].rlim_cur = RLIMIT_SIGPENDING_DEFAULT;
    rlimits->limits[RLIMIT_SIGPENDING].rlim_max = RLIMIT_SIGPENDING_CEILING;

    // Nobody lowers the nice value or gets a real-time
    // priority without a capability, and these say so
    rlimits->limits[RLIMIT_NICE].rlim_cur = 0;
    rlimits->limits[RLIMIT_NICE].rlim_max = 0;
    rlimits->limits[RLIMIT_RTPRIO].rlim_cur = 0;
    rlimits->limits[RLIMIT_RTPRIO].rlim_max = 0;

    return rlimits;
}

void rlimits_incref(rlimits_t* restrict rlimits)
{
    atomic_fetch_add(&rlimits->refcount, 1, ATOMIC_RELAXED);
}

void rlimits_decref(rlimits_t* restrict rlimits)
{
    if(rlimits == nullptr) {
        return;
    }

    if(1 == atomic_fetch_sub(&rlimits->refcount, 1, ATOMIC_ACQ_REL)) {
        slab_free(rlimits);
    }
}

rlimits_t* process_get_rlimits(process_t* restrict process)
{
    bool intr_state = spinlock_acquire_intr(&process->lock);

    rlimits_t* rlimits = process->rlimits;
    rlimits_incref(rlimits);

    spinlock_release_intr(&process->lock, intr_state);

    return rlimits;
}

rlimit_t process_get_rlimit(process_t* restrict process, unsigned resource)
{
    kassert(resource < RLIMIT_NLIMITS);

    bool intr_state = spinlock_acquire_intr(&process->lock);
    rlimit_t limit = process->rlimits->limits[resource];
    spinlock_release_intr(&process->lock, intr_state);

    return limit;
}

int process_set_rlimit(process_t* restrict process, unsigned resource, const rlimit_t* restrict limit, bool privileged)
{
    kassert(resource < RLIMIT_NLIMITS);

    if(limit->rlim_cur > limit->rlim_max) {
        return -EINVAL;
    }

    if(above_kernel_ceiling(resource, limit)) {
        return -EPERM;
    }

    rlimits_t* replacement = slab_alloc(sizeof(rlimits_t));

    if(replacement == nullptr) {
        return -ENOMEM;
    }

    bool intr_state = spinlock_acquire_intr(&process->lock);

    rlimits_t* old = process->rlimits;

    if(limit->rlim_max > old->limits[resource].rlim_max && !privileged) {
        spinlock_release_intr(&process->lock, intr_state);
        slab_free(replacement);
        return -EPERM;
    }

    memcpy(replacement, old, sizeof(rlimits_t));
    replacement->refcount = 1;
    replacement->limits[resource] = *limit;
    process->rlimits = replacement;

    spinlock_release_intr(&process->lock, intr_state);

    rlimits_decref(old);

    return 0;
}
