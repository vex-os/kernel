#include <vex/fcntl.h>
#include <vex/ioctl.h>
#include <vex/kplog.h>
#include <vex/termios.h>
#include <vex/time.h>
#include <vex/utsname.h>
#include <vex/wait.h>

#include "ktest.h"

#define TEST_PATH_PREFIX "/kst/"
#define TEST_PATH_SUFFIX ".elf"
#define TEST_PATH_SIZE   128
#define LINE_SIZE        128
#define WORDS_MAX        16
#define PROMPT           "> "
#define CLEAR_SCREEN     "\033[3J\033[2J\033[H"
#define EXEC_FAILED_EXIT 127

#define NANOSECONDS_PER_FRACTION 100000
#define PREFIX_GOOD              "[ OK ] "
#define PREFIX_FAIL              "[FAIL] "

static const char* const test_names[] = { KST_TEST_NAMES };

static long runner_group;

static long become_foreground(long group)
{
    __k_pid_t pgid = group;
    return ktest_syscall3(__k_SYS_IOCTL, KTEST_STDIN, __k_TIOCSPGRP, (long)(&pgid));
}

static void set_signals(bool enabled)
{
    struct __k_termios termios = {};

    ktest_syscall3(__k_SYS_IOCTL, KTEST_STDIN, __k_TCGETS, (long)(&termios));

    if(enabled) {
        termios.c_lflag |= __k_ISIG;
    }
    else {
        termios.c_lflag &= ~__k_ISIG;
    }

    ktest_syscall3(__k_SYS_IOCTL, KTEST_STDIN, __k_TCSETS, (long)(&termios));
}

// A serial terminal sends CR for the Enter key, and has no modem lines to wait for
static void set_line_discipline(void)
{
    struct __k_termios termios = {};

    ktest_syscall3(__k_SYS_IOCTL, KTEST_STDIN, __k_TCGETS, (long)(&termios));

    termios.c_iflag |= __k_ICRNL;
    termios.c_oflag |= __k_OPOST | __k_ONLCR;
    termios.c_cflag |= __k_CLOCAL | __k_CREAD;
    termios.c_lflag |= __k_ICANON | __k_ECHO | __k_ECHOE | __k_ECHOK;
    termios.c_lflag &= ~__k_ISIG;

    ktest_syscall3(__k_SYS_IOCTL, KTEST_STDIN, __k_TCSETS, (long)(&termios));
}

static long mount_devfs(void)
{
    ktest_syscall3(__k_SYS_MKDIRAT, __k_AT_FDCWD, (long)("/dev"), 0755);
    return ktest_syscall6(__k_SYS_MOUNT, 0, (long)("/dev"), (long)("devfs"), 0, 0, 0);
}

// Makes the terminal the standard input, output and error, and the controlling terminal of the session init leads
static long open_terminal(void)
{
    long terminal = ktest_open(KST_TERMINAL_PATH, __k_O_RDWR, 0);

    if(terminal < 0) {
        return terminal;
    }

    __k_int32_t steal = 0;
    long retval = ktest_syscall3(__k_SYS_IOCTL, terminal, __k_TIOCSCTTY, (long)(&steal));

    if(retval < 0) {
        ktest_close(terminal);
        return retval;
    }

    ktest_kplog_close();

    for(long fd = KTEST_STDIN; fd <= KTEST_STDERR; ++fd) {
        if(fd != terminal) {
            ktest_syscall3(__k_SYS_DUP, terminal, fd, 0);
        }
    }

    if(terminal > KTEST_STDERR) {
        ktest_close(terminal);
    }

    return 0;
}

static void print_banner(void)
{
    struct __k_utsname name = {};

    ktest_syscall1(__k_SYS_UNAME, (long)(&name));

    ktest_printf("kst: %s %s %s %s\n", name.sysname, name.release, name.version, name.machine);
    ktest_printf("kst: type help for the commands\n");
}

static void print_help(void)
{
    ktest_printf("help           this text\n");
    ktest_printf("list           the tests, in the order that all runs them\n");
    ktest_printf("run <test>...  run tests by name, or programs by path\n");
    ktest_printf("all            run every test\n");
    ktest_printf("clear          clear the screen\n");
    ktest_printf("exit           leave, which panics the kernel as init died\n");
}

static void print_list(void)
{
    for(size_t i = 0; i < sizeof(test_names) / sizeof(test_names[0]); ++i) {
        ktest_printf("%s\n", test_names[i]);
    }
}

static bool build_test_path(char* buffer, const char* name)
{
    if(name[0] == '/') {
        if(ktest_strlen(name) >= TEST_PATH_SIZE) {
            return false;
        }

        memcpy(buffer, name, ktest_strlen(name) + 1);
        return true;
    }

    size_t prefix = sizeof(TEST_PATH_PREFIX) - 1;
    size_t suffix = sizeof(TEST_PATH_SUFFIX);
    size_t length = ktest_strlen(name);

    if(prefix + length + suffix > TEST_PATH_SIZE) {
        return false;
    }

    memcpy(buffer, TEST_PATH_PREFIX, prefix);
    memcpy(buffer + prefix, name, length);
    memcpy(buffer + prefix + length, TEST_PATH_SUFFIX, suffix);

    return true;
}

static void __attribute__((noreturn)) exec_program(const char* path)
{
    const char* argv[] = { path, nullptr };
    const char* envp[] = { nullptr };

    long retval = ktest_syscall3(__k_SYS_EXEC, (long)(path), (long)(argv), (long)(envp));
    ktest_dprintf(KTEST_STDERR, "kst: cannot execute %s: %ld\n", path, retval);
    ktest_exit(EXEC_FAILED_EXIT);
}

// Waits for the program with the terminal in its hands, so that ^C reaches it and not the runner
static long wait_program(long child)
{
    int status = -1;

    ktest_syscall2(__k_SYS_SETPGID, child, child);
    become_foreground(child);
    set_signals(true);

    long waited = ktest_syscall3(__k_SYS_WAITPID, child, (long)(&status), 0);

    set_signals(false);
    become_foreground(runner_group);

    if(waited < 0) {
        return waited;
    }

    return status;
}

static void print_result_prefix(const char* prefix)
{
    struct __k_timespec now = {};

    ktest_syscall2(__k_SYS_GETTIME, __k_CLOCK_MONOTONIC, (long)(&now));

    // Same layout as the timestamp of the kernel log
    ktest_printf("%s[%5lu.%04lu] ", prefix, (unsigned long)(now.tv_sec), (unsigned long)(now.tv_nsec / NANOSECONDS_PER_FRACTION));
}

static bool run_program(const char* name)
{
    char path[TEST_PATH_SIZE];

    if(!build_test_path(path, name)) {
        print_result_prefix(PREFIX_FAIL);
        ktest_printf("%s (name too long)\n", name);
        return false;
    }

    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child < 0) {
        print_result_prefix(PREFIX_FAIL);
        ktest_printf("%s (fork %ld)\n", name, child);
        return false;
    }

    if(child == 0) {
        ktest_syscall2(__k_SYS_SETPGID, 0, 0);
        exec_program(path);
    }

    long status = wait_program(child);

    if(status < 0) {
        print_result_prefix(PREFIX_FAIL);
        ktest_printf("%s (waitpid %ld)\n", name, status);
    }
    else if(__k_WIFSIGNALED(status)) {
        print_result_prefix(PREFIX_FAIL);
        ktest_printf("%s (signal %ld)\n", name, __k_WTERMSIG(status));
    }
    else if(__k_WEXITSTATUS(status)) {
        print_result_prefix(PREFIX_FAIL);
        ktest_printf("%s (exit %ld)\n", name, __k_WEXITSTATUS(status));
    }
    else {
        print_result_prefix(PREFIX_GOOD);
        ktest_printf("%s\n", name);
        return true;
    }

    return false;
}

static void run_programs(const char* const* names, size_t count)
{
    size_t passed = 0;

    for(size_t i = 0; i < count; ++i) {
        if(run_program(names[i])) {
            passed += 1;
        }
    }

    ktest_printf("kst: done passed=%lu failed=%lu\n", (unsigned long)(passed), (unsigned long)(count - passed));
}

static size_t split_words(char* line, char** words, size_t capacity)
{
    size_t count = 0;

    while(*line && count < capacity) {
        while(*line == ' ' || *line == '\t') {
            *line = '\0';
            line += 1;
        }

        if(*line == '\0') {
            break;
        }

        words[count] = line;
        count += 1;

        while(*line && *line != ' ' && *line != '\t') {
            line += 1;
        }
    }

    return count;
}

static void strip_line_end(char* line, size_t length)
{
    while(length && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        length -= 1;
    }

    line[length] = '\0';
}

// Returns false when the runner has to stop
static bool handle_command(char** words, size_t count)
{
    const char* command = words[0];

    if(ktest_string_equal(command, "help")) {
        print_help();
    }
    else if(ktest_string_equal(command, "list")) {
        print_list();
    }
    else if(ktest_string_equal(command, "run")) {
        run_programs((const char* const*)(words + 1), count - 1);
    }
    else if(ktest_string_equal(command, "all")) {
        run_programs(test_names, sizeof(test_names) / sizeof(test_names[0]));
    }
    else if(ktest_string_equal(command, "clear")) {
        ktest_puts(KTEST_STDOUT, CLEAR_SCREEN);
    }
    else if(ktest_string_equal(command, "exit")) {
        return false;
    }
    else {
        ktest_printf("kst: unknown command %s\n", command);
    }

    return true;
}

static int command_loop(void)
{
    char line[LINE_SIZE];
    char* words[WORDS_MAX];

    for(;;) {
        ktest_puts(KTEST_STDOUT, PROMPT);

        long length = ktest_read(KTEST_STDIN, line, sizeof(line) - 1);

        if(length == 0) {
            ktest_printf("kst: end of input\n");
            return 0;
        }

        if(length < 0) {
            ktest_printf("kst: read failed: %ld\n", length);
            return 1;
        }

        strip_line_end(line, length);

        size_t count = split_words(line, words, WORDS_MAX);

        if(count && !handle_command(words, count)) {
            return 0;
        }
    }
}

int main(int argc, char** argv)
{
    long retval = mount_devfs();

    if(retval < 0) {
        ktest_kplog(__k_KP_ERROR, "kst: mounting devfs failed: %ld", retval);
        return 1;
    }

    ktest_kplog(__k_KP_NOTICE, "kst: opening %s", KST_TERMINAL_PATH);

    retval = open_terminal();

    if(retval < 0) {
        ktest_kplog(__k_KP_ERROR, "kst: cannot use %s: %ld, is a serial port bound to it with serial= on the kernel command line?",
            KST_TERMINAL_PATH, retval);
        return 1;
    }

    runner_group = ktest_syscall1(__k_SYS_GETPGID, 0);

    set_line_discipline();

    ktest_puts(KTEST_STDOUT, CLEAR_SCREEN);
    print_banner();

    return command_loop();
}
