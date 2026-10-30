#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/ioctl.h>
#include <vex/limits.h>
#include <vex/signal.h>
#include <vex/stat.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define FIFO            "memory/fifo"
#define SETTLE_NSEC     50000000L
#define NSEC_PER_SEC    1000000000L
#define UNPRIVILEGED_ID 1000

static volatile int interrupted;

static long make_node(const char* path, long mode, long rdev)
{
    return ktest_syscall4(__k_SYS_MKNODAT, __k_AT_FDCWD, (long)(path), mode, rdev);
}

static struct __k_stat stat_path(const char* path)
{
    struct __k_stat st = {};
    ktest_expect(ktest_syscall4(__k_SYS_FSTATAT, __k_AT_FDCWD, (long)(path), (long)(&st), 0), 0);
    return st;
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

static void test_make(void)
{
    ktest_expect(make_node(FIFO, __k_S_IFIFO | 0640, 0), 0);
    ktest_expect(make_node(FIFO, __k_S_IFIFO | 0640, 0), -__k_EEXIST);

    struct __k_stat st = stat_path(FIFO);
    ktest_expect(st.st_mode & __k_S_IFMT, __k_S_IFIFO);
    ktest_expect(st.st_mode & 0777, 0640);
    ktest_expect(st.st_rdev, 0);

    // No type is a regular file, as in Linux
    ktest_expect(make_node("memory/plain", 0644, 0), 0);
    st = stat_path("memory/plain");
    ktest_expect(st.st_mode & __k_S_IFMT, __k_S_IFREG);

    ktest_expect(make_node("memory/device", __k_S_IFCHR | 0600, 0x1234), 0);
    st = stat_path("memory/device");
    ktest_expect(st.st_mode & __k_S_IFMT, __k_S_IFCHR);
    ktest_expect(st.st_rdev, 0x1234);

    ktest_expect(make_node("memory/directory", __k_S_IFDIR | 0755, 0), -__k_EINVAL);
    ktest_expect(make_node("memory/socket", __k_S_IFSOCK | 0755, 0), -__k_EINVAL);
}

static void unprivileged_device(void)
{
    ktest_expect(ktest_syscall3(__k_SYS_SETRESUID, UNPRIVILEGED_ID, UNPRIVILEGED_ID, UNPRIVILEGED_ID), 0);
    ktest_expect(make_node("memory/denied", __k_S_IFBLK | 0600, 1), -__k_EPERM);
    ktest_expect(make_node("memory/allowed", __k_S_IFIFO | 0600, 0), 0);
}

static void test_privilege(void)
{
    // Anyone may write the directory
    ktest_expect(ktest_syscall4(__k_SYS_FCHMODAT, __k_AT_FDCWD, (long)("memory"), 0777, 0), 0);

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        unprivileged_device();
        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    ktest_assert(child > 0);
    wait_child(child);
}

// Opens that do not wait: a reader with O_NONBLOCK, then a writer that finds it
static void test_flow(void)
{
    long reader = ktest_open(FIFO, __k_O_RDONLY | __k_O_NONBLOCK, 0);
    ktest_assert(reader >= 0);

    long writer = ktest_open(FIFO, __k_O_WRONLY | __k_O_NONBLOCK, 0);
    ktest_assert(writer >= 0);

    char buffer[8] = {};

    ktest_expect(ktest_read(reader, buffer, sizeof(buffer)), -__k_EAGAIN);
    ktest_expect(ktest_write(writer, "fifo", 4), 4);
    ktest_expect(ktest_read(reader, buffer, sizeof(buffer)), 4);
    ktest_expect(buffer[0], 'f');

    // The last writer leaves the end of the file behind
    ktest_close(writer);
    ktest_expect(ktest_read(reader, buffer, sizeof(buffer)), 0);
    ktest_close(reader);

    ktest_expect(ktest_open(FIFO, __k_O_WRONLY | __k_O_NONBLOCK, 0), -__k_ENXIO);

    // An open for both ends waits for nobody, and the data goes with the last close
    long both = ktest_open(FIFO, __k_O_RDWR, 0);
    ktest_assert(both >= 0);
    ktest_expect(ktest_write(both, "lost", 4), 4);
    ktest_close(both);

    both = ktest_open(FIFO, __k_O_RDWR, 0);
    ktest_assert(both >= 0);

    __k_int32_t queued = -1;
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, both, __k_FIONREAD, (long)(&queued)), 0);
    ktest_expect(queued, 0);
    ktest_close(both);
}

static void late_writer(void)
{
    sleep_for(SETTLE_NSEC);

    long writer = ktest_open(FIFO, __k_O_WRONLY, 0);
    ktest_assert(writer >= 0);
    ktest_expect(ktest_write(writer, "late", 4), 4);
    ktest_close(writer);
}

static void early_writer(void)
{
    long writer = ktest_open(FIFO, __k_O_WRONLY, 0);
    ktest_assert(writer >= 0);
    ktest_expect(ktest_write(writer, "early", 5), 5);
    ktest_close(writer);
}

static long fork_into(void (*body)(void))
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        body();
        ktest_syscall1(__k_SYS_EXIT, ktest_result());
    }

    ktest_assert(child > 0);
    return child;
}

// A reader waits for a writer, and a writer waits for a reader
static void test_waits(void)
{
    char buffer[8] = {};

    long child = fork_into(&late_writer);
    long start = monotonic_nsec();
    long reader = ktest_open(FIFO, __k_O_RDONLY, 0);

    ktest_assert(reader >= 0);
    ktest_assert(monotonic_nsec() - start >= SETTLE_NSEC / 2);
    ktest_expect(ktest_read(reader, buffer, sizeof(buffer)), 4);
    ktest_close(reader);
    wait_child(child);

    child = fork_into(&early_writer);
    sleep_for(SETTLE_NSEC);
    reader = ktest_open(FIFO, __k_O_RDONLY, 0);

    ktest_assert(reader >= 0);
    ktest_expect(ktest_read(reader, buffer, sizeof(buffer)), 5);
    ktest_expect(buffer[0], 'e');
    ktest_close(reader);
    wait_child(child);
}

static void on_signal(__k_int32_t sig)
{
    interrupted += 1;
}

static void lonely_reader(void)
{
    struct __k_sigaction action = {};
    action.sa_handler = &on_signal;
    action.sa_flags = __k_SA_RESTORER;
    action.sa_restorer = &ktest_signal_restorer;
    ktest_expect(ktest_syscall3(__k_SYS_SIGACTION, __k_SIGUSR1, (long)(&action), 0), 0);

    ktest_expect(ktest_open(FIFO, __k_O_RDONLY, 0), -__k_EINTR);
    ktest_expect(interrupted, 1);
}

// A signal ends the wait of an open, and leaves nothing of it behind
static void test_interrupt(void)
{
    long child = fork_into(&lonely_reader);

    sleep_for(SETTLE_NSEC);
    ktest_expect(ktest_syscall2(__k_SYS_KILL, child, __k_SIGUSR1), 0);
    wait_child(child);

    ktest_expect(ktest_open(FIFO, __k_O_WRONLY | __k_O_NONBLOCK, 0), -__k_ENXIO);
}

int main(void)
{
    ktest_enter_work_directory("fifo");

    ktest_expect(ktest_syscall3(__k_SYS_MKDIRAT, __k_AT_FDCWD, (long)("memory"), 0755), 0);
    ktest_expect(ktest_syscall6(__k_SYS_MOUNT, 0, (long)("memory"), (long)("tmpfs"), 0, 0, 0), 0);

    test_make();
    test_privilege();
    test_flow();
    test_waits();
    test_interrupt();

    // A filesystem without nodes of these kinds says so
    ktest_expect(make_node("fifo-on-disk", __k_S_IFIFO | 0600, 0), -__k_EPERM);

    ktest_expect(ktest_syscall1(__k_SYS_UMOUNT, (long)("memory")), 0);

    return ktest_result();
}
