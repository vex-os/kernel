#pragma once

#include <stdarg.h>
#include <stddef.h>

#include <vex/syscall.h>

#define KTEST_STDIN  0
#define KTEST_STDOUT 1
#define KTEST_STDERR 2

#define KTEST_THREAD_STACK_SIZE 4096

// Compares the value of the expression to the expected one, and goes on after a mismatch
#define ktest_expect(expression, expected)                                                      \
    do {                                                                                        \
        long ktest_actual = (long)(expression);                                                 \
        long ktest_wanted = (long)(expected);                                                   \
        if(ktest_actual != ktest_wanted) {                                                      \
            ktest_report_mismatch(__FILE__, __LINE__, #expression, ktest_actual, ktest_wanted); \
        }                                                                                       \
    } while(0)

#define ktest_assert(condition)                                                        \
    do {                                                                               \
        if(!(condition)) {                                                             \
            ktest_fail("%s:%d: assertion failed: %s", __FILE__, __LINE__, #condition); \
        }                                                                              \
    } while(0)

static inline long ktest_syscall1(long number, long arg1)
{
    long result;
    asm volatile("syscall" : "=a"(result) : "a"(number), "D"(arg1) : "rcx", "r11", "memory");
    return result;
}

static inline long ktest_syscall2(long number, long arg1, long arg2)
{
    long result;
    asm volatile("syscall" : "=a"(result) : "a"(number), "D"(arg1), "S"(arg2) : "rcx", "r11", "memory");
    return result;
}

static inline long ktest_syscall3(long number, long arg1, long arg2, long arg3)
{
    long result;
    asm volatile("syscall" : "=a"(result) : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3) : "rcx", "r11", "memory");
    return result;
}

static inline long ktest_syscall4(long number, long arg1, long arg2, long arg3, long arg4)
{
    long result;
    register long r10 asm("r10") = arg4;
    asm volatile("syscall" : "=a"(result) : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10) : "rcx", "r11", "memory");
    return result;
}

static inline long ktest_syscall5(long number, long arg1, long arg2, long arg3, long arg4, long arg5)
{
    long result;
    register long r10 asm("r10") = arg4;
    register long r8 asm("r8") = arg5;
    asm volatile("syscall" : "=a"(result) : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return result;
}

static inline long ktest_syscall6(long number, long arg1, long arg2, long arg3, long arg4, long arg5, long arg6)
{
    long result;
    register long r10 asm("r10") = arg4;
    register long r8 asm("r8") = arg5;
    register long r9 asm("r9") = arg6;
    asm volatile("syscall"
        : "=a"(result)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return result;
}

/// Prints the message to the standard error and exits with code 1
[[noreturn]] void ktest_fail(const char* format, ...);

[[noreturn]] void ktest_exit(int code);

/// Prints the mismatch and counts it, so that ktest_result tells that the test failed
void ktest_report_mismatch(const char* file, int line, const char* expression, long actual, long expected);

/// @return 0 when every ktest_expect held, 1 otherwise; the test returns it from main
int ktest_result(void);

size_t ktest_strlen(const char* string);

bool ktest_string_equal(const char* left, const char* right);

bool ktest_bytes_equal(const void* left, const void* right, size_t count);

bool ktest_has_prefix(const char* string, const char* prefix);

long ktest_open(const char* path, unsigned flags, unsigned mode);

/// @return Whether the file starts with the same bytes as the string does, and holds nothing less
bool ktest_file_has_prefix(const char* path, const char* prefix);

long ktest_write(long fd, const void* buffer, size_t size);

long ktest_read(long fd, void* buffer, size_t size);

void ktest_close(long fd);

void ktest_puts(long fd, const char* string);

/// Understands %s %c %d %u %x %% with the l and ll length modifiers, and a zero flag with a width
void ktest_vdprintf(long fd, const char* format, va_list args);

void ktest_dprintf(long fd, const char* format, ...);

/// Formats like ktest_printf into the buffer and cuts the result to the size
size_t ktest_snprintf(char* buffer, size_t size, const char* format, ...);

void ktest_printf(const char* format, ...);

/// Writes one line to the kernel log, and opens /dev/kplog on the first call
/// @pre /dev/kplog exists and the process has CAP_SYSLOG
/// @param level A __k_KP_* level from __k_KP_ERROR to __k_KP_VERBOSE
/// @note Does nothing when /dev/kplog does not open
void ktest_kplog(unsigned level, const char* format, ...);

void ktest_kplog_close(void);

void* memcpy(void* restrict destination, const void* restrict source, size_t count);
void* memmove(void* destination, const void* source, size_t count);
void* memset(void* destination, int value, size_t count);
int memcmp(const void* left, const void* right, size_t count);

extern long ktest_tls_block[2];
extern volatile unsigned int ktest_join_word;

/// Starts a thread on a stack that all the threads of the program share; its entry sees the stack as if it was called
/// @return The thread id
long ktest_spawn(void (*entry)(long), long arg);

/// Like ktest_spawn, with ktest_join_word cleared when the thread ends
/// @pre The previous joinable thread was joined
long ktest_spawn_joinable(void (*entry)(long), long arg);

void ktest_join(void);

[[noreturn]] void ktest_thread_exit(void);

/// Restorer for the handlers that ktest installs with SA_RESTORER
void ktest_signal_restorer(void);

/// Runs the body in a forked process that leads a new session and a new process group, and returns what the body returned
/// @note A program that was started as a process group leader cannot call setsid
int ktest_in_new_session(int (*body)(void));

/// Makes a directory that is new for this process under /tmp and changes the working directory to it
/// @return The directory path, which stays valid until the program exits
const char* ktest_enter_work_directory(const char* name);
