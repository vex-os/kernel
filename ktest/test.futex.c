#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/futex.h>
#include <vex/mman.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define PINGPONG_ROUNDS 1000
#define PAGE_SIZE       4096
#define GIVE_UP_NSEC    5000000000L
#define SETTLE_NSEC     50000000L

static volatile unsigned int turn;
static volatile long pongs;
static volatile unsigned int never_changes;

[[noreturn]] static void ponger(long arg)
{
    for(long i = 0; i < arg; ++i) {
        while(turn == 0) {
            ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&turn, 0, 0, 0);
        }

        pongs += 1;
        turn = 0;
        ktest_syscall3(__k_SYS_FUTEX_WAKE, (long)&turn, 1, 0);
    }

    ktest_syscall1(__k_SYS_THREAD_EXIT, 0);
    __builtin_unreachable();
}

static void sleep_for(long nanoseconds)
{
    struct __k_timespec request;
    request.tv_sec = nanoseconds / 1000000000L;
    request.tv_nsec = nanoseconds % 1000000000L;

    ktest_syscall2(__k_SYS_SLEEP, (long)&request, 0);
}

static long wait_status(long child)
{
    int wstatus = -1;

    ktest_syscall3(__k_SYS_WAITPID, child, (long)&wstatus, 0);

    return wstatus;
}

// Waits in a forked process on a word that the parent shares through the mapping, with the given kind of key
// Exits with 7 when the word changed, 6 when the wait ran out, and 9 when the wait ended with nothing changed
[[noreturn]] static void shared_waiter(volatile unsigned int* word, long wait_nsec, long flags)
{
    struct __k_timespec timeout;
    timeout.tv_sec = wait_nsec / 1000000000L;
    timeout.tv_nsec = wait_nsec % 1000000000L;

    for(;;) {
        if(*word) {
            ktest_syscall1(__k_SYS_EXIT, 7);
        }

        long retval = ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)word, 0, (long)&timeout, flags);

        if(retval == -__k_ETIMEDOUT) {
            ktest_syscall1(__k_SYS_EXIT, 6);
        }

        if(retval == 0 && *word == 0) {
            ktest_syscall1(__k_SYS_EXIT, 9);
        }
    }
}

static void test_shared(void)
{
    ktest_enter_work_directory("futex");

    long fd = ktest_open("word.bin", __k_O_RDWR | __k_O_CREAT, 0644);
    char zeros[PAGE_SIZE] = {};

    ktest_assert(fd >= 0);
    ktest_expect(ktest_write(fd, zeros, sizeof(zeros)), PAGE_SIZE);

    long base = ktest_syscall6(__k_SYS_MMAP, 0, PAGE_SIZE, __k_PROT_READ | __k_PROT_WRITE, __k_MAP_SHARED, fd, 0);

    ktest_assert(base > 0);

    volatile unsigned int* word = (volatile unsigned int*)(base);

    // A wake by address does not reach a waiter on the frame, and the other way round
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        shared_waiter(word, 2 * SETTLE_NSEC, __k_FUTEX_SHARED);
    }

    sleep_for(SETTLE_NSEC / 2);
    ktest_expect(ktest_syscall3(__k_SYS_FUTEX_WAKE, (long)word, 10, 0), 0);
    ktest_expect(wait_status(child), __k_W_EXITCODE(6, 0));

    // A wake on the frame reaches a waiter in another process that maps the same file
    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        shared_waiter(word, GIVE_UP_NSEC, __k_FUTEX_SHARED);
    }

    sleep_for(SETTLE_NSEC);
    *word = 1;
    ktest_expect(ktest_syscall3(__k_SYS_FUTEX_WAKE, (long)word, 1, __k_FUTEX_SHARED) >= 0, 1);
    ktest_expect(wait_status(child), __k_W_EXITCODE(7, 0));

    ktest_syscall2(__k_SYS_MUNMAP, base, PAGE_SIZE);
    ktest_close(fd);
}

int main(void)
{
    // futex: argument checks and timeouts
    struct __k_timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 10000000;
    ktest_expect(ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes, 1, 0, 0), -__k_EAGAIN);
    ktest_expect(ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes, 0, (long)&timeout, 0), -__k_ETIMEDOUT);
    ktest_expect(ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes + 1, 0, 0, 0), -__k_EINVAL);
    ktest_expect(ktest_syscall4(__k_SYS_FUTEX_WAIT, -4096L, 0, 0, 0), -__k_EFAULT);
    ktest_expect(ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes, 1, 0, __k_FUTEX_SHARED), -__k_EAGAIN);
    timeout.tv_nsec = 10000000;
    ktest_expect(ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes, 0, (long)&timeout, __k_FUTEX_SHARED), -__k_ETIMEDOUT);
    ktest_expect(ktest_syscall4(__k_SYS_FUTEX_WAIT, -4096L, 0, 0, __k_FUTEX_SHARED), -__k_EFAULT);
    ktest_expect(ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes + 1, 0, 0, __k_FUTEX_SHARED), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_FUTEX_WAKE, (long)&never_changes, 1, __k_FUTEX_SHARED), 0);
    ktest_expect(ktest_syscall3(__k_SYS_FUTEX_WAKE, -4096L, 1, __k_FUTEX_SHARED), -__k_EFAULT);
    ktest_expect(ktest_syscall3(__k_SYS_FUTEX_WAKE, (long)&never_changes + 1, 1, __k_FUTEX_SHARED), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_FUTEX_WAKE, (long)&never_changes, 1, 2), -__k_EINVAL);
    ktest_expect(ktest_syscall3(__k_SYS_FUTEX_WAKE, (long)&never_changes, 1, 0), 0);
    timeout.tv_nsec = 0;
    ktest_expect(ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes, 0, (long)&timeout, 0), -__k_ETIMEDOUT);

    // futex: two threads hand a turn back and forth, then the second is joined through clear_tid
    ktest_spawn_joinable(&ponger, PINGPONG_ROUNDS);

    for(long i = 0; i < PINGPONG_ROUNDS; ++i) {
        turn = 1;
        ktest_syscall3(__k_SYS_FUTEX_WAKE, (long)&turn, 1, 0);

        while(turn == 1) {
            ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&turn, 1, 0, 0);
        }
    }

    ktest_join();
    ktest_expect(pongs, 1000);
    ktest_expect(ktest_join_word, 0);

    test_shared();

    return ktest_result();
}
