#include <vex/assert.h>
#include <vex/clocksrc.h>
#include <vex/cmdline.h>
#include <vex/errno.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/random.h>
#include <vex/random_source.h>
#include <vex/spinlock.h>
#include <vex/sprintf.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/time.h>

#define RANDOM_MIN_POLL_MS 100
#define MAX_FAILURES       5
#define OPTION_SIZE        64

#define MIN_POLL_INTERVAL (RANDOM_MIN_POLL_MS * NSEC_PER_MSEC)

static spinlock_t poll_lock = SPINLOCK_INIT;

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

    kassert(source->sample_size > 0 && source->sample_size <= RANDOM_SOURCE_SAMPLE_MAX);

    source->priv.usable = source->probe();

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
        if(it->priv.usable && now_ns >= it->priv.next_poll_ns) {
            poll_source(it, now_ns);
        }
    }

    spinlock_release(&poll_lock);
}

static void init_random_sources(void)
{
    for(random_source_t* it = random_sources_start; it < random_sources_end; it += 1) {
        init_source(it);
    }

    random_source_poll_all();
}

initcall_bsp_bootinfo(init_random_sources, INIT_LAST);
