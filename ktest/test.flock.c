#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define SETTLE_NSEC  50000000L
#define NSEC_PER_SEC 1000000000L

static volatile int interrupted;

static long open_data(unsigned flags)
{
    long fd = ktest_open("data", flags, 0);
    ktest_assert(fd >= 0);
    return fd;
}

static long set_lock(long fd, long command, short type, short whence, long start, long length)
{
    struct __k_flock lock = {};
    lock.l_type = type;
    lock.l_whence = whence;
    lock.l_start = start;
    lock.l_len = length;

    return ktest_syscall3(__k_SYS_FCNTL, fd, command, (long)(&lock));
}

static long lock_range(long fd, short type, long start, long length)
{
    return set_lock(fd, __k_F_SETLK, type, __k_SEEK_SET, start, length);
}

// What F_GETLK reports in the way of a write lock on the range
static struct __k_flock test_range(long fd, long start, long length)
{
    struct __k_flock lock = {};
    lock.l_type = __k_F_WRLCK;
    lock.l_whence = __k_SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;

    ktest_expect(ktest_syscall3(__k_SYS_FCNTL, fd, __k_F_GETLK, (long)(&lock)), 0);
    return lock;
}

static void sleep_for(long nsec)
{
    struct __k_timespec request = {
        .tv_sec = 0,
        .tv_nsec = nsec,
    };

    ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)(&request), 0);
}

static long monotonic_nsec(void)
{
    struct __k_timespec now = {};
    ktest_syscall2(__k_SYS_GETTIME, __k_CLOCK_MONOTONIC, (long)(&now));
    return now.tv_sec * NSEC_PER_SEC + now.tv_nsec;
}

static void wait_child(long child)
{
    int wstatus = -1;
    ktest_expect(ktest_syscall4(__k_SYS_WAITPID, child, (long)(&wstatus), 0, 0), child);
    ktest_expect(wstatus, 0);
}

// The child sees the locks of the parent only as locks of another process
static void check_from_child(void (*body)(long parent), long parent)
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        body(parent);
        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    ktest_assert(child > 0);
    wait_child(child);
}

static void conflicts_of_parent(long parent)
{
    long fd = open_data(__k_O_RDWR);

    ktest_expect(lock_range(fd, __k_F_WRLCK, 50, 10), -__k_EAGAIN);
    ktest_expect(lock_range(fd, __k_F_RDLCK, 0, 1), -__k_EAGAIN);
    ktest_expect(lock_range(fd, __k_F_WRLCK, 100, 100), 0);

    struct __k_flock lock = test_range(fd, 0, 10);
    ktest_expect(lock.l_type, __k_F_WRLCK);
    ktest_expect(lock.l_whence, __k_SEEK_SET);
    ktest_expect(lock.l_start, 0);
    ktest_expect(lock.l_len, 100);
    ktest_expect(lock.l_pid, parent);

    ktest_close(fd);
}

static void split_by_parent(long parent)
{
    long fd = open_data(__k_O_RDWR);

    ktest_expect(lock_range(fd, __k_F_WRLCK, 45, 10), 0);
    ktest_expect(lock_range(fd, __k_F_WRLCK, 30, 5), -__k_EAGAIN);
    ktest_expect(lock_range(fd, __k_F_WRLCK, 65, 5), -__k_EAGAIN);

    ktest_close(fd);
}

static void readers_of_parent(long parent)
{
    long fd = open_data(__k_O_RDWR);

    ktest_expect(lock_range(fd, __k_F_RDLCK, 0, 100), 0);
    ktest_expect(lock_range(fd, __k_F_WRLCK, 0, 100), -__k_EAGAIN);

    ktest_close(fd);
}

static void nothing_of_parent(long parent)
{
    long fd = open_data(__k_O_RDWR);

    struct __k_flock lock = test_range(fd, 0, 0);
    ktest_expect(lock.l_type, __k_F_UNLCK);

    ktest_close(fd);
}

static void to_the_end_of_parent(long parent)
{
    long fd = open_data(__k_O_RDWR);

    // The lock runs past the end of the file
    ktest_expect(lock_range(fd, __k_F_WRLCK, 1L << 40, 1), -__k_EAGAIN);
    ktest_expect(lock_range(fd, __k_F_WRLCK, 0, 4095), 0);

    struct __k_flock lock = test_range(fd, 5000, 1);
    ktest_expect(lock.l_start, 4095);
    ktest_expect(lock.l_len, 0);

    ktest_close(fd);
}

static void test_conflicts(long fd)
{
    long parent = ktest_syscall1(__k_SYS_GETPID, 0);

    ktest_expect(lock_range(fd, __k_F_WRLCK, 0, 100), 0);
    check_from_child(&conflicts_of_parent, parent);

    // A hole cut in the middle leaves two ranges
    ktest_expect(lock_range(fd, __k_F_UNLCK, 40, 20), 0);
    check_from_child(&split_by_parent, parent);

    // Changing the type of the own lock needs no unlock first
    ktest_expect(lock_range(fd, __k_F_RDLCK, 0, 100), 0);
    check_from_child(&readers_of_parent, parent);

    ktest_expect(lock_range(fd, __k_F_UNLCK, 0, 0), 0);
    check_from_child(&nothing_of_parent, parent);

    // From the end: the last byte and everything after it
    ktest_expect(set_lock(fd, __k_F_SETLK, __k_F_WRLCK, __k_SEEK_END, -1, 0), 0);
    check_from_child(&to_the_end_of_parent, parent);
    ktest_expect(lock_range(fd, __k_F_UNLCK, 0, 0), 0);
}

// Closing any descriptor of the file drops the locks of the process, as POSIX has it
static void test_close_drops(long fd)
{
    long parent = ktest_syscall1(__k_SYS_GETPID, 0);

    ktest_expect(lock_range(fd, __k_F_WRLCK, 0, 0), 0);

    long other = open_data(__k_O_RDONLY);
    ktest_close(other);

    check_from_child(&nothing_of_parent, parent);
}

static void wait_for_parent(long parent)
{
    long fd = open_data(__k_O_RDWR);
    long start = monotonic_nsec();

    ktest_expect(set_lock(fd, __k_F_SETLKW, __k_F_WRLCK, __k_SEEK_SET, 0, 10), 0);
    ktest_assert(monotonic_nsec() - start >= SETTLE_NSEC / 2);

    ktest_close(fd);
}

static void test_wait(long fd)
{
    long parent = ktest_syscall1(__k_SYS_GETPID, 0);

    ktest_expect(lock_range(fd, __k_F_WRLCK, 0, 10), 0);

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        wait_for_parent(parent);
        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    ktest_assert(child > 0);

    sleep_for(SETTLE_NSEC);
    ktest_expect(lock_range(fd, __k_F_UNLCK, 0, 10), 0);
    wait_child(child);
}

static void on_signal(__k_int32_t sig)
{
    interrupted += 1;
}

static void interrupted_wait(long parent)
{
    struct __k_sigaction action = {};
    action.sa_handler = &on_signal;
    action.sa_flags = __k_SA_RESTORER;
    action.sa_restorer = &ktest_signal_restorer;
    ktest_expect(ktest_syscall3(__k_SYS_SIGACTION, __k_SIGUSR1, (long)(&action), 0), 0);

    long fd = open_data(__k_O_RDWR);

    ktest_expect(set_lock(fd, __k_F_SETLKW, __k_F_WRLCK, __k_SEEK_SET, 0, 10), -__k_EINTR);
    ktest_expect(interrupted, 1);

    ktest_close(fd);
}

static void test_interrupt(long fd)
{
    long parent = ktest_syscall1(__k_SYS_GETPID, 0);

    ktest_expect(lock_range(fd, __k_F_WRLCK, 0, 10), 0);

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        interrupted_wait(parent);
        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    ktest_assert(child > 0);

    sleep_for(SETTLE_NSEC);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGUSR1), 0);
    wait_child(child);

    ktest_expect(lock_range(fd, __k_F_UNLCK, 0, 0), 0);
}

static void test_errors(long fd)
{
    long reader = open_data(__k_O_RDONLY);

    ktest_expect(lock_range(reader, __k_F_WRLCK, 0, 1), -__k_EBADF);
    ktest_expect(lock_range(reader, __k_F_RDLCK, 0, 1), 0);
    ktest_close(reader);

    ktest_expect(lock_range(fd, 7, 0, 1), -__k_EINVAL);
    ktest_expect(lock_range(fd, __k_F_WRLCK, -1, 1), -__k_EINVAL);
    ktest_expect(lock_range(fd, __k_F_WRLCK, 5, -10), -__k_EINVAL);
    ktest_expect(set_lock(fd, __k_F_SETLK, __k_F_WRLCK, 9, 0, 1), -__k_EINVAL);
    ktest_expect(set_lock(fd, __k_F_GETLK, __k_F_UNLCK, __k_SEEK_SET, 0, 1), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_FCNTL, fd, __k_F_SETLK, 1), -__k_EFAULT);
}

static long flock_fd(long fd, long operation)
{
    return ktest_syscall2(__k_SYS_FLOCK, fd, operation);
}

// flock locks the open file, so two opens conflict even in one process, and a copy of the descriptor shares the lock
static void test_flock(void)
{
    long first = open_data(__k_O_RDONLY); // No write access needed, unlike F_WRLCK
    long second = open_data(__k_O_RDONLY);

    ktest_expect(flock_fd(first, __k_LOCK_EX), 0);
    ktest_expect(flock_fd(second, __k_LOCK_EX | __k_LOCK_NB), -__k_EAGAIN);
    ktest_expect(flock_fd(second, __k_LOCK_SH | __k_LOCK_NB), -__k_EAGAIN);

    long copy = ktest_syscall3(__k_SYS_FCNTL, first, __k_F_DUPFD, 0);
    ktest_assert(copy >= 0);
    ktest_close(copy);
    ktest_expect(flock_fd(second, __k_LOCK_SH | __k_LOCK_NB), -__k_EAGAIN);

    ktest_expect(flock_fd(first, __k_LOCK_SH), 0);
    ktest_expect(flock_fd(second, __k_LOCK_SH | __k_LOCK_NB), 0);

    // A change of type drops the old lock first, so the failed one leaves second with nothing
    ktest_expect(flock_fd(second, __k_LOCK_EX | __k_LOCK_NB), -__k_EAGAIN);
    ktest_expect(flock_fd(first, __k_LOCK_EX | __k_LOCK_NB), 0);

    // It meets the locks of fcntl, also those of the same process
    ktest_expect(lock_range(second, __k_F_RDLCK, 0, 1), -__k_EAGAIN);

    // The last close of the open file drops it
    ktest_close(first);
    ktest_expect(flock_fd(second, __k_LOCK_EX | __k_LOCK_NB), 0);
    ktest_expect(flock_fd(second, __k_LOCK_EX | __k_LOCK_NB), 0);
    ktest_expect(flock_fd(second, __k_LOCK_UN), 0);

    ktest_expect(flock_fd(second, 0), -__k_EINVAL);
    ktest_expect(flock_fd(second, __k_LOCK_SH | __k_LOCK_EX), -__k_EINVAL);
    ktest_expect(flock_fd(second, 16), -__k_EINVAL);
    ktest_expect(flock_fd(-1, __k_LOCK_SH), -__k_EBADF);

    ktest_close(second);
}

static long ofd_range(long fd, short type, long start, long length)
{
    return set_lock(fd, __k_F_OFD_SETLK, type, __k_SEEK_SET, start, length);
}

// A lock of an open file survives the close of other descriptors, and F_GETLK reports it with pid -1
static void test_ofd(void)
{
    long first = open_data(__k_O_RDWR);
    long second = open_data(__k_O_RDWR);

    ktest_expect(ofd_range(first, __k_F_WRLCK, 0, 10), 0);
    ktest_expect(ofd_range(second, __k_F_WRLCK, 5, 10), -__k_EAGAIN);

    struct __k_flock lock = {};
    lock.l_type = __k_F_RDLCK;
    lock.l_whence = __k_SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 1;

    ktest_expect(ktest_syscall3(__k_SYS_FCNTL, second, __k_F_OFD_GETLK, (long)(&lock)), 0);
    ktest_expect(lock.l_type, __k_F_WRLCK);
    ktest_expect(lock.l_start, 0);
    ktest_expect(lock.l_len, 10);
    ktest_expect(lock.l_pid, -1);

    // The process is another owner than the open file
    lock = test_range(second, 0, 1);
    ktest_expect(lock.l_pid, -1);

    long other = open_data(__k_O_RDONLY);
    ktest_close(other);
    ktest_expect(ofd_range(second, __k_F_WRLCK, 5, 10), -__k_EAGAIN);

    lock = (struct __k_flock) {};
    lock.l_type = __k_F_WRLCK;
    lock.l_pid = 5;
    ktest_expect(ktest_syscall3(__k_SYS_FCNTL, second, __k_F_OFD_SETLK, (long)(&lock)), -__k_EINVAL);

    ktest_close(first);
    ktest_expect(ofd_range(second, __k_F_WRLCK, 5, 10), 0);

    ktest_close(second);
}

int main(void)
{
    ktest_enter_work_directory("flock");

    static char page[4096];
    long fd = open_data(__k_O_CREAT | __k_O_RDWR);
    ktest_expect(ktest_write(fd, page, sizeof(page)), sizeof(page));

    test_conflicts(fd);
    test_close_drops(fd);

    ktest_close(fd);
    fd = open_data(__k_O_RDWR);

    test_wait(fd);
    test_interrupt(fd);
    test_errors(fd);
    test_flock();
    test_ofd();

    ktest_close(fd);

    return ktest_result();
}
