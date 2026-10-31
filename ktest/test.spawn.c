#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/spawn.h>
#include <vex/syscall.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

#define CHILD "/kst/spawn.elf"

static char* child_argv[] = { (char*)(CHILD), (char*)("--child"), (char*)("plain"), nullptr };
static char* child_envp[] = { nullptr };

// The other side of this test: main() re-spawns the program with "--child"
// as argv[1], and the child runs its mode and exits
static int child_main(int argc, char** argv)
{
    if(argc < 1) {
        return 99;
    }

    const char* mode = argv[0];

    if(ktest_string_equal(mode, "echo")) {
        ktest_dprintf(1, "%d", argc);

        for(int i = 0; i < argc; i += 1) {
            ktest_dprintf(1, " %s", argv[i]);
        }

        return 0;
    }

    if(ktest_string_equal(mode, "plain")) {
        return 42;
    }

    if(ktest_string_equal(mode, "badfd")) {
        char buffer[8];
        long result = ktest_syscall3(__k_SYS_READ, 0, (long)(buffer), sizeof(buffer));

        if(result == -__k_EBADF) {
            return 0;
        }

        return 1;
    }

    if(ktest_string_equal(mode, "wait")) {
        struct __k_timespec request = {
            .tv_sec = 0,
            .tv_nsec = 200000000L,
        };

        ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)(&request), 0);
        return 0;
    }

    if(ktest_string_equal(mode, "stdin")) {
        char buffer[64];
        long count = ktest_read(0, buffer, sizeof(buffer));

        if(count > 0) {
            ktest_write(1, buffer, (size_t)(count));
        }

        return 0;
    }

    return 99;
}

static long spawn_args(const char* path, char* const argv[], long flags, long pgid, const union __k_spawn_action* actions,
    long action_count)
{
    struct __k_spawn_args args = {};

    args.path = (long)(path);
    args.argv = (long)(argv);
    args.envp = (long)(child_envp);
    args.flags = (unsigned long)(flags);
    args.pgid = (__k_pid_t)(pgid);
    args.actions = (long)(actions);
    args.action_count = (unsigned long)(action_count);

    return ktest_syscall1(__k_SYS_SPAWN, (long)(&args));
}

static void wait_code(long child, int code)
{
    int wstatus = -1;
    ktest_expect(ktest_syscall4(__k_SYS_WAITPID, child, (long)(&wstatus), 0, 0), child);
    ktest_expect(__k_WIFEXITED(wstatus), 1);
    ktest_expect(__k_WEXITSTATUS(wstatus), code);
}

static long getpgid_of(long pid)
{
    return ktest_syscall1(__k_SYS_GETPGID, pid);
}

static long getsid_of(long pid)
{
    return ktest_syscall1(__k_SYS_GETSID, pid);
}

// An open on fd 1 redirects what the child writes there into a new file
static void make_stdout_redirect(union __k_spawn_action* actions, const char* path)
{
    memset(actions, 0, sizeof(*actions));
    actions[0].open.op = __k_SPAWN_OPEN;
    actions[0].open.dirfd = __k_AT_FDCWD;
    actions[0].open.open_flags = __k_O_WRONLY | __k_O_CREAT | __k_O_TRUNC;
    actions[0].open.mode = 0644;
    actions[0].open.fd = 1;
    actions[0].open.path = (long)(path);
}

static void test_arguments(void)
{
    union __k_spawn_action actions[1];
    make_stdout_redirect(actions, "args.txt");

    char* argv[] = { (char*)(CHILD), (char*)("--child"), (char*)("echo"), (char*)("alpha"), (char*)("beta"), nullptr };

    long child = spawn_args(CHILD, argv, 0, 0, actions, 1);
    ktest_expect(child > 0, 1);
    wait_code(child, 0);

    char buffer[64] = {};
    long fd = ktest_open("args.txt", __k_O_RDONLY, 0);
    ktest_expect(fd >= 0, 1);
    long count = ktest_read(fd, buffer, sizeof(buffer) - 1);
    ktest_close(fd);
    ktest_expect(count, 17);
    ktest_expect(ktest_string_equal(buffer, "3 echo alpha beta"), 1);
}

static void test_exit_code(void)
{
    long child = spawn_args(CHILD, child_argv, 0, 0, nullptr, 0);
    ktest_expect(child > 0, 1);
    wait_code(child, 42);
}

static void test_empty_fds(void)
{
    char* argv[] = { (char*)(CHILD), (char*)("--child"), (char*)("badfd"), nullptr };

    long child = spawn_args(CHILD, argv, __k_SPAWN_EMPTY_FDS, 0, nullptr, 0);
    ktest_expect(child > 0, 1);
    wait_code(child, 0);
}

// An open on fd 5 and a dup2 onto fd 0 feed the child from a file
static void test_stdin_redirect(void)
{
    long fd = ktest_open("input.txt", __k_O_WRONLY | __k_O_CREAT | __k_O_TRUNC, 0644);
    ktest_expect(fd >= 0, 1);
    ktest_write(fd, "piped", 5);
    ktest_close(fd);

    union __k_spawn_action actions[3];
    memset(actions, 0, sizeof(actions));

    actions[0].open.op = __k_SPAWN_OPEN;
    actions[0].open.dirfd = __k_AT_FDCWD;
    actions[0].open.open_flags = __k_O_RDONLY;
    actions[0].open.mode = 0;
    actions[0].open.fd = 5;
    actions[0].open.path = (long)("input.txt");

    actions[1].dup2.op = __k_SPAWN_DUP2;
    actions[1].dup2.oldfd = 5;
    actions[1].dup2.newfd = 0;

    actions[2].open.op = __k_SPAWN_OPEN;
    actions[2].open.dirfd = __k_AT_FDCWD;
    actions[2].open.open_flags = __k_O_WRONLY | __k_O_CREAT | __k_O_TRUNC;
    actions[2].open.mode = 0644;
    actions[2].open.fd = 1;
    actions[2].open.path = (long)("piped.txt");

    char* argv[] = { (char*)(CHILD), (char*)("--child"), (char*)("stdin"), nullptr };

    long child = spawn_args(CHILD, argv, 0, 0, actions, 3);
    ktest_expect(child > 0, 1);
    wait_code(child, 0);

    ktest_expect(ktest_file_has_prefix("piped.txt", "piped"), 1);
}

static void test_setsid(void)
{
    char* argv[] = { (char*)(CHILD), (char*)("--child"), (char*)("wait"), nullptr };

    long child = spawn_args(CHILD, argv, __k_SPAWN_SETSID, 0, nullptr, 0);
    ktest_expect(child > 0, 1);

    ktest_expect(getsid_of(child), child);
    ktest_expect(getpgid_of(child), child);

    wait_code(child, 0);
}

static void test_setpgid(void)
{
    char* argv[] = { (char*)(CHILD), (char*)("--child"), (char*)("wait"), nullptr };

    long leader = spawn_args(CHILD, argv, __k_SPAWN_SETPGID, 0, nullptr, 0);
    ktest_expect(leader > 0, 1);
    ktest_expect(getpgid_of(leader), leader);

    long member = spawn_args(CHILD, argv, __k_SPAWN_SETPGID, leader, nullptr, 0);
    ktest_expect(member > 0, 1);
    ktest_expect(getpgid_of(member), leader);

    wait_code(member, 0);
    wait_code(leader, 0);
}

static void test_errors(void)
{
    ktest_expect(spawn_args("/no/such/program", child_argv, 0, 0, nullptr, 0), -__k_ENOENT);

    // An unknown flag, a session leader that joins a group, and a negative pgid
    ktest_expect(spawn_args(CHILD, child_argv, 0x100, 0, nullptr, 0), -__k_EINVAL);
    ktest_expect(spawn_args(CHILD, child_argv, __k_SPAWN_SETSID | __k_SPAWN_SETPGID, 0, nullptr, 0), -__k_EINVAL);
    ktest_expect(spawn_args(CHILD, child_argv, __k_SPAWN_SETPGID, -1, nullptr, 0), -__k_EINVAL);

    // A group that is not there: the move fails with the rights of setpgid
    ktest_expect(spawn_args(CHILD, child_argv, __k_SPAWN_SETPGID, 999999, nullptr, 0), -__k_EPERM);

    // An unknown action and a reserved field that is not zero
    union __k_spawn_action bad[1];
    memset(bad, 0, sizeof(bad));
    bad[0].op = 99;
    ktest_expect(spawn_args(CHILD, child_argv, 0, 0, bad, 1), -__k_EINVAL);

    memset(bad, 0, sizeof(bad));
    bad[0].open.op = __k_SPAWN_OPEN;
    bad[0].open.dirfd = __k_AT_FDCWD;
    bad[0].open.open_flags = __k_O_RDONLY;
    bad[0].open.reserved = 1;
    bad[0].open.path = (long)("input.txt");
    ktest_expect(spawn_args(CHILD, child_argv, 0, 0, bad, 1), -__k_EINVAL);

    // An action that fails on its own fails the whole call: nothing spawns
    memset(bad, 0, sizeof(bad));
    bad[0].open.op = __k_SPAWN_OPEN;
    bad[0].open.dirfd = __k_AT_FDCWD;
    bad[0].open.open_flags = __k_O_RDONLY;
    bad[0].open.path = (long)("not-there.txt");
    ktest_expect(spawn_args(CHILD, child_argv, 0, 0, bad, 1), -__k_ENOENT);

    memset(bad, 0, sizeof(bad));
    bad[0].dup2.op = __k_SPAWN_DUP2;
    bad[0].dup2.oldfd = 77;
    bad[0].dup2.newfd = 1;
    ktest_expect(spawn_args(CHILD, child_argv, 0, 0, bad, 1), -__k_EBADF);

    // A close of a descriptor that is not open is not an error
    memset(bad, 0, sizeof(bad));
    bad[0].close.op = __k_SPAWN_CLOSE;
    bad[0].close.fd = 77;
    long child = spawn_args(CHILD, child_argv, 0, 0, bad, 1);
    ktest_expect(child > 0, 1);
    wait_code(child, 42);
}

int main(int argc, char** argv)
{
    if(argc >= 2 && ktest_string_equal(argv[1], "--child")) {
        return child_main(argc - 2, argv + 2);
    }

    ktest_enter_work_directory("spawn");

    test_arguments();
    test_exit_code();
    test_empty_fds();
    test_stdin_redirect();
    test_setsid();
    test_setpgid();
    test_errors();

    return ktest_result();
}
