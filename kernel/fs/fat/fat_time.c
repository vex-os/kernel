#include <vex/calendar.h>
#include <vex/endian.h>

#include "fat_internal.h"

#define FAT_YEAR_BASE 1980
#define FAT_TIME_MIN  INT64_C(315532800)  // 1980-01-01 00:00:00
#define FAT_TIME_MAX  INT64_C(4354819199) // 2107-12-31 23:59:59

#define FAT_NSEC_PER_HUNDREDTH (NSEC_PER_SEC / 100)

void fat_time_pack(const timespec_t* restrict time, fat_timestamp_t* restrict out)
{
    int64_t seconds = time->tv_sec;
    int64_t nsec = time->tv_nsec;

    if(seconds < FAT_TIME_MIN) {
        seconds = FAT_TIME_MIN;
        nsec = 0;
    }

    if(seconds > FAT_TIME_MAX) {
        seconds = FAT_TIME_MAX;
        nsec = 0;
    }

    calendar_time_t calendar;
    calendar_from_time(seconds, &calendar);

    unsigned year = (unsigned)(calendar.year - FAT_YEAR_BASE);

    out->date = htole16((uint16_t)(year << 9 | calendar.month << 5 | calendar.day));
    out->time = htole16((uint16_t)(calendar.hour << 11 | calendar.minute << 5 | calendar.second / 2));
    out->tenth = (uint8_t)(calendar.second % 2 * 100 + nsec / FAT_NSEC_PER_HUNDREDTH);
}

void fat_time_unpack(const fat_timestamp_t* restrict stamp, timespec_t* restrict out)
{
    uint16_t date = le16toh(stamp->date);
    uint16_t time = le16toh(stamp->time);

    calendar_time_t calendar;
    calendar.year = FAT_YEAR_BASE + (date >> 9);
    calendar.month = (date >> 5) & 0x0F;
    calendar.day = date & 0x1F;
    calendar.hour = time >> 11;
    calendar.minute = (time >> 5) & 0x3F;
    calendar.second = (time & 0x1F) * 2;

    out->tv_sec = 0;
    out->tv_nsec = 0;

    bool bad_month = calendar.month < 1 || calendar.month > 12;

    if(bad_month || calendar.day < 1 || stamp->tenth > 199) {
        return;
    }

    out->tv_sec = calendar_to_time(&calendar) + stamp->tenth / 100;
    out->tv_nsec = (int64_t)(stamp->tenth % 100) * FAT_NSEC_PER_HUNDREDTH;
}
