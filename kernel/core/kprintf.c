#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/clocksrc.h>
#include <vex/cmdline.h>
#include <vex/console.h>
#include <vex/event.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/panic.h>
#include <vex/spinlock.h>
#include <vex/sprintf.h>
#include <vex/string.h>
#include <vex/strings.h>

#ifndef KP_DEFAULT_LOGLEVEL
#define KP_DEFAULT_LOGLEVEL KP_INFORM
#endif

#define RECORD_ALIGN 8U
#define PADDING_SIZE UINT16_MAX // The size of a record that only fills the end of the ring

static_assert(KP_LINE_MAX < PADDING_SIZE);
static_assert(0 == sizeof(kp_record_t) % RECORD_ALIGN);
static_assert(0 == KP_BUFSIZE % RECORD_ALIGN, "CONFIG_KPRINTF_BUFSIZE must be a multiple of 8");

// The padding in front of a record at the end of the ring is shorter than the record
static_assert(KP_BUFSIZE >= 2U * (sizeof(kp_record_t) + KP_LINE_MAX), "CONFIG_KPRINTF_BUFSIZE can't hold the longest record");

unsigned loglevel = KP_DEFAULT_LOGLEVEL;
event_t kprintf_appended = {};

static event_deferral_t appended_deferral = {
    .event = &kprintf_appended,
};

static spinlock_t kprintf_lock = SPINLOCK_INIT;

// Records lie back to back between the tail and the head, and the positions only grow;
// the head moves only once a record is whole, so a panic never reads one half written
static unsigned char ring[KP_BUFSIZE] __aligned(RECORD_ALIGN);
static uint64_t ring_head = 0U;
static uint64_t ring_tail = 0U;
static uint64_t first_seq = 0U;
static uint64_t next_seq = 0U;

// Under kprintf_lock; a panic overwrites it
// but the message it held is never finished
static char line[KP_LINE_MAX + 1U];

// A panic has stopped the other harts, so a
// holder of the lock is either stopped or this hart
static bool ring_lock(void)
{
    if(atomic_load_n(&panic_in_progress, ATOMIC_ACQUIRE)) {
        return false;
    }

    raw_spinlock_acquire(&kprintf_lock);

    return true;
}

static void ring_unlock(bool locked)
{
    if(locked) {
        raw_spinlock_release(&kprintf_lock);
    }
}

static size_t record_footprint(size_t text_size)
{
    size_t size = sizeof(kp_record_t) + text_size;
    return (size + RECORD_ALIGN - 1U) & ~(size_t)(RECORD_ALIGN - 1U);
}

// The position of the record at position, past
// the padding that may end the ring in front of it
static uint64_t skip_padding(uint64_t position)
{
    size_t offset = position % KP_BUFSIZE;
    size_t remaining = KP_BUFSIZE - offset;

    if(remaining < sizeof(kp_record_t)) {
        return position + remaining;
    }

    kp_record_t record;
    memcpy(&record, &ring[offset], sizeof(record));

    if(record.size == PADDING_SIZE) {
        return position + remaining;
    }

    return position;
}

// The ring keeps no padding behind its last record,
// so dropping every record leaves the tail at the head
static void drop_oldest_locked(void)
{
    kassert(first_seq < next_seq);

    ring_tail = skip_padding(ring_tail);

    kp_record_t record;
    memcpy(&record, &ring[ring_tail % KP_BUFSIZE], sizeof(record));

    ring_tail += record_footprint(record.size);
    first_seq += 1U;
}

static void append_locked(unsigned subsystem, unsigned level, unsigned flags, uint64_t time_ns, const char* restrict text, size_t size)
{
    size_t footprint = record_footprint(size);
    size_t offset = ring_head % KP_BUFSIZE;
    size_t padding = 0U;

    if(KP_BUFSIZE - offset < footprint) {
        padding = KP_BUFSIZE - offset;
    }

    while(KP_BUFSIZE - (ring_head - ring_tail) < padding + footprint) {
        drop_oldest_locked();
    }

    if(padding >= sizeof(kp_record_t)) {
        kp_record_t marker = {};
        marker.size = PADDING_SIZE;
        memcpy(&ring[offset], &marker, sizeof(marker));
    }

    offset = (ring_head + padding) % KP_BUFSIZE;

    kp_record_t record = {};
    record.seq = next_seq;
    record.time_ns = time_ns;
    record.size = (uint16_t)(size);
    record.subsystem = (uint16_t)(subsystem);
    record.level = (uint8_t)(level);
    record.flags = (uint8_t)(flags);

    memcpy(&ring[offset], &record, sizeof(record));
    memcpy(&ring[offset + sizeof(record)], text, size);

    ring_head += padding + footprint;
    atomic_store_n(&next_seq, next_seq + 1U, ATOMIC_RELEASE);
}

static uint64_t record_time_ns(bool locked)
{
    if(locked) {
        return clock_source_now_ns();
    }

    return clock_source_peek_ns();
}

static void append_done(bool locked, bool intr_state)
{
    ring_unlock(locked);

    event_trigger_deferred(&appended_deferral);

    console_flush();

    restore_interrupts(intr_state);
}

void kprintf(unsigned subsystem, unsigned level, const char* restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(subsystem, level, fmt, ap);
    va_end(ap);
}

void kvprintf(unsigned subsystem, unsigned level, const char* restrict fmt, va_list ap)
{
    bool intr_state = disable_interrupts();
    bool locked = ring_lock();
    uint64_t timestamp = record_time_ns(locked);

    unsigned flags = 0U;
    int length = vsnprintf(line, sizeof(line), fmt, ap);
    size_t size = 0U;

    if(length > 0) {
        size = (size_t)(length);
    }

    if(size > KP_LINE_MAX) {
        size = KP_LINE_MAX;
        flags |= KPLOG_FLAG_TRUNCATED;
    }

    while(size && (line[size - 1U] == '\n' || line[size - 1U] == '\r')) {
        size -= 1U;
    }

    append_locked(subsystem, level, flags, timestamp, line, size);

    append_done(locked, intr_state);
}

void kputs(unsigned subsystem, unsigned level, unsigned flags, const char* restrict text, size_t size)
{
    kassert(size <= KP_LINE_MAX);

    bool intr_state = disable_interrupts();
    bool locked = ring_lock();

    uint64_t time_ns = record_time_ns(locked);

    append_locked(subsystem, level, flags, time_ns, text, size);

    append_done(locked, intr_state);
}

void kprintf_seek_first(kp_cursor_t* restrict cursor)
{
    bool intr_state = disable_interrupts();
    bool locked = ring_lock();

    cursor->seq = first_seq;
    cursor->position = ring_tail;

    ring_unlock(locked);
    restore_interrupts(intr_state);
}

void kprintf_seek_end(kp_cursor_t* restrict cursor)
{
    bool intr_state = disable_interrupts();
    bool locked = ring_lock();

    cursor->seq = next_seq;
    cursor->position = ring_head;

    ring_unlock(locked);
    restore_interrupts(intr_state);
}

bool kprintf_seek_seq(kp_cursor_t* restrict cursor, uint64_t seq)
{
    bool intr_state = disable_interrupts();
    bool locked = ring_lock();
    bool valid = seq <= next_seq;

    if(valid) {
        uint64_t position = ring_tail;

        // NOTE: The walk is linear in the ring, with interrupts off
        for(uint64_t skipped = first_seq; skipped < seq; skipped += 1) {
            position = skip_padding(position);

            kp_record_t record;
            memcpy(&record, &ring[position % KP_BUFSIZE], sizeof(record));

            position += record_footprint(record.size);
        }

        cursor->seq = seq;
        cursor->position = position;
    }

    ring_unlock(locked);
    restore_interrupts(intr_state);

    return valid;
}

void kprintf_get_range(uint64_t* restrict first, uint64_t* restrict next)
{
    bool intr_state = disable_interrupts();
    bool locked = ring_lock();

    *first = first_seq;
    *next = next_seq;

    ring_unlock(locked);
    restore_interrupts(intr_state);
}

uint64_t kprintf_next_seq(void)
{
    return atomic_load_n(&next_seq, ATOMIC_ACQUIRE);
}

bool kprintf_read(kp_cursor_t* restrict cursor, kp_record_t* restrict record, char* restrict text)
{
    bool intr_state = disable_interrupts();
    bool locked = ring_lock();

    if(cursor->seq < first_seq) {
        cursor->seq = first_seq;
        cursor->position = ring_tail;
    }

    bool found = cursor->seq < next_seq;

    if(found) {
        uint64_t position = skip_padding(cursor->position);
        size_t offset = position % KP_BUFSIZE;

        memcpy(record, &ring[offset], sizeof(*record));
        memcpy(text, &ring[offset + sizeof(*record)], record->size);

        kassert(record->seq == cursor->seq);

        cursor->seq = record->seq + 1U;
        cursor->position = position + record_footprint(record->size);
    }

    ring_unlock(locked);
    restore_interrupts(intr_state);

    return found;
}

static void init_kprintf(void)
{
    event_init(&kprintf_appended);

    const char* argument = cmdline_argument(&sys_cmdline, "loglevel");

    if(argument) {
        if(0 == strcasecmp(argument, "verbose")) {
            loglevel = KP_VERBOSE;
        }
        else if(0 == strcasecmp(argument, "inform")) {
            loglevel = KP_INFORM;
        }
        else if(0 == strcasecmp(argument, "notice")) {
            loglevel = KP_NOTICE;
        }
        else if(0 == strcasecmp(argument, "warning")) {
            loglevel = KP_WARNING;
        }
        else if(0 == strcasecmp(argument, "error")) {
            loglevel = KP_ERROR;
        }
        else if(0 == strcasecmp(argument, "critical")) {
            loglevel = KP_CRITICAL;
        }
        else if(0 == strcasecmp(argument, "alert")) {
            loglevel = KP_ALERT;
        }
    }
}

initcall_bsp_bootinfo(init_kprintf, INIT_LAST);
