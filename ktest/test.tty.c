#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/futex.h>
#include <vex/ioctl.h>
#include <vex/mman.h>
#include <vex/serial.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/termios.h>
#include <vex/time.h>
#include <vex/wait.h>

#include "ktest.h"

static volatile unsigned int never_changes;

#define LINE_RECEIVE_ERROR __k_IOCTL_WRITE(sizeof(__k_int32_t), 't', 0x01) /* The far end's own, in serial_test.c */
#define LINE_ERROR_BREAK   1
#define LINE_ERROR_PARITY  2
#define LINE_ERROR_OVERRUN 4
#define SERIAL_TEST_DEVICE "/dev/serial1337" /* The number is in serial_test.c */
#define LINE_FILL_SIZE     3900              /* Leaves the 4 KiB input ring less room than IXOFF's margin */

static long tty_fd;
static long line_fd;
static long serial_fd;
static char line_fill[LINE_FILL_SIZE];

static void line_send(const char* text)
{
    ktest_syscall3(__k_SYS_WRITE, line_fd, (long)text, ktest_strlen(text));
}

static void line_error(int error, int byte)
{
    __k_int32_t value = (error << 8) | byte;
    ktest_syscall3(__k_SYS_IOCTL, line_fd, LINE_RECEIVE_ERROR, (long)&value);
}

/* Whether the next read gives exactly what is expected */
static int expect_bytes(long fd, const char* expected, long expected_length)
{
    char buffer[128];
    long length = ktest_read(fd, buffer, sizeof(buffer));

    return length == expected_length && ktest_bytes_equal(buffer, expected, length);
}

static int expect_read(long fd, const char* expected)
{
    return expect_bytes(fd, expected, ktest_strlen(expected));
}

/* What the terminal sends may come in pieces, as tty_worker echoes before the kernel prints a status line */
static int expect_line(const char* expected)
{
    char buffer[128];
    long expected_length = ktest_strlen(expected);
    long length = 0;
    long piece = 1;

    while(length < expected_length && piece > 0) {
        piece = ktest_read(line_fd, buffer + length, sizeof(buffer) - length);

        if(piece > 0) {
            length += piece;
        }
    }

    return length == expected_length && ktest_bytes_equal(buffer, expected, length);
}

static void line_drain(void)
{
    char buffer[128];

    while(ktest_syscall3(__k_SYS_READ, line_fd, (long)buffer, sizeof(buffer)) > 0) {
        /* empty */
    }
}

static void tty_raw(int vmin, int vtime)
{
    struct __k_termios termios;

    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TCGETS, (long)&termios);
    termios.c_lflag &= ~(__k_ICANON | __k_ECHO);
    termios.c_cc[__k_VMIN] = vmin;
    termios.c_cc[__k_VTIME] = vtime;
    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TCSETS, (long)&termios);
}

static void tty_set_iflag(__k_uint32_t iflag)
{
    struct __k_termios termios;

    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TCGETS, (long)&termios);
    termios.c_iflag = iflag;
    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TCSETS, (long)&termios);
}

/* Before TIOCSCTTY, so that BRKINT's SIGINT has no foreground group to reach */
static void test_tty_line(void)
{
    long total = 0;
    long length = 1;
    long i;

    tty_raw(1, 0);

    /* PARMRK marks a parity error and a break, and doubles a \377 that came intact */
    tty_set_iflag(__k_INPCK | __k_PARMRK);
    line_error(LINE_ERROR_PARITY, 'x');
    ktest_expect(expect_bytes(tty_fd, "\377\0x", 3), 1);
    line_send("\377");
    ktest_expect(expect_bytes(tty_fd, "\377\377", 2), 1);
    line_error(LINE_ERROR_BREAK, 0);
    ktest_expect(expect_bytes(tty_fd, "\377\0\0", 3), 1);

    /* Without PARMRK the error reads as a NUL, IGNPAR drops it, and without INPCK the byte stays */
    tty_set_iflag(__k_INPCK);
    line_error(LINE_ERROR_PARITY, 'x');
    ktest_expect(expect_bytes(tty_fd, "\0", 1), 1);
    tty_set_iflag(__k_INPCK | __k_IGNPAR);
    line_error(LINE_ERROR_PARITY, 'x');
    line_send("y");
    ktest_expect(expect_read(tty_fd, "y"), 1);
    tty_set_iflag(0);
    line_error(LINE_ERROR_PARITY, 'z');
    ktest_expect(expect_read(tty_fd, "z"), 1);
    line_error(LINE_ERROR_OVERRUN, 'o');
    ktest_expect(expect_read(tty_fd, "o"), 1);

    /* BRKINT throws away what came before the break */
    tty_set_iflag(__k_BRKINT);
    line_send("abc");
    line_error(LINE_ERROR_BREAK, 0);
    line_drain();
    line_send("q");
    ktest_expect(expect_read(tty_fd, "q"), 1);

    /* IXOFF sends VSTOP as the input fills, and VSTART once a read emptied it */
    tty_set_iflag(__k_IXOFF);

    for(i = 0; i < LINE_FILL_SIZE; ++i) {
        line_fill[i] = 'a';
    }

    ktest_syscall3(__k_SYS_WRITE, line_fd, (long)line_fill, LINE_FILL_SIZE);
    ktest_expect(expect_line("\x13"), 1);

    while(total < LINE_FILL_SIZE && length > 0) {
        length = ktest_syscall3(__k_SYS_READ, tty_fd, (long)line_fill, LINE_FILL_SIZE);

        if(length > 0) {
            total += length;
        }
    }

    ktest_expect(total, 3900);
    ktest_expect(expect_line("\x11"), 1);
}

/* Answers the AT command an exchange sends, as a modem would */
[[noreturn]] static void modem_answerer(long arg)
{
    char buffer[16];
    long length = 0;
    long rounds = 0;
    long piece;

    while(length < 3 && rounds < 10) {
        piece = ktest_syscall3(__k_SYS_READ, line_fd, (long)(buffer + length), sizeof(buffer) - length);

        if(piece > 0) {
            length += piece;
        }

        rounds += 1;
    }

    if(length == 3 && ktest_bytes_equal(buffer, "AT\r", 3)) {
        line_send("OK\r\n");
    }

    ktest_syscall1(__k_SYS_THREAD_EXIT, 0);
    __builtin_unreachable();
}

/* Leaves the serial test port bound to a terminal, and serial_fd its controlling file */
static void test_serial(void)
{
    struct __k_serial_info info;
    struct __k_serial_line line;
    struct __k_serial_exchange exchange;
    struct __k_serial_binding binding = {};
    char answer[16];
    long viewer;

    viewer = ktest_open(SERIAL_TEST_DEVICE, __k_O_RDONLY, 0);
    ktest_expect(viewer >= 0, 1);
    ktest_syscall3(__k_SYS_IOCTL, viewer, __k_SERIAL_GET_INFO, (long)&info);
    ktest_expect(ktest_string_equal(info.driver, "serial_test"), 1);
    ktest_syscall3(__k_SYS_IOCTL, viewer, __k_SERIAL_GET_LINE, (long)&line);
    ktest_expect(line.speed, 38400);

    /* Only a file that keeps everyone else out changes the port */
    line.speed = 115200;
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, viewer, __k_SERIAL_SET_LINE, (long)&line), -__k_EPERM);
    ktest_expect(ktest_open(SERIAL_TEST_DEVICE, __k_O_RDWR | __k_O_EXCL, 0), -__k_EBUSY);
    ktest_close(viewer);

    serial_fd = ktest_open(SERIAL_TEST_DEVICE, __k_O_RDWR | __k_O_EXCL, 0);
    ktest_expect(serial_fd >= 0, 1);
    ktest_expect(ktest_open(SERIAL_TEST_DEVICE, __k_O_RDONLY, 0), -__k_EBUSY);
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, serial_fd, __k_SERIAL_SET_LINE, (long)&line), 0);

    /* An unbound port talks to what is on the line, here a modem that answers OK */
    exchange.send = (long)"AT\r";
    exchange.send_size = 3;
    exchange.receive = (long)answer;
    exchange.receive_size = sizeof(answer);
    exchange.timeout_msec = 2000;
    exchange.terminator = '\n';
    ktest_spawn_joinable(&modem_answerer, 0);
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, serial_fd, __k_SERIAL_EXCHANGE, (long)&exchange), 0);
    ktest_join();
    ktest_expect(exchange.receive_size == 4 && ktest_bytes_equal(answer, "OK\r\n", 4), 1);

    binding.consumer[0] = 't';
    binding.consumer[1] = 't';
    binding.consumer[2] = 'y';
    binding.index = __k_SERIAL_ANY_INDEX;
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, serial_fd, __k_SERIAL_BIND, (long)&binding), 0);
    ktest_expect(ktest_string_equal(binding.object, "tty0"), 1);
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, serial_fd, __k_SERIAL_BIND, (long)&binding), -__k_EBUSY);
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, serial_fd, __k_SERIAL_EXCHANGE, (long)&exchange), -__k_EBUSY);
}

// The child waits until it is in a group of its own, which the parent moves it to
static void wait_for_own_group(void)
{
    while(ktest_syscall1(__k_SYS_GETPGID, 0) != ktest_syscall1(__k_SYS_GETPID, 0)) {
        ktest_syscall1(__k_SYS_YIELD, 0);
    }
}

static long start_background(void (*job)(void))
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        wait_for_own_group();
        job();
        ktest_syscall1(__k_SYS_EXIT, 6);
    }

    ktest_syscall2(__k_SYS_SETPGID, child, child);

    return child;
}

static void read_in_background(void)
{
    char buffer[8];
    long length = ktest_read(tty_fd, buffer, sizeof(buffer));

    if(length == 2 && buffer[0] == 'k') {
        ktest_syscall1(__k_SYS_EXIT, 7);
    }
}

static void write_in_background(void)
{
    if(ktest_syscall3(__k_SYS_WRITE, tty_fd, (long)"x", 1) == 1) {
        ktest_syscall1(__k_SYS_EXIT, 7);
    }
}

static void write_ignoring_in_background(void)
{
    struct __k_sigaction action = {};
    action.sa_handler = __k_SIG_IGN;
    ktest_syscall3(__k_SYS_SIGACTION, __k_SIGTTOU, (long)(&action), 0);
    write_in_background();
}

static void read_ignoring_in_background(void)
{
    char buffer[8];
    struct __k_sigaction action = {};
    action.sa_handler = __k_SIG_IGN;
    ktest_syscall3(__k_SYS_SIGACTION, __k_SIGTTIN, (long)(&action), 0);

    if(ktest_read(tty_fd, buffer, sizeof(buffer)) == -__k_EIO) {
        ktest_syscall1(__k_SYS_EXIT, 7);
    }
}

static void set_termios_in_background(void)
{
    struct __k_termios termios;

    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TCGETS, (long)&termios);

    if(ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TCSETS, (long)&termios) == 0) {
        ktest_syscall1(__k_SYS_EXIT, 7);
    }
}

// As a shell does it: a process in the background that sets the foreground group stops, unless it ignores SIGTTOU meanwhile
static long set_foreground(__k_pid_t pgid)
{
    struct __k_sigaction ignore = {};
    struct __k_sigaction previous = {};

    ignore.sa_handler = __k_SIG_IGN;
    ktest_syscall3(__k_SYS_SIGACTION, __k_SIGTTOU, (long)(&ignore), (long)(&previous));

    long retval = ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TIOCSPGRP, (long)(&pgid));

    ktest_syscall3(__k_SYS_SIGACTION, __k_SIGTTOU, (long)(&previous), 0);

    return retval;
}

static void set_foreground_in_background(void)
{
    __k_pid_t pgid = ktest_syscall1(__k_SYS_GETPID, 0);

    if(ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TIOCSPGRP, (long)(&pgid)) == 0) {
        ktest_syscall1(__k_SYS_EXIT, 7);
    }
}

static long wait_for(long child, long options, int* restrict wstatus)
{
    return ktest_syscall3(__k_SYS_WAITPID, child, (long)wstatus, options);
}

static void expect_stop(void (*job)(void), int sig)
{
    int wstatus = -1;
    long child = start_background(job);

    ktest_expect(wait_for(child, __k_WUNTRACED, &wstatus), child);
    ktest_expect(__k_WIFSTOPPED(wstatus) && __k_WSTOPSIG(wstatus) == sig, 1);
    ktest_syscall2(__k_SYS_KILL, child, __k_SIGKILL);
    wait_for(child, 0, &wstatus);
}

static void expect_exit(void (*job)(void), int code)
{
    int wstatus = -1;
    long child = start_background(job);

    ktest_expect(wait_for(child, 0, &wstatus), child);
    ktest_expect(wstatus, __k_W_EXITCODE(code, 0));
}

// The background job of an orphaned group cannot be continued by anyone, so it gets an error instead of a stop
static void test_tty_orphaned(void)
{
    volatile int* result = (volatile int*)(ktest_syscall6(__k_SYS_MMAP, 0, 4096, __k_PROT_READ | __k_PROT_WRITE,
        __k_MAP_SHARED | __k_MAP_ANONYMOUS, -1, 0));
    int wstatus = -1;
    long middle = ktest_syscall1(__k_SYS_FORK, 0);

    if(middle == 0) {
        long grandchild = ktest_syscall1(__k_SYS_FORK, 0);

        if(grandchild == 0) {
            char buffer[8];

            ktest_syscall2(__k_SYS_SETPGID, 0, 0);

            // The group is orphaned once the middle process, its only link to the session, is gone
            while(ktest_syscall1(__k_SYS_GETPPID, 0) != 1) {
                ktest_syscall1(__k_SYS_YIELD, 0);
            }

            if(ktest_read(tty_fd, buffer, sizeof(buffer)) == -__k_EIO) {
                *result = 7;
            }
            else {
                *result = 6;
            }

            ktest_syscall1(__k_SYS_EXIT, 0);
        }

        ktest_syscall1(__k_SYS_EXIT, 0);
    }

    ktest_syscall2(__k_SYS_SETPGID, middle, middle);
    wait_for(middle, 0, &wstatus);

    struct __k_timespec delay = {};
    delay.tv_nsec = 20000000;

    for(int round = 0; round < 100 && *result == 0; round += 1) {
        ktest_syscall4(__k_SYS_NANOSLEEP, __k_CLOCK_MONOTONIC, 0, (long)&delay, 0);
    }

    ktest_expect(*result, 7);
}

static void test_tty_background(long pid)
{
    struct __k_termios saved;
    struct __k_termios tostop;
    __k_pid_t pgid;
    int wstatus = -1;
    long child = start_background(&read_in_background);

    // A read stops the job, and it reads once it has the terminal and is continued
    ktest_expect(wait_for(child, __k_WUNTRACED, &wstatus), child);
    ktest_expect(__k_WIFSTOPPED(wstatus) && __k_WSTOPSIG(wstatus) == __k_SIGTTIN, 1);

    pgid = child;
    ktest_expect(set_foreground(pgid), 0);
    ktest_syscall2(__k_SYS_KILL, -child, __k_SIGCONT);
    line_send("k\n");
    line_drain();
    ktest_expect(wait_for(child, 0, &wstatus), child);
    ktest_expect(wstatus, __k_W_EXITCODE(7, 0));

    pgid = pid;
    ktest_expect(set_foreground(pgid), 0);

    expect_exit(&read_ignoring_in_background, 7);

    // Output goes through from the background until the terminal asks for the stop
    expect_exit(&write_in_background, 7);
    line_drain();

    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TCGETS, (long)&saved);
    tostop = saved;
    tostop.c_lflag |= __k_TOSTOP;
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TCSETS, (long)&tostop), 0);

    expect_stop(&write_in_background, __k_SIGTTOU);
    expect_stop(&set_termios_in_background, __k_SIGTTOU);
    expect_stop(&set_foreground_in_background, __k_SIGTTOU);
    expect_exit(&write_ignoring_in_background, 7);
    line_drain();

    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TCSETS, (long)&saved), 0);

    test_tty_orphaned();
}

static void test_tty(void)
{
    struct __k_termios saved;
    char buffer[64];
    __k_int32_t value = 0;
    __k_pid_t pgid;
    long pid = ktest_syscall1(__k_SYS_GETPID, 0);
    char status[64];
    struct __k_winsize winsize = {};
    struct __k_winsize winsize_back;
    long child;
    int wstatus;

    winsize.ws_row = 24;
    winsize.ws_col = 80;

    line_fd = ktest_open("/dev/serialtest", __k_O_RDWR, 0);
    test_serial();
    tty_fd = ktest_open("/dev/tty0", __k_O_RDWR, 0);
    ktest_expect(tty_fd >= 0 && line_fd >= 0, 1);

    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TCGETS, (long)&saved);
    ktest_expect(saved.c_cc[__k_VMIN] == 1 && (saved.c_lflag & __k_ICANON), 1);

    /* DEL erases with ECHOE, and ONLCR echoes the newline as \r\n */
    line_send("ab\x7f"
              "c\n");
    ktest_expect(expect_line("ab\b \bc\r\n"), 1);
    ktest_expect(expect_read(tty_fd, "ac\n"), 1);

    ktest_syscall3(__k_SYS_WRITE, tty_fd, (long)"x\ny", 3);
    ktest_expect(expect_line("x\r\ny"), 1);

    /* ^U kills the line and ^W the last word */
    line_send("abc\x15"
              "d\n");
    line_drain();
    ktest_expect(expect_read(tty_fd, "d\n"), 1);
    line_send("foo bar\x17"
              "x\n");
    line_drain();
    ktest_expect(expect_read(tty_fd, "foo x\n"), 1);

    /* ^D on a line hands it out, and on an empty one reads as end of file */
    line_send("ab\x04");
    line_drain();
    ktest_expect(expect_read(tty_fd, "ab"), 1);
    line_send("\x04");
    line_drain();
    ktest_expect(ktest_syscall3(__k_SYS_READ, tty_fd, (long)buffer, sizeof(buffer)), 0);

    /* A line is readable once it ends */
    line_send("hi\n");
    line_drain();
    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_FIONREAD, (long)&value);
    ktest_expect(value, 3);
    ktest_expect(expect_read(tty_fd, "hi\n"), 1);

    /* VMIN 0 and VTIME 2 give up after 0.2 s, VMIN 3 waits for three bytes */
    tty_raw(0, 2);
    ktest_expect(ktest_syscall3(__k_SYS_READ, tty_fd, (long)buffer, sizeof(buffer)), 0);
    tty_raw(3, 0);
    line_send("xyz");
    ktest_expect(expect_read(tty_fd, "xyz"), 1);

    ktest_syscall3(__k_SYS_FCNTL, tty_fd, __k_F_SETFL, __k_O_NONBLOCK);
    ktest_expect(ktest_syscall3(__k_SYS_READ, tty_fd, (long)buffer, sizeof(buffer)), -__k_EAGAIN);
    ktest_syscall3(__k_SYS_FCNTL, tty_fd, __k_F_SETFL, 0);

    test_tty_line();
    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TCSETS, (long)&saved);

    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TIOCSWINSZ, (long)&winsize);
    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TIOCGWINSZ, (long)&winsize_back);
    ktest_expect(winsize_back.ws_row == 24 && winsize_back.ws_col == 80, 1);

    /* The body leads a session without a terminal, so the terminal can become its controlling one */
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TIOCGPGRP, (long)&pgid), -__k_ENOTTY);
    value = 0;
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TIOCSCTTY, (long)&value), 0);
    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TIOCGPGRP, (long)&pgid);
    ktest_expect(pgid, pid);
    ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TIOCGSID, (long)&pgid);
    ktest_expect(pgid, pid);

    /* ^C goes to the foreground group, a job in a group of its own */
    child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child == 0) {
        for(;;) {
            ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)&never_changes, 0, 0, 0);
        }
    }

    ktest_syscall2(__k_SYS_SETPGID, child, child);
    pgid = child;
    ktest_expect(set_foreground(pgid), 0);
    line_send("\x03");
    wstatus = -1;
    ktest_syscall3(__k_SYS_WAITPID, child, (long)&wstatus, 0);
    ktest_expect(wstatus, 2);
    ktest_expect(expect_line("^C"), 1);

    pgid = pid;
    ktest_expect(set_foreground(pgid), 0);

    test_tty_background(pid);

    /* ^T has the kernel print the foreground group, init ignores the SIGINFO */
    line_send("\x14");
    ktest_snprintf(status, sizeof(status), "^Tprocess group %ld: %ld (running)\r\n", pid, pid);
    ktest_expect(expect_line(status), 1);

    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TIOCNOTTY, 0), 0);
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, tty_fd, __k_TIOCGPGRP, (long)&pgid), -__k_ENOTTY);

    /* The terminal goes away with the binding, but not while it is open */
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, serial_fd, __k_SERIAL_UNBIND, 0), -__k_EBUSY);
    ktest_close(tty_fd);
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, serial_fd, __k_SERIAL_UNBIND, 0), 0);
    ktest_expect(ktest_open("/dev/tty0", __k_O_RDWR, 0), -__k_ENOENT);

    // The port keeps its speed, and the next run expects the one that it started with
    struct __k_serial_line line = {};
    ktest_syscall3(__k_SYS_IOCTL, serial_fd, __k_SERIAL_GET_LINE, (long)&line);
    line.speed = 38400;
    ktest_expect(ktest_syscall3(__k_SYS_IOCTL, serial_fd, __k_SERIAL_SET_LINE, (long)&line), 0);
    ktest_close(serial_fd);
    ktest_close(line_fd);
}

static int run(void)
{
    struct __k_sigaction action = {};

    // The terminal sends SIGINFO for ^T, which kills a process that does not ignore it
    action.sa_handler = __k_SIG_IGN;
    ktest_syscall3(__k_SYS_SIGACTION, __k_SIGINFO, (long)(&action), 0);

    test_tty();

    return ktest_result();
}

int main(void)
{
    return ktest_in_new_session(&run);
}
