#include <vex/errno.h>
#include <vex/poll.h>
#include <vex/resource.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define SETTLE_NSEC 50000000L
#define MANY_FDS    200 // More than the kernel keeps on its stack, and more than one page of entries
#define BIT(sig)    (1UL << ((sig) - 1))

static volatile int handled_usr1;
static struct __k_pollfd many[MANY_FDS];

static long make_pipe(int* fds)
{
    return ktest_syscall2(__k_SYS_PIPE, (long)(fds), 0);
}

static long ppoll(struct __k_pollfd* fds, long count, const struct __k_timespec* timeout, const __k_sigset_t* mask)
{
    return ktest_syscall4(__k_SYS_PPOLL, (long)(fds), count, (long)(timeout), (long)(mask));
}

// One descriptor, with a zero timeout
static long poll_now(int fd, int events, int* revents)
{
    struct __k_pollfd entry = {};
    entry.fd = fd;
    entry.events = events;
    entry.revents = 0x7FFF;

    struct __k_timespec zero = {};
    long result = ppoll(&entry, 1, &zero, nullptr);

    *revents = entry.revents;

    return result;
}

static __k_sigset_t get_mask(void)
{
    __k_sigset_t mask = 0;

    ktest_syscall3(__k_SYS_SIGPROCMASK, __k_SIG_BLOCK, 0, (long)(&mask));

    return mask;
}

static void set_mask(__k_sigset_t mask)
{
    ktest_syscall3(__k_SYS_SIGPROCMASK, __k_SIG_SETMASK, (long)(&mask), 0);
}

static void on_usr1(__k_int32_t sig)
{
    handled_usr1 += 1;
}

static void sleep_for(long nanoseconds)
{
    struct __k_timespec request;
    request.tv_sec = nanoseconds / 1000000000L;
    request.tv_nsec = nanoseconds % 1000000000L;

    ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)(&request), 0);
}

static long now_nsec(void)
{
    struct __k_timespec time = {};

    ktest_syscall2(__k_SYS_GETTIME, __k_CLOCK_MONOTONIC, (long)(&time));

    return time.tv_sec * 1000000000L + time.tv_nsec;
}

static void reap(long child)
{
    int wstatus = -1;

    ktest_syscall3(__k_SYS_WAITPID, child, (long)(&wstatus), 0);
}

// A child that writes a byte after the parent had time to start waiting, and then exits
static long write_later(int fd)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        sleep_for(SETTLE_NSEC);
        ktest_write(fd, "x", 1);
        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    return child;
}

static long signal_later(long target, int sig)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        sleep_for(SETTLE_NSEC);
        ktest_syscall2(__k_SYS_KILL, target, sig);
        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    return child;
}

static void test_state(void)
{
    int fds[2] = {};
    int revents = 0;
    char byte = 0;

    ktest_expect(make_pipe(fds), 0);

    ktest_expect(poll_now(fds[0], __k_POLLIN, &revents), 0);
    ktest_expect(revents, 0);
    ktest_expect(poll_now(fds[1], __k_POLLOUT | __k_POLLWRNORM, &revents), 1);
    ktest_expect(revents, __k_POLLOUT | __k_POLLWRNORM);

    // Only the bits that were asked for come back
    ktest_expect(ktest_write(fds[1], "x", 1), 1);
    ktest_expect(poll_now(fds[0], __k_POLLIN, &revents), 1);
    ktest_expect(revents, __k_POLLIN);
    ktest_expect(poll_now(fds[0], __k_POLLRDNORM | __k_POLLPRI, &revents), 1);
    ktest_expect(revents, __k_POLLRDNORM);
    ktest_expect(poll_now(fds[0], __k_POLLOUT, &revents), 0);
    ktest_expect(revents, 0);

    // POLLHUP comes without being asked for, and with the data that is left
    ktest_close(fds[1]);
    ktest_expect(poll_now(fds[0], 0, &revents), 1);
    ktest_expect(revents, __k_POLLHUP);
    ktest_expect(poll_now(fds[0], __k_POLLIN, &revents), 1);
    ktest_expect(revents, __k_POLLIN | __k_POLLHUP);
    ktest_expect(ktest_read(fds[0], &byte, 1), 1);
    ktest_expect(poll_now(fds[0], __k_POLLIN, &revents), 1);
    ktest_expect(revents, __k_POLLHUP);
    ktest_close(fds[0]);

    // A writer without readers gets POLLERR
    ktest_expect(make_pipe(fds), 0);
    ktest_close(fds[0]);
    ktest_expect(poll_now(fds[1], 0, &revents), 1);
    ktest_expect(revents, __k_POLLERR);
    ktest_close(fds[1]);
}

static void test_descriptors(void)
{
    int revents = 0;
    struct __k_timespec zero = {};
    struct __k_pollfd entries[2] = {};

    ktest_expect(poll_now(1000, __k_POLLIN, &revents), 1);
    ktest_expect(revents, __k_POLLNVAL);

    // A negative descriptor is skipped, and its revents is cleared
    entries[0].fd = -1;
    entries[0].events = __k_POLLIN;
    entries[0].revents = __k_POLLIN;
    entries[1].fd = 1000;
    entries[1].events = __k_POLLIN;
    ktest_expect(ppoll(entries, 2, &zero, nullptr), 1);
    ktest_expect(entries[0].revents, 0);
    ktest_expect(entries[1].revents, __k_POLLNVAL);

    ktest_expect(ppoll(entries, 0, &zero, nullptr), 0);
    ktest_expect(ppoll((struct __k_pollfd*)(-4096L), 1, &zero, nullptr), -__k_EFAULT);
    ktest_expect(ppoll(entries, 2, (const struct __k_timespec*)(-4096L), nullptr), -__k_EFAULT);
    ktest_expect(ppoll(entries, 2, &zero, (const __k_sigset_t*)(-4096L)), -__k_EFAULT);

    struct __k_rlimit limit = {};
    ktest_syscall2(__k_SYS_GETRLIMIT, __k_RLIMIT_NOFILE, (long)(&limit));
    ktest_expect(ppoll(entries, (long)(limit.rlim_cur) + 1, &zero, nullptr), -__k_EINVAL);

    struct __k_timespec invalid = {};
    invalid.tv_nsec = 1000000000L;
    ktest_expect(ppoll(entries, 2, &invalid, nullptr), -__k_EINVAL);
}

static void test_many(void)
{
    int fds[2] = {};

    ktest_expect(make_pipe(fds), 0);
    ktest_expect(ktest_write(fds[1], "x", 1), 1);

    for(int i = 0; i < MANY_FDS; ++i) {
        many[i].fd = fds[0];
        many[i].events = __k_POLLIN;
        many[i].revents = 0;
    }

    // The last one waits for nothing, so the count shows that every chunk was seen
    many[MANY_FDS - 1].events = __k_POLLOUT;

    struct __k_timespec zero = {};
    ktest_expect(ppoll(many, MANY_FDS, &zero, nullptr), MANY_FDS - 1);
    ktest_expect(many[0].revents, __k_POLLIN);
    ktest_expect(many[MANY_FDS - 2].revents, __k_POLLIN);
    ktest_expect(many[MANY_FDS - 1].revents, 0);

    // A wait over all the chunks ends on a write
    char byte = 0;
    ktest_expect(ktest_read(fds[0], &byte, 1), 1);

    long child = write_later(fds[1]);
    ktest_expect(ppoll(many, MANY_FDS, nullptr, nullptr), MANY_FDS - 1);
    reap(child);
    ktest_expect(many[MANY_FDS - 2].revents, __k_POLLIN);

    ktest_close(fds[0]);
    ktest_close(fds[1]);
}

static void test_wait(void)
{
    int first[2] = {};
    int second[2] = {};
    struct __k_pollfd entries[2] = {};

    ktest_expect(make_pipe(first), 0);
    ktest_expect(make_pipe(second), 0);

    entries[0].fd = first[0];
    entries[0].events = __k_POLLIN;
    entries[1].fd = second[0];
    entries[1].events = __k_POLLIN;

    // The timeout passes with nothing to read
    struct __k_timespec timeout = {};
    timeout.tv_nsec = SETTLE_NSEC;

    long start = now_nsec();
    ktest_expect(ppoll(entries, 2, &timeout, nullptr), 0);
    ktest_assert(now_nsec() - start >= SETTLE_NSEC);
    ktest_expect(entries[0].revents, 0);
    ktest_expect(entries[1].revents, 0);

    // A write to the second pipe ends a wait without a timeout, and only it reports
    long child = write_later(second[1]);
    ktest_expect(ppoll(entries, 2, nullptr, nullptr), 1);
    reap(child);
    ktest_expect(entries[0].revents, 0);
    ktest_expect(entries[1].revents, __k_POLLIN);

    // The last writer that goes away ends the wait too
    char byte = 0;
    ktest_expect(ktest_read(second[0], &byte, 1), 1);

    child = ktest_syscall1(__k_SYS_FORK, 0);

    // Its exit closes its copy of the write end
    if(child == 0) {
        sleep_for(SETTLE_NSEC);
        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    ktest_close(first[1]);
    ktest_expect(ppoll(entries, 2, nullptr, nullptr), 1);
    reap(child);
    ktest_expect(entries[0].revents, __k_POLLHUP);

    ktest_close(first[0]);
    ktest_close(second[0]);
    ktest_close(second[1]);
}

static void test_signals(void)
{
    int fds[2] = {};
    long pid = ktest_syscall1(__k_SYS_GETPID, 0);
    struct __k_pollfd entry = {};

    struct __k_sigaction action = {};
    action.sa_handler = &on_usr1;
    action.sa_flags = __k_SA_RESTORER;
    action.sa_restorer = &ktest_signal_restorer;
    ktest_syscall3(__k_SYS_SIGACTION, __k_SIGUSR1, (long)(&action), 0);

    ktest_expect(make_pipe(fds), 0);
    entry.fd = fds[0];
    entry.events = __k_POLLIN;

    // A signal ends the wait
    long child = signal_later(pid, __k_SIGUSR1);
    ktest_expect(ppoll(&entry, 1, nullptr, nullptr), -__k_EINTR);
    reap(child);
    ktest_expect(handled_usr1, 1);

    // The mask of the call lets in a signal that the thread blocks, and the old mask is back afterwards
    set_mask(BIT(__k_SIGUSR1));

    __k_sigset_t open = 0;
    child = signal_later(pid, __k_SIGUSR1);
    ktest_expect(ppoll(&entry, 1, nullptr, &open), -__k_EINTR);
    reap(child);
    ktest_expect(handled_usr1, 2);
    ktest_expect(get_mask(), BIT(__k_SIGUSR1));

    // A signal that the mask of the call blocks does not end the wait, and runs once the call is done
    set_mask(0);

    __k_sigset_t closed = BIT(__k_SIGUSR1);
    struct __k_timespec timeout = {};
    timeout.tv_nsec = 2 * SETTLE_NSEC;

    child = signal_later(pid, __k_SIGUSR1);
    ktest_expect(ppoll(&entry, 1, &timeout, &closed), 0);
    reap(child);
    ktest_expect(handled_usr1, 3);
    ktest_expect(get_mask(), 0);

    ktest_close(fds[0]);
    ktest_close(fds[1]);
}

int main(void)
{
    test_state();
    test_descriptors();
    test_many();
    test_wait();
    test_signals();

    return ktest_result();
}
