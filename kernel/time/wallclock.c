#include <limine.h>
#include <vex/calendar.h>
#include <vex/clocksrc.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/rtc.h>
#include <vex/seqlock.h>
#include <vex/string.h>
#include <vex/wallclock.h>

#define UPTIME_SEC_MAX    ((int64_t)(UINT64_MAX / NSEC_PER_SEC) + 1)
#define WALLCLOCK_SEC_MAX ((int64_t)(INT64_MAX - UPTIME_SEC_MAX))

#define RTC_DRIFT_SEC_MAX 2
#define RTC_ZONE_STEP_SEC 900 // Time zones are whole quarter hours apart

static volatile __used __section(".limine_requests") struct limine_date_at_boot_request date_request = {
    .id = LIMINE_DATE_AT_BOOT_REQUEST_ID,
    .revision = 0,
};

static timespec_t wallclock_offset = {};
static seqlock_t wallclock_seqlock = SEQLOCK_INIT;
static bool date_known = false; // Set at boot or by someone, so an RTC that comes later only checks it

// Triggered under set_lock after each wallclock_set,
// so a waiter that computed its timeout under the lock can't miss it
static event_t set_event;
static spinlock_t set_lock = SPINLOCK_INIT;

static void store_offset(const timespec_t* restrict offset)
{
    bool intr_state = seqlock_write_begin(&wallclock_seqlock);
    wallclock_offset = *offset;
    seqlock_write_end(&wallclock_seqlock, intr_state);
}

static void step_to(const timespec_t* restrict time)
{
    timespec_t monotonic;
    timespec_from_nsec(&monotonic, clock_source_now_ns());

    timespec_t offset;
    offset.tv_sec = time->tv_sec - monotonic.tv_sec;
    offset.tv_nsec = time->tv_nsec - monotonic.tv_nsec;

    if(offset.tv_nsec < 0) {
        offset.tv_nsec += NSEC_PER_SEC;
        offset.tv_sec -= 1;
    }

    store_offset(&offset);
}

static void inform_date(const char* restrict source, time_t seconds)
{
    calendar_time_t calendar;
    calendar_from_time(seconds, &calendar);

    pr_inform("wallclock: %s %04lld-%02u-%02u %02u:%02u:%02u UTC", source, calendar.year, calendar.month, calendar.day, calendar.hour,
        calendar.minute, calendar.second);
}

static bool seconds_valid(int64_t seconds)
{
    return seconds >= 0 && seconds <= WALLCLOCK_SEC_MAX;
}

static void check_rtc_drift(time_t rtc_seconds)
{
    timespec_t now;
    wallclock_get(&now);

    long long drift = (long long)(rtc_seconds - now.tv_sec);
    long long distance = drift;

    if(distance < 0) {
        distance = -distance;
    }

    if(distance <= RTC_DRIFT_SEC_MAX) {
        return;
    }

    pr_warning("wallclock: the RTC has drifted %lld seconds off the date at boot", drift);

    int64_t zone_rest = (int64_t)(distance % RTC_ZONE_STEP_SEC);

    if(zone_rest <= RTC_DRIFT_SEC_MAX || zone_rest >= RTC_ZONE_STEP_SEC - RTC_DRIFT_SEC_MAX) {
        pr_warning("wallclock: the RTC may be kept in local time instead of UTC");
    }
}

void wallclock_get(timespec_t* restrict out)
{
    timespec_t offset;
    unsigned sequence;

    do {
        sequence = seqlock_read_begin(&wallclock_seqlock);
        offset = wallclock_offset;
    } while(seqlock_read_retry(&wallclock_seqlock, sequence));

    timespec_t monotonic;
    timespec_from_nsec(&monotonic, clock_source_now_ns());

    out->tv_sec = offset.tv_sec + monotonic.tv_sec;
    out->tv_nsec = offset.tv_nsec + monotonic.tv_nsec;

    if(out->tv_nsec >= NSEC_PER_SEC) {
        out->tv_nsec -= NSEC_PER_SEC;
        out->tv_sec += 1;
    }
}

int wallclock_set(const timespec_t* restrict time)
{
    if(!seconds_valid(time->tv_sec)) {
        return -EINVAL;
    }

    if(time->tv_nsec < 0 || time->tv_nsec >= NSEC_PER_SEC) {
        return -EINVAL;
    }

    bool intr_state = spinlock_acquire_intr(&set_lock);

    step_to(time);
    date_known = true;

    event_trigger(&set_event, false);
    spinlock_release_intr(&set_lock, intr_state);

    // The system clock is set by now, and failing to keep
    // the time over a reboot is no reason to undo that
    int retval = rtc_write(time->tv_sec);

    if(retval < 0 && retval != -ENODEV) {
        pr_warning("wallclock: writing the RTC failed: %s", strerror(retval));
    }

    return 0;
}

ssize_t wallclock_wait_until(const timespec_t* restrict target)
{
    for(;;) {
        bool intr_state = spinlock_acquire_intr(&set_lock);

        timespec_t now;
        wallclock_get(&now);

        if(target->tv_sec < now.tv_sec || (target->tv_sec == now.tv_sec && target->tv_nsec <= now.tv_nsec)) {
            spinlock_release_intr(&set_lock, intr_state);
            return 0;
        }

        timespec_t left;
        left.tv_sec = target->tv_sec - now.tv_sec;
        left.tv_nsec = target->tv_nsec - now.tv_nsec;

        if(left.tv_nsec < 0) {
            left.tv_nsec += NSEC_PER_SEC;
            left.tv_sec -= 1;
        }

        event_t* event = &set_event;
        ssize_t result = event_await_unlock(&event, 1, &set_lock, intr_state, timespec_to_nsec(&left));

        // Either the clock was set or the time
        // ran out; the next round tells which
        if(result != 0 && result != -ETIMEDOUT) {
            return result;
        }
    }
}

time_t wallclock_now(void)
{
    timespec_t now;
    wallclock_get(&now);
    return now.tv_sec;
}

void wallclock_rtc_arrived(void)
{
    time_t rtc_seconds;

    if(rtc_read(&rtc_seconds) < 0 || !seconds_valid(rtc_seconds)) {
        pr_warning("wallclock: the RTC holds no valid time");
        return;
    }

    if(date_known) {
        check_rtc_drift(rtc_seconds);
        return;
    }

    timespec_t now;
    now.tv_sec = rtc_seconds;
    now.tv_nsec = 0;
    step_to(&now);
    date_known = true;

    inform_date("the RTC says", rtc_seconds);
}

// The RTCs are in the device tree, which comes later
static void init_wallclock(void)
{
    struct limine_date_at_boot_response* response = date_request.response;

    event_init(&set_event);

    if(response == nullptr || !seconds_valid(response->timestamp)) {
        pr_warning("wallclock: no date at boot, starting at the epoch until an RTC says otherwise");
        return;
    }

    timespec_t offset;
    offset.tv_sec = response->timestamp;
    offset.tv_nsec = 0;

    store_offset(&offset);
    date_known = true;

    inform_date("date at boot is", (time_t)(response->timestamp));
}

initcall_bsp_timer(init_wallclock, INIT_LAST);
