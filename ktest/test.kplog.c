#include <vex/capability.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/futex.h>
#include <vex/kplog.h>
#include <vex/syscall.h>
#include <vex/time.h>

#include "ktest.h"

#define KPLOG_POKE "kplog poke"

static volatile unsigned int never_changes;
static long poke_fd;

/* Sleeps so the reader blocks first, then logs a line */
[[noreturn]] static void kplog_poker(long arg)
{
    struct __k_timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 100000000;
    ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes, 0, (long)&delay, 0);
    ktest_write(poke_fd, KPLOG_POKE, sizeof(KPLOG_POKE) - 1);
    ktest_syscall1(__k_SYS_THREAD_EXIT, 0);
    __builtin_unreachable();
}

/* Records come in whole, aligned to __k_FEED_ALIGN; a read may give several */
static unsigned long long kplog_buffer[4096 / sizeof(unsigned long long)];
static long kplog_offset;
static long kplog_length;

/* Drops what kplog_next has read ahead, as after a seek */
static void kplog_forget(void)
{
    kplog_offset = 0;
    kplog_length = 0;
}

/* The next record from the file, read when the buffer runs out; null with the read's result in *result */
static const struct __k_kplog_record* kplog_next(long fd, long* result)
{
    const struct __k_kplog_record* record;

    if(kplog_offset == kplog_length) {
        kplog_forget();
        *result = ktest_syscall3(__k_SYS_READ, fd, (long)kplog_buffer, sizeof(kplog_buffer));

        if(*result <= 0) {
            return 0;
        }

        kplog_length = *result;
    }

    record = (const struct __k_kplog_record*)((const char*)kplog_buffer + kplog_offset);
    kplog_offset += record->feed.size;
    *result = record->feed.size;

    return record;
}

/* Whether the record is a userspace one with exactly this text */
static int kplog_has_text(const struct __k_kplog_record* record, const char* text)
{
    long length = ktest_strlen(text);

    return record->subsystem == __k_KP_USER && (long)record->text_size == length && ktest_bytes_equal(record->text, text, length);
}

static void test_kplog(void)
{
    const struct __k_kplog_record* record;
    struct __k_kplog_info info;
    char small[8];
    long fd = ktest_open("/dev/kplog", __k_O_RDONLY, 0);
    long length, nonblock, seek_bad, seek_end, set_level;
    __k_uint64_t seq;
    __k_int32_t level = -1;
    long reads = 0;
    int found = 0;

    ktest_assert(fd >= 0);
    poke_fd = ktest_open("/dev/kplog", __k_O_WRONLY, 0);
    ktest_assert(poke_fd >= 0);

    ktest_syscall3(__k_SYS_IOCTL, fd, __k_KPLOG_GET_INFO, (long)&info);

    record = kplog_next(fd, &length);
    ktest_assert(record);
    ktest_expect(record->feed.size % __k_FEED_ALIGN == 0 && record->feed.type == __k_FEED_TYPE_KPLOG, 1);
    ktest_expect(record->feed.seq == info.seq_first, 1); // the oldest record the ring holds

    kplog_forget();
    ktest_expect(ktest_syscall3(__k_SYS_READ, fd, (long)small, sizeof(small)), -__k_EINVAL); // too small for a record

    ktest_syscall3(__k_SYS_IOCTL, fd, __k_KPLOG_GET_INFO, (long)&info);
    ktest_expect(info.seq_first <= info.seq_next && info.seq_next > 1, 1);

    seq = info.seq_next - 1;
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, fd, __k_KPLOG_SEEK_SEQ, (long)&seq), 0);
    kplog_forget();
    record = kplog_next(fd, &length);
    ktest_expect(record && record->feed.seq == seq, 1);

    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, fd, __k_KPLOG_GET_CONSOLE_LEVEL, (long)&level), 0);
    ktest_expect(level >= __k_KP_EMERG && level <= __k_KP_VERBOSE, 1);

    /* Nothing logs between the seeks and the reads, so the results wait until after them */
    set_level = ktest_syscall3(__k_SYS_IOCTL, fd, __k_KPLOG_SET_CONSOLE_LEVEL, (long)&level);
    seq = info.seq_next + 1000;
    seek_bad = ktest_syscall3(__k_SYS_IOCTL, fd, __k_KPLOG_SEEK_SEQ, (long)&seq);
    seek_end = ktest_syscall3(__k_SYS_IOCTL, fd, __k_KPLOG_SEEK_END, 0);
    kplog_forget();
    ktest_syscall3(__k_SYS_FCNTL, fd, __k_F_SETFL, __k_O_NONBLOCK);
    kplog_next(fd, &nonblock);
    ktest_syscall3(__k_SYS_FCNTL, fd, __k_F_SETFL, 0);
    ktest_spawn_joinable(&kplog_poker, 0);

    /* The kernel may log something else first */
    while(!found && reads < 16) {
        record = kplog_next(fd, &length);
        found = record && kplog_has_text(record, KPLOG_POKE);
        reads += 1;
    }

    ktest_join();

    ktest_expect(set_level, -__k_EPERM);
    ktest_expect(seek_bad, -__k_EINVAL);
    ktest_expect(seek_end, 0);
    ktest_expect(nonblock, -__k_EAGAIN);
    ktest_expect(found, 1);

    ktest_close(poke_fd);
    ktest_close(fd);
}

/* Whether boot logged from memory management and from the device tree, each with its subsystem */
static void test_kplog_subsystems(void)
{
    const struct __k_kplog_record* record;
    long fd = ktest_open("/dev/kplog", __k_O_RDONLY | __k_O_NONBLOCK, 0);
    long result;
    int seen_mm = 0;
    int seen_device = 0;

    kplog_forget();

    while((record = kplog_next(fd, &result))) {
        seen_mm |= record->subsystem == __k_KP_MM;
        seen_device |= record->subsystem == __k_KP_DEVICE;
    }

    ktest_expect(seen_mm && seen_device, 1);

    ktest_close(fd);
}

/* Reads records without blocking until a userspace one has exactly the level and the text; 0 when none does */
static int kplog_find(long fd, long level, const char* text)
{
    const struct __k_kplog_record* record;
    long length = ktest_strlen(text);
    long result;

    while((record = kplog_next(fd, &result))) {
        if(record->subsystem == __k_KP_USER && record->level == level && (long)record->text_size == length
            && ktest_bytes_equal(record->text, text, length)) {
            return 1;
        }
    }

    return 0;
}

static void test_kplog_write(void)
{
    char too_long[600];
    int wstatus = -1;
    long child;
    long reader = ktest_open("/dev/kplog", __k_O_RDONLY | __k_O_NONBLOCK, 0);
    long writer = ktest_open("/dev/kplog", __k_O_WRONLY, 0);
    long i;

    for(i = 0; i < (long)sizeof(too_long); ++i) {
        too_long[i] = 'x';
    }

    ktest_expect(reader >= 0 && writer >= 0, 1);
    ktest_syscall3(__k_SYS_IOCTL, reader, __k_KPLOG_SEEK_END, 0);
    kplog_forget();

    ktest_expect(ktest_syscall3(__k_SYS_WRITE, writer, (long)"user line\r\n", 11), 11);
    ktest_expect(ktest_syscall3(__k_SYS_WRITE, writer, (long)"<4>warned\n", 10), 10);
    ktest_expect(ktest_syscall3(__k_SYS_WRITE, writer, (long)"<14>syslog user", 15), 15);
    ktest_expect(ktest_syscall3(__k_SYS_WRITE, writer, (long)"tab\there\\", 9), 9);
    ktest_expect(ktest_syscall3(__k_SYS_WRITE, writer, (long)"<2>critical", 11), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_WRITE, writer, (long)"two\nlines", 9), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_WRITE, writer, (long)"nul\0byte", 8), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_WRITE, writer, (long)too_long, sizeof(too_long)), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_WRITE, reader, (long)"read only", 9), -__k_EBADF);

    /* A <N> prefix sets only the level, and the text comes as it was written */
    ktest_expect(kplog_find(reader, __k_KP_NOTICE, "user line"), 1);
    ktest_expect(kplog_find(reader, __k_KP_WARNING, "warned"), 1);
    ktest_expect(kplog_find(reader, __k_KP_INFORM, "syslog user"), 1);
    ktest_expect(kplog_find(reader, __k_KP_NOTICE, "tab\there\\"), 1);

    /* A child without CAP_SYSLOG still reads, and its open for writing exits with the error */
    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        struct __k_caps caps;
        long denied, allowed;

        ktest_syscall1(__k_SYS_CAPGET, (long)&caps);
        caps.effective &= ~__k_CAP_TO_MASK(__k_CAP_SYSLOG);
        ktest_syscall1(__k_SYS_CAPSET, (long)&caps);

        denied = ktest_open("/dev/kplog", __k_O_WRONLY, 0);
        allowed = ktest_open("/dev/kplog", __k_O_RDONLY, 0);
        ktest_syscall1(__k_SYS_EXIT, allowed >= 0 ? -denied : 0);
    }

    ktest_syscall3(__k_SYS_WAITPID, child, (long)&wstatus, 0);
    ktest_expect(wstatus >> 8, __k_EPERM & 0xFF);

    ktest_close(writer);
    ktest_close(reader);
}

int main(void)
{
    test_kplog();
    test_kplog_subsystems();
    test_kplog_write();

    return ktest_result();
}
