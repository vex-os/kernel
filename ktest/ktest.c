#include <vex/dirent.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/kplog.h>
#include <vex/signal.h>

#include "ktest.h"

#define FORMAT_BUFFER_SIZE 1024
#define NUMBER_DIGITS_MAX  24
#define WORK_PATH_SIZE     128
#define DIRENT_BUFFER_SIZE 1024

typedef struct sink_s sink_t;

struct sink_s {
    char* buffer;
    size_t size;
    size_t length;
};

static long kplog_fd = -1;
static unsigned long mismatch_count;

alignas(16) static char thread_stack[KTEST_THREAD_STACK_SIZE];

long ktest_tls_block[2];
volatile unsigned int ktest_join_word;

static void sink_put(sink_t* sink, char character)
{
    if(sink->length + 1 < sink->size) {
        sink->buffer[sink->length] = character;
        sink->length += 1;
    }
}

static void sink_put_string(sink_t* sink, const char* string)
{
    if(string == nullptr) {
        string = "(null)";
    }

    while(*string) {
        sink_put(sink, *string);
        string += 1;
    }
}

static void sink_put_number(sink_t* sink, unsigned long long value, unsigned base, bool negative, int width, bool zero_padded)
{
    char digits[NUMBER_DIGITS_MAX];
    int count = 0;

    do {
        unsigned digit = value % base;
        digits[count] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        count += 1;
        value /= base;
    } while(value);

    int padding = width - count - (negative ? 1 : 0);

    if(negative && zero_padded) {
        sink_put(sink, '-');
    }

    while(padding > 0) {
        sink_put(sink, zero_padded ? '0' : ' ');
        padding -= 1;
    }

    if(negative && !zero_padded) {
        sink_put(sink, '-');
    }

    while(count > 0) {
        count -= 1;
        sink_put(sink, digits[count]);
    }
}

static unsigned long long take_unsigned(va_list* args, int length_class)
{
    if(length_class >= 2) {
        return va_arg(*args, unsigned long long);
    }

    if(length_class == 1) {
        return va_arg(*args, unsigned long);
    }

    return va_arg(*args, unsigned);
}

static long long take_signed(va_list* args, int length_class)
{
    if(length_class >= 2) {
        return va_arg(*args, long long);
    }

    if(length_class == 1) {
        return va_arg(*args, long);
    }

    return va_arg(*args, int);
}

static void put_signed(sink_t* sink, va_list* args, int length_class, int width, bool zero_padded)
{
    long long value = take_signed(args, length_class);

    if(value < 0) {
        sink_put_number(sink, -(unsigned long long)(value), 10, true, width, zero_padded);
    }
    else {
        sink_put_number(sink, value, 10, false, width, zero_padded);
    }
}

static void put_conversion(sink_t* sink, char conversion, va_list* args, int length_class, int width, bool zero_padded)
{
    switch(conversion) {
        case 'd':
            put_signed(sink, args, length_class, width, zero_padded);
            break;
        case 'u':
            sink_put_number(sink, take_unsigned(args, length_class), 10, false, width, zero_padded);
            break;
        case 'x':
            sink_put_number(sink, take_unsigned(args, length_class), 16, false, width, zero_padded);
            break;
        case 's':
            sink_put_string(sink, va_arg(*args, const char*));
            break;
        case 'c':
            sink_put(sink, va_arg(*args, int));
            break;
        default:
            sink_put(sink, '%');
            sink_put(sink, conversion);
            break;
    }
}

static size_t format_into(char* buffer, size_t size, const char* format, va_list args)
{
    sink_t sink = {};
    sink.buffer = buffer;
    sink.size = size;

    va_list copy;
    va_copy(copy, args);

    while(*format) {
        if(*format != '%') {
            sink_put(&sink, *format);
            format += 1;
            continue;
        }

        format += 1;

        bool zero_padded = *format == '0';
        int width = 0;
        int length_class = 0;

        if(zero_padded) {
            format += 1;
        }

        while(*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format += 1;
        }

        while(*format == 'l') {
            length_class += 1;
            format += 1;
        }

        if(*format == '%') {
            sink_put(&sink, '%');
        }
        else if(*format) {
            put_conversion(&sink, *format, &copy, length_class, width, zero_padded);
        }
        else {
            break;
        }

        format += 1;
    }

    va_end(copy);

    buffer[sink.length] = '\0';

    return sink.length;
}

void ktest_report_mismatch(const char* file, int line, const char* expression, long actual, long expected)
{
    ktest_dprintf(KTEST_STDERR, "%s:%d: %s is %ld, expected %ld\n", file, line, expression, actual, expected);
    mismatch_count += 1;
}

int ktest_result(void)
{
    if(mismatch_count) {
        return 1;
    }

    return 0;
}

void ktest_exit(int code)
{
    ktest_syscall1(__k_SYS_EXIT, code);

    for(;;) {
        // The exit does not return
    }
}

void ktest_fail(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    ktest_vdprintf(KTEST_STDERR, format, args);
    va_end(args);

    ktest_puts(KTEST_STDERR, "\n");
    ktest_exit(1);
}

size_t ktest_strlen(const char* string)
{
    size_t length = 0;

    while(string[length]) {
        length += 1;
    }

    return length;
}

bool ktest_string_equal(const char* left, const char* right)
{
    while(*left && *left == *right) {
        left += 1;
        right += 1;
    }

    return *left == *right;
}

bool ktest_bytes_equal(const void* left, const void* right, size_t count)
{
    return memcmp(left, right, count) == 0;
}

bool ktest_has_prefix(const char* string, const char* prefix)
{
    while(*prefix && *string == *prefix) {
        string += 1;
        prefix += 1;
    }

    return *prefix == '\0';
}

long ktest_open(const char* path, unsigned flags, unsigned mode)
{
    return ktest_syscall4(__k_SYS_OPENAT, __k_AT_FDCWD, (long)(path), flags, mode);
}

long ktest_write(long fd, const void* buffer, size_t size)
{
    return ktest_syscall3(__k_SYS_WRITE, fd, (long)(buffer), size);
}

long ktest_read(long fd, void* buffer, size_t size)
{
    return ktest_syscall3(__k_SYS_READ, fd, (long)(buffer), size);
}

bool ktest_file_has_prefix(const char* path, const char* prefix)
{
    char buffer[64];
    size_t length = ktest_strlen(prefix);
    long fd = ktest_open(path, __k_O_RDONLY, 0);

    if(fd < 0 || length > sizeof(buffer)) {
        return false;
    }

    long count = ktest_read(fd, buffer, sizeof(buffer));
    ktest_close(fd);

    return count >= (long)(length) && ktest_bytes_equal(buffer, prefix, length);
}

void ktest_close(long fd)
{
    ktest_syscall1(__k_SYS_CLOSE, fd);
}

void ktest_puts(long fd, const char* string)
{
    size_t remaining = ktest_strlen(string);

    while(remaining) {
        long written = ktest_write(fd, string, remaining);

        if(written <= 0) {
            return;
        }

        string += written;
        remaining -= written;
    }
}

void ktest_vdprintf(long fd, const char* format, va_list args)
{
    char buffer[FORMAT_BUFFER_SIZE];
    format_into(buffer, sizeof(buffer), format, args);
    ktest_puts(fd, buffer);
}

void ktest_dprintf(long fd, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    ktest_vdprintf(fd, format, args);
    va_end(args);
}

size_t ktest_snprintf(char* buffer, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    size_t length = format_into(buffer, size, format, args);
    va_end(args);

    return length;
}

void ktest_printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    ktest_vdprintf(KTEST_STDOUT, format, args);
    va_end(args);
}

void ktest_kplog(unsigned level, const char* format, ...)
{
    char buffer[FORMAT_BUFFER_SIZE];

    if(kplog_fd < 0) {
        kplog_fd = ktest_open("/dev/kplog", __k_O_WRONLY, 0);

        if(kplog_fd < 0) {
            return;
        }
    }

    const size_t prefix = 3;
    buffer[0] = '<';
    buffer[1] = '0' + level;
    buffer[2] = '>';

    va_list args;
    va_start(args, format);
    size_t length = prefix + format_into(buffer + prefix, __k_KPLOG_TEXT_MAX - prefix, format, args);
    va_end(args);

    for(size_t i = prefix; i < length; ++i) {
        if(buffer[i] == '\n' || buffer[i] == '\r') {
            buffer[i] = ' ';
        }
    }

    ktest_write(kplog_fd, buffer, length);
}

void ktest_kplog_close(void)
{
    if(kplog_fd >= 0) {
        ktest_close(kplog_fd);
        kplog_fd = -1;
    }
}

void* memcpy(void* restrict destination, const void* restrict source, size_t count)
{
    void* cursor = destination;
    asm volatile("rep movsb" : "+D"(cursor), "+S"(source), "+c"(count) : : "memory");
    return destination;
}

void* memmove(void* destination, const void* source, size_t count)
{
    if(destination <= source || (const char*)(destination) >= (const char*)(source) + count) {
        return memcpy(destination, source, count);
    }

    char* to = (char*)(destination) + count;
    const char* from = (const char*)(source) + count;

    while(count) {
        to -= 1;
        from -= 1;
        *to = *from;
        count -= 1;
    }

    return destination;
}

void* memset(void* destination, int value, size_t count)
{
    void* cursor = destination;
    asm volatile("rep stosb" : "+D"(cursor), "+c"(count) : "a"(value) : "memory");
    return destination;
}

int memcmp(const void* left, const void* right, size_t count)
{
    const unsigned char* a = left;
    const unsigned char* b = right;

    for(size_t i = 0; i < count; ++i) {
        if(a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }

    return 0;
}

long ktest_spawn(void (*entry)(long), long arg)
{
    long stack_top = (long)(thread_stack + KTEST_THREAD_STACK_SIZE - 8);
    return ktest_syscall5(__k_SYS_THREAD_CREATE, (long)(entry), stack_top, (long)(ktest_tls_block), arg, 0);
}

long ktest_spawn_joinable(void (*entry)(long), long arg)
{
    long stack_top = (long)(thread_stack + KTEST_THREAD_STACK_SIZE - 8);
    ktest_join_word = 1;
    return ktest_syscall5(__k_SYS_THREAD_CREATE, (long)(entry), stack_top, (long)(ktest_tls_block), arg, (long)(&ktest_join_word));
}

void ktest_join(void)
{
    while(ktest_join_word) {
        ktest_syscall4(__k_SYS_FUTEX_WAIT, (long)(&ktest_join_word), 1, 0, 0);
    }
}

void ktest_thread_exit(void)
{
    ktest_syscall1(__k_SYS_THREAD_EXIT, 0);

    for(;;) {
        // The exit does not return
    }
}

[[gnu::naked]] void ktest_signal_restorer(void)
{
    asm volatile("movq %0, %%rax\n\tsyscall" ::"i"(__k_SYS_SIGRETURN));
}

int ktest_in_new_session(int (*body)(void))
{
    long child = ktest_syscall1(__k_SYS_FORK, 0);

    if(child < 0) {
        ktest_fail("fork failed: %ld", child);
    }

    if(child == 0) {
        long session = ktest_syscall1(__k_SYS_SETSID, 0);

        if(session < 0) {
            ktest_fail("setsid failed: %ld", session);
        }

        ktest_exit(body());
    }

    int status = -1;
    long waited = ktest_syscall3(__k_SYS_WAITPID, child, (long)(&status), 0);

    if(waited != child) {
        ktest_fail("waitpid failed: %ld", waited);
    }

    if(status) {
        // The body printed what went wrong
        return 1;
    }

    return 0;
}

static long unlink_path(const char* path, long flags)
{
    return ktest_syscall3(__k_SYS_UNLINKAT, __k_AT_FDCWD, (long)(path), flags);
}

static bool find_entry(const char* directory, char* name, size_t size)
{
    long fd = ktest_open(directory, __k_O_RDONLY, 0);

    if(fd < 0) {
        return false;
    }

    alignas(8) char records[DIRENT_BUFFER_SIZE];
    long filled = ktest_syscall3(__k_SYS_GETDENTS, fd, (long)(records), sizeof(records));
    bool found = false;

    long position = 0;

    while(!found && position < filled) {
        const struct __k_dirent* entry = (const struct __k_dirent*)(records + position);
        const char* entry_name = (const char*)(entry->d_name);

        if(!ktest_string_equal(entry_name, ".") && !ktest_string_equal(entry_name, "..")) {
            ktest_snprintf(name, size, "%s", entry_name);
            found = true;
        }

        position += entry->d_reclen;
    }

    ktest_close(fd);
    return found;
}

static bool remove_tree(const char* path)
{
    long retval = unlink_path(path, 0);

    if(retval != -__k_EISDIR) {
        return retval == 0;
    }

    char child[WORK_PATH_SIZE];
    char name[WORK_PATH_SIZE];

    while(find_entry(path, name, sizeof(name))) {
        ktest_snprintf(child, sizeof(child), "%s/%s", path, name);

        if(!remove_tree(child)) {
            return false;
        }
    }

    return unlink_path(path, __k_AT_REMOVEDIR) == 0;
}

const char* ktest_enter_work_directory(const char* name)
{
    static char path[64];

    long pid = ktest_syscall1(__k_SYS_GETPID, 0);
    ktest_snprintf(path, sizeof(path), "/tmp/%s-%ld", name, pid);

    long retval = ktest_syscall3(__k_SYS_MKDIRAT, __k_AT_FDCWD, (long)(path), 0755);

    if(retval == -__k_EEXIST) {
        // The work directory lives on the disk, and the pid repeats on every boot
        bool removed = remove_tree(path);
        ktest_printf("ktest: stale %s %s\n", path, removed ? "removed" : "could not be removed");
        retval = ktest_syscall3(__k_SYS_MKDIRAT, __k_AT_FDCWD, (long)(path), 0755);
    }

    if(retval < 0) {
        ktest_fail("cannot make %s: %ld", path, retval);
    }

    retval = ktest_syscall1(__k_SYS_CHDIR, (long)(path));

    if(retval < 0) {
        ktest_fail("cannot enter %s: %ld", path, retval);
    }

    return path;
}
