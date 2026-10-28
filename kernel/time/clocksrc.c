#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/clocksrc.h>
#include <vex/errno.h>
#include <vex/kprintf.h>
#include <vex/spinlock.h>
#include <vex/time.h>

static clock_source_t* clock_source_head = nullptr;
static clock_source_t* active_clock_source = nullptr;
static spinlock_t clock_source_lock = SPINLOCK_INIT;

static void compute_mult_shift(clock_source_t* restrict source, uint64_t scale, uint64_t frequency)
{
    uint64_t freq = scale * frequency;
    uint64_t nsec_per_sec = NSEC_PER_SEC;

    source->priv.shift = __builtin_clzll(nsec_per_sec);
    source->priv.mult = (nsec_per_sec << source->priv.shift) / freq;
}

static uint64_t update_nolock(void)
{
    if(active_clock_source == nullptr) {
        return UINT64_C(0);
    }

    uint64_t now = active_clock_source->ops->read(active_clock_source) & active_clock_source->mask;
    uint128_t delta;

    if(now >= active_clock_source->priv.last) {
        delta = now - active_clock_source->priv.last;
    }
    else {
        delta = active_clock_source->mask - active_clock_source->priv.last + now + UINT64_C(1);
    }

    delta *= active_clock_source->priv.mult;
    delta >>= active_clock_source->priv.shift;

    active_clock_source->priv.base += delta;
    active_clock_source->priv.last = now;

    return active_clock_source->priv.base;
}

// The new source picks up where the
// old one left off, so time never goes backwards
static void activate_nolock(clock_source_t* restrict source)
{
    uint64_t now_nsec = update_nolock();

    active_clock_source = source;

    if(source) {
        source->priv.last = source->ops->read(source) & source->mask;
        source->priv.base = now_nsec;
    }
}

static bool is_registered_nolock(const clock_source_t* restrict source)
{
    for(clock_source_t* it = clock_source_head; it; it = it->priv.next) {
        if(it == source) {
            return true;
        }
    }

    return false;
}

int clock_source_add_raw(clock_source_t* restrict source, uint64_t scale, uint64_t frequency)
{
    bool intr_state = spinlock_acquire_intr(&clock_source_lock);

    if(is_registered_nolock(source)) {
        spinlock_release_intr(&clock_source_lock, intr_state);
        return -EEXIST;
    }

    compute_mult_shift(source, scale, frequency);

    source->priv.next = clock_source_head;
    clock_source_head = source;

    bool activated = active_clock_source == nullptr || source->rating > active_clock_source->rating;

    if(activated) {
        activate_nolock(source);
    }

    spinlock_release_intr(&clock_source_lock, intr_state);

    pr_verbose("clocksrc: added %s: rating=%u", source->name, source->rating);

    if(activated) {
        pr_verbose("clocksrc: %s is now active", source->name);
    }

    return 0;
}

int clock_source_calibrate_raw(clock_source_t* restrict source, uint64_t scale, uint64_t frequency)
{
    bool intr_state = spinlock_acquire_intr(&clock_source_lock);

    if(!is_registered_nolock(source)) {
        spinlock_release_intr(&clock_source_lock, intr_state);
        return -ENOENT;
    }

    // Ticks counted so far
    // still convert at the old rate
    if(source == active_clock_source) {
        update_nolock();
    }

    compute_mult_shift(source, scale, frequency);

    spinlock_release_intr(&clock_source_lock, intr_state);

    return 0;
}

int clock_source_remove(clock_source_t* restrict source)
{
    bool intr_state = spinlock_acquire_intr(&clock_source_lock);

    clock_source_t** prev = &clock_source_head;

    while(*prev && *prev != source) {
        prev = &(*prev)->priv.next;
    }

    if(*prev == nullptr) {
        spinlock_release_intr(&clock_source_lock, intr_state);
        return -ENOENT;
    }

    clock_source_t* best = nullptr;
    bool was_active = active_clock_source == source;

    for(clock_source_t* it = clock_source_head; it; it = it->priv.next) {
        if(it != source && (best == nullptr || it->rating > best->rating)) {
            best = it;
        }
    }

    if(was_active && best == nullptr) {
        spinlock_release_intr(&clock_source_lock, intr_state);
        return -EBUSY;
    }

    *prev = source->priv.next;

    if(was_active) {
        activate_nolock(best);
    }

    spinlock_release_intr(&clock_source_lock, intr_state);

    if(was_active && best) {
        pr_verbose("clocksrc: %s is now active", best->name);
    }

    return 0;
}

clock_source_t* clock_source_active(void)
{
    return active_clock_source;
}

uint64_t clock_source_now_ns(void)
{
    bool intr_state = spinlock_acquire_intr(&clock_source_lock);
    uint64_t now_nsec = update_nolock();
    spinlock_release_intr(&clock_source_lock, intr_state);
    return now_nsec;
}

uint64_t clock_source_resolution_ns(void)
{
    bool intr_state = spinlock_acquire_intr(&clock_source_lock);

    uint64_t resolution = 1;

    if(active_clock_source) {
        // The tick is mult / 2^shift nanoseconds
        uint128_t tick = (uint128_t)(active_clock_source->priv.mult) + (UINT64_C(1) << active_clock_source->priv.shift) - 1;
        tick >>= active_clock_source->priv.shift;

        if(tick > resolution) {
            resolution = (uint64_t)(tick);
        }
    }

    spinlock_release_intr(&clock_source_lock, intr_state);
    return resolution;
}

uint64_t clock_source_peek_ns(void)
{
    bool intr_state = disable_interrupts();

    if(raw_spinlock_try_acquire(&clock_source_lock)) {
        uint64_t now_nsec = update_nolock();
        raw_spinlock_release(&clock_source_lock);
        restore_interrupts(intr_state);
        return now_nsec;
    }

    restore_interrupts(intr_state);

    clock_source_t* source = atomic_load(&active_clock_source, ATOMIC_ACQUIRE);

    if(source == nullptr) {
        return UINT64_C(0);
    }

    return atomic_load(&source->priv.base, ATOMIC_RELAXED);
}

void clock_source_update(void)
{
    bool intr_state = spinlock_acquire_intr(&clock_source_lock);
    update_nolock();
    spinlock_release_intr(&clock_source_lock, intr_state);
}
