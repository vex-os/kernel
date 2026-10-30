#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/clocksrc.h>
#include <vex/cmdline.h>
#include <vex/cycles.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/hart.h>
#include <vex/initcall.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/random.h>
#include <vex/random_source.h>
#include <vex/sched.h>
#include <vex/spinlock.h>
#include <vex/sprintf.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/time.h>

#include "random_internal.h"

#define RANDOM_MIN_POLL_MS 100
#define MAX_FAILURES       5
#define OPTION_SIZE        64

#define EVENT_VALUES     32
#define MAX_CREDIT_UNITS (256 * RANDOM_SOURCE_CREDIT_UNITS)

#define MIN_POLL_INTERVAL (RANDOM_MIN_POLL_MS * NSEC_PER_MSEC)

typedef struct event_buffer_s event_buffer_t;

struct event_buffer_s {
    spinlock_t lock;
    size_t count; // Events since the last drain; past EVENT_VALUES, new events mix into the old slots
    unsigned credit_units;
    uint64_t values[EVENT_VALUES];
};

static spinlock_t poll_lock = SPINLOCK_INIT;

static event_t worker_wakeup;
static event_buffer_t* event_buffers; // One per hart, published once the thread runs
static unsigned spare_units;          // Owned by the thread

static void absorb(random_source_t* restrict source, const uint8_t* sample, ssize_t size)
{
    if(size == -EAGAIN) {
        return;
    }

    if(size == -ENODEV) {
        source->priv.usable = false;
        return;
    }

    if(size < 0) {
        source->priv.failures += 1;

        if(source->priv.failures >= MAX_FAILURES) {
            source->priv.usable = false;
            pr_warning("random: source %s failed %u times, it is out", source->name, source->priv.failures);
        }

        return;
    }

    source->priv.failures = 0;

    unsigned bits = 0;

    if(source->priv.credited) {
        bits = source->sample_bits * size / source->sample_size;
    }

    random_add(sample, size, bits);
}

static void poll_source(random_source_t* restrict source, uint64_t now_ns)
{
    uint8_t sample[RANDOM_SOURCE_SAMPLE_MAX];

    ssize_t size = source->poll(sample, source->sample_size);

    source->priv.next_poll_ns = now_ns + MIN_POLL_INTERVAL;

    absorb(source, sample, size);

    memzero_explicit(sample, sizeof(sample));
}

static void init_source(random_source_t* restrict source)
{
    char option[OPTION_SIZE];

    if(source->poll) {
        kassert(source->sample_size > 0 && source->sample_size <= RANDOM_SOURCE_SAMPLE_MAX);

        source->priv.usable = source->probe();
    }
    else {
        kassert(source->events_per_bit > 0 && source->events_per_bit <= RANDOM_SOURCE_CREDIT_UNITS);

        source->priv.usable = true;
    }

    source->priv.credited = source->trusted;

    snprintf(option, sizeof(option), "random.trust_%s", source->name);

    const char* argument = cmdline_argument(&sys_cmdline, option);

    if(argument && 0 == strcmp(argument, "0")) {
        source->priv.credited = false;
    }
    else if(argument) {
        source->priv.credited = true;
    }

    pr_inform("random: source %s: %s", source->name, source->priv.usable ? "found" : "absent");
}

void random_source_poll_all(void)
{
    if(!spinlock_try_acquire(&poll_lock)) {
        return;
    }

    uint64_t now_ns = clock_source_now_ns();

    for(random_source_t* it = random_sources_start; it < random_sources_end; it += 1) {
        bool due = it->priv.usable && now_ns >= it->priv.next_poll_ns;

        if(it->poll && due) {
            poll_source(it, now_ns);
        }
    }

    spinlock_release(&poll_lock);
}

// Values only, so that a burst of events does not cost more than one buffer of hashing
static void drain_buffer(event_buffer_t* restrict buffer)
{
    uint64_t values[EVENT_VALUES];

    bool intr_state = spinlock_acquire_intr(&buffer->lock);

    size_t count = buffer->count;
    unsigned units = buffer->credit_units;

    if(count > EVENT_VALUES) {
        count = EVENT_VALUES;
    }

    memcpy(values, buffer->values, sizeof(values));
    memset(buffer->values, 0, sizeof(buffer->values));

    buffer->count = 0;
    buffer->credit_units = 0;

    spinlock_release_intr(&buffer->lock, intr_state);

    if(count) {
        spare_units += units;

        unsigned bits = spare_units / RANDOM_SOURCE_CREDIT_UNITS;
        spare_units %= RANDOM_SOURCE_CREDIT_UNITS;

        random_add(values, count * sizeof(values[0]), bits);
    }

    memzero_explicit(values, sizeof(values));
}

static void __noreturn random_worker(void* restrict arg)
{
    event_t* events[] = { &worker_wakeup };

    for(;;) {
        // Without memory to listen, the thread goes on and does its work on the next wakeup
        (void)(event_await_blocking(events, 1));

        for(unsigned long i = 0; i < num_harts; ++i) {
            drain_buffer(&event_buffers[i]);
        }

        if(random_needs_noise()) {
            random_source_poll_all();
        }
    }
}

void random_source_add_event(random_source_t* restrict source, uint64_t value)
{
    event_buffer_t* buffers = atomic_load(&event_buffers, ATOMIC_ACQUIRE);

    if(buffers == nullptr) {
        return;
    }

    hart_local_t* hart = this_hart();
    event_buffer_t* buffer = &buffers[hart->index];
    uint64_t now = local_clock_ns();
    uint64_t mixed = now ^ (value << 32) ^ (value >> 32);

    raw_spinlock_acquire(&buffer->lock);

    buffer->values[buffer->count % EVENT_VALUES] ^= mixed;
    buffer->count += 1;

    if(source->priv.credited && buffer->credit_units < MAX_CREDIT_UNITS) {
        buffer->credit_units += RANDOM_SOURCE_CREDIT_UNITS / source->events_per_bit;
    }

    bool full = buffer->count == EVENT_VALUES;

    raw_spinlock_release(&buffer->lock);

    if(full) {
        event_trigger(&worker_wakeup, true);
    }
}

void random_source_wake(void)
{
    event_trigger(&worker_wakeup, true);
}

static void init_random_events(void)
{
    event_init(&worker_wakeup);
}

initcall_bsp_early(init_random_events, INIT_FIRST);

static void init_random_sources(void)
{
    for(random_source_t* it = random_sources_start; it < random_sources_end; it += 1) {
        init_source(it);
    }

    random_source_poll_all();
}

initcall_bsp_bootinfo(init_random_sources, INIT_LAST);

static void init_random_worker(void)
{
    event_buffer_t* buffers = kmalloc(num_harts * sizeof(event_buffer_t));

    if(buffers == nullptr) {
        pr_warning("random: out of memory, events are not collected");
        return;
    }

    memset(buffers, 0, num_harts * sizeof(event_buffer_t));

    if(!thread_spawn_kernel(&random_worker, nullptr)) {
        pr_warning("random: out of memory, events are not collected");
        kfree(buffers);
        return;
    }

    atomic_store(&event_buffers, buffers, ATOMIC_RELEASE);
}

initcall_bsp_services(init_random_worker, INIT_FIRST);
