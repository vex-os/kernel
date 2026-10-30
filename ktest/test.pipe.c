#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/ioctl.h>
#include <vex/signal.h>
#include <vex/stat.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define PAGE_SIZE   4096
#define PIPE_SIZE   (16 * PAGE_SIZE)
#define SETTLE_NSEC 50000000L

static volatile int sigpipes;
static char pattern[PIPE_SIZE + PAGE_SIZE];
static char readback[PIPE_SIZE + PAGE_SIZE];

static long make_pipe(int* fds, long flags)
{
    return ktest_syscall2(__k_SYS_PIPE, (long)(fds), flags);
}

static void on_sigpipe(__k_int32_t sig)
{
    sigpipes += 1;
}

static void sleep_for(long nanoseconds)
{
    struct __k_timespec request;
    request.tv_sec = nanoseconds / 1000000000L;
    request.tv_nsec = nanoseconds % 1000000000L;

    ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)(&request), 0);
}

static void reap(long child)
{
    int wstatus = -1;

    ktest_syscall3(__k_SYS_WAITPID, child, (long)(&wstatus), 0);
}

static void fill_pattern(void)
{
    for(unsigned i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = (char)(i * 7 + i / 251);
    }
}

// Reads until the buffer is full or the pipe has nothing; stops on a short read as well
static long drain(long fd, char* buffer, size_t size)
{
    size_t total = 0;

    while(total < size) {
        long count = ktest_read(fd, buffer + total, size - total);

        if(count <= 0) {
            break;
        }

        total += count;
    }

    return total;
}

static void test_basic(void)
{
    int fds[2] = {};
    char buffer[16] = {};
    struct __k_stat stat = {};

    ktest_expect(make_pipe(fds, 0), 0);
    ktest_assert(fds[0] >= 0 && fds[1] > fds[0]);

    ktest_expect(ktest_write(fds[1], "hello", 5), 5);
    ktest_expect(ktest_read(fds[0], buffer, sizeof(buffer)), 5); // Hands out what it has
    ktest_expect(ktest_bytes_equal(buffer, "hello", 5), 1);

    // Either end tells how much is waiting
    int waiting = -1;

    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, fds[0], __k_FIONREAD, (long)(&waiting)), 0);
    ktest_expect(waiting, 0);
    ktest_expect(ktest_write(fds[1], pattern, 3 * PAGE_SIZE + 5), 3 * PAGE_SIZE + 5);
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, fds[1], __k_FIONREAD, (long)(&waiting)), 0);
    ktest_expect(waiting, 3 * PAGE_SIZE + 5);
    ktest_expect(ktest_read(fds[0], readback, 100), 100);
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, fds[0], __k_FIONREAD, (long)(&waiting)), 0);
    ktest_expect(waiting, 3 * PAGE_SIZE - 95);
    ktest_expect(drain(fds[0], readback, 3 * PAGE_SIZE - 95), 3 * PAGE_SIZE - 95);
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, fds[0], __k_FIONREAD, 0), -__k_EFAULT);
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, fds[0], __k_FIONREAD + 1, (long)(&waiting)), -__k_ENOTTY);

    // Each end allows only its own direction
    ktest_expect(ktest_write(fds[0], "x", 1), -__k_EBADF);
    ktest_expect(ktest_read(fds[1], buffer, 1), -__k_EBADF);

    ktest_expect(ktest_syscall2(__k_SYS_FSTAT, fds[0], (long)(&stat)), 0);
    ktest_expect(stat.st_mode & __k_S_IFMT, __k_S_IFIFO);
    ktest_expect(stat.st_size, 0);

    // A pipe is a stream
    ktest_expect(ktest_syscall3(__k_SYS_SEEK, fds[0], 0, __k_SEEK_SET), -__k_ESPIPE);
    ktest_expect(ktest_syscall4(__k_SYS_PREAD, fds[0], (long)(buffer), 1, 0), -__k_ESPIPE);
    ktest_expect(ktest_syscall4(__k_SYS_PWRITE, fds[1], (long)("x"), 1, 0), -__k_ESPIPE);

    ktest_close(fds[0]);
    ktest_close(fds[1]);
}

static void test_flags(void)
{
    int fds[2] = {};

    ktest_expect(make_pipe(fds, __k_O_APPEND), -__k_EINVAL);
    ktest_expect(make_pipe(nullptr, 0), -__k_EFAULT);

    // The failed calls left no descriptor behind
    ktest_expect(make_pipe(fds, __k_O_CLOEXEC | __k_O_NONBLOCK), 0);

    long first = fds[0];

    ktest_expect(ktest_syscall2(__k_SYS_FCNTL, fds[0], __k_F_GETFD), __k_FD_CLOEXEC);
    ktest_expect(ktest_syscall2(__k_SYS_FCNTL, fds[1], __k_F_GETFD), __k_FD_CLOEXEC);
    ktest_expect(ktest_syscall2(__k_SYS_FCNTL, fds[0], __k_F_GETFL) & __k_O_NONBLOCK, __k_O_NONBLOCK);
    ktest_close(fds[0]);
    ktest_close(fds[1]);

    ktest_expect(make_pipe(fds, 0), 0);
    ktest_expect(fds[0], first);
    ktest_expect(ktest_syscall2(__k_SYS_FCNTL, fds[0], __k_F_GETFD), 0);
    ktest_close(fds[0]);
    ktest_close(fds[1]);
}

static void test_ends(void)
{
    int fds[2] = {};
    char buffer[8] = {};

    // The end of the file comes with the last writer
    ktest_expect(make_pipe(fds, __k_O_NONBLOCK), 0);
    ktest_expect(ktest_read(fds[0], buffer, sizeof(buffer)), -__k_EAGAIN);

    long extra = ktest_syscall3(__k_SYS_FCNTL, fds[1], __k_F_DUPFD, 0);

    ktest_assert(extra >= 0);
    ktest_expect(ktest_write(fds[1], "ab", 2), 2);
    ktest_close(fds[1]);
    ktest_expect(ktest_read(fds[0], buffer, sizeof(buffer)), 2);
    ktest_expect(ktest_read(fds[0], buffer, sizeof(buffer)), -__k_EAGAIN); // The duplicate still writes
    ktest_close(extra);
    ktest_expect(ktest_read(fds[0], buffer, sizeof(buffer)), 0);
    ktest_close(fds[0]);

    // Data that is left is read before the end of the file shows
    ktest_expect(make_pipe(fds, 0), 0);
    ktest_expect(ktest_write(fds[1], "left", 4), 4);
    ktest_close(fds[1]);
    ktest_expect(ktest_read(fds[0], buffer, sizeof(buffer)), 4);
    ktest_expect(ktest_read(fds[0], buffer, sizeof(buffer)), 0);
    ktest_close(fds[0]);

    // The last reader leaves the writer with SIGPIPE and EPIPE
    ktest_expect(make_pipe(fds, 0), 0);
    ktest_close(fds[0]);
    ktest_expect(ktest_write(fds[1], "x", 1), -__k_EPIPE);
    ktest_expect(sigpipes, 1);
    ktest_close(fds[1]);
}

static void test_full(void)
{
    int fds[2] = {};

    ktest_expect(make_pipe(fds, __k_O_NONBLOCK), 0);

    // A write that does not fit takes what fits; one that finds no room fails
    ktest_expect(ktest_write(fds[1], pattern, sizeof(pattern)), PIPE_SIZE);
    ktest_expect(ktest_write(fds[1], "x", 1), -__k_EAGAIN);

    ktest_expect(drain(fds[0], readback, sizeof(readback)), PIPE_SIZE);
    ktest_expect(memcmp(readback, pattern, PIPE_SIZE), 0);
    ktest_expect(ktest_read(fds[0], readback, 1), -__k_EAGAIN);

    // The ring wraps over its pages
    for(int round = 0; round < 40; ++round) {
        long size = 1 + (round * 997) % (3 * PAGE_SIZE);

        ktest_expect(ktest_write(fds[1], pattern + round, size), size);
        ktest_expect(drain(fds[0], readback, size), size); // A read hands out one page at most
        ktest_expect(memcmp(readback, pattern + round, size), 0);
    }

    ktest_close(fds[0]);
    ktest_close(fds[1]);
}

// A reader that waits gets the data of a writer that comes later, and a writer that waits gets the room
static void test_blocking(void)
{
    int fds[2] = {};
    char buffer[8] = {};

    ktest_expect(make_pipe(fds, 0), 0);

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        sleep_for(SETTLE_NSEC);
        ktest_write(fds[1], "late", 4);
        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    ktest_expect(ktest_read(fds[0], buffer, sizeof(buffer)), 4);
    ktest_expect(ktest_bytes_equal(buffer, "late", 4), 1);
    reap(child);

    ktest_expect(ktest_write(fds[1], pattern, PIPE_SIZE), PIPE_SIZE);

    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        sleep_for(SETTLE_NSEC);
        ktest_read(fds[0], readback, PAGE_SIZE);
        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    ktest_expect(ktest_write(fds[1], pattern + PIPE_SIZE, PAGE_SIZE), PAGE_SIZE);
    reap(child);

    // All of it, in order: the first page went to the child
    ktest_expect(drain(fds[0], readback, PIPE_SIZE), PIPE_SIZE);
    ktest_expect(memcmp(readback, pattern + PAGE_SIZE, PIPE_SIZE), 0);

    // A reader that waits sees the end of the file when the last writer goes
    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        sleep_for(SETTLE_NSEC);
        ktest_close(fds[1]);
        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    ktest_close(fds[1]);
    ktest_expect(ktest_read(fds[0], buffer, sizeof(buffer)), 0);
    reap(child);
    ktest_close(fds[0]);
}

int main(void)
{
    struct __k_sigaction action = {};

    action.sa_handler = &on_sigpipe;
    action.sa_flags = __k_SA_RESTORER;
    action.sa_restorer = &ktest_signal_restorer;
    ktest_syscall3(__k_SYS_SIGACTION, __k_SIGPIPE, (long)(&action), 0);

    fill_pattern();

    test_basic();
    test_flags();
    test_ends();
    test_full();
    test_blocking();

    return ktest_result();
}
