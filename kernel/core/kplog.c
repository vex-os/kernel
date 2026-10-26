#include <vex/capability.h>
#include <vex/char_interface.h>
#include <vex/clocksrc.h>
#include <vex/cred.h>
#include <vex/ctype.h>
#include <vex/dev_name.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/initcall.h>
#include <vex/kmalloc.h>
#include <vex/kplog.h>
#include <vex/kprintf.h>
#include <vex/mutex.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/uaccess.h>

#define RECORD_HEAD_SIZE offsetof(kplog_record_t, text)

typedef struct kplog_file_s kplog_file_t;

struct kplog_file_s {
    mutex_t lock; // Threads that share the file share its cursor
    kp_cursor_t cursor;
    uint64_t lost; // Records the ring dropped under the cursor, which the next read reports
    bool writable; // Opened for writing, which takes CAP_SYSLOG
};

static_assert(KP_LINE_MAX == sizeof(((kplog_record_t*)(nullptr))->text));
static_assert(sizeof(kplog_record_t) <= FILE_IO_CHUNK, "A read system call can't take the longest record");

static char_interface_t kplog_device;

static int kplog_open_file(const char_interface_t* restrict interface, unsigned flags, void** restrict file_ctxdata)
{
    thread_t* thread = this_thread();
    bool writable = file_flags_allow_write(flags);

    if(writable && !cred_capable(thread->credentials, CAP_SYSLOG)) {
        return -EPERM;
    }

    kplog_file_t* file = kmalloc(sizeof(kplog_file_t));

    if(file == nullptr) {
        return -ENOMEM;
    }

    mutex_init(&file->lock);

    kprintf_seek_first(&file->cursor);

    file->lost = 0;
    file->writable = writable;

    *file_ctxdata = file;

    return 0;
}

static void kplog_close_file(const char_interface_t* restrict interface, void* restrict file_ctxdata)
{
    kplog_file_t* file = file_ctxdata;

    mutex_deinit(&file->lock);
    kfree(file);
}

static size_t record_footprint(size_t text_size)
{
    size_t size = RECORD_HEAD_SIZE + text_size;

    return (size + FEED_ALIGN - 1U) & ~(size_t)(FEED_ALIGN - 1U);
}

static size_t put_lost(kplog_file_t* restrict file, char* restrict buffer)
{
    feed_lost_t lost = {};

    lost.feed.size = sizeof(lost);
    lost.feed.type = FEED_TYPE_LOST;
    lost.feed.seq = file->cursor.seq;
    lost.feed.time_ns = clock_source_now_ns();
    lost.count = file->lost;

    memcpy(buffer, &lost, sizeof(lost));

    file->lost = 0;

    return sizeof(lost);
}

// The cursor moves past a record only once the record is in the buffer
static ssize_t read_locked(kplog_file_t* restrict file, char* restrict buffer, size_t size)
{
    size_t copied = 0;

    for(;;) {
        if(file->lost) {
            if(size - copied < sizeof(feed_lost_t)) {
                break;
            }

            copied += put_lost(file, &buffer[copied]);
            continue;
        }

        kp_cursor_t next = file->cursor;
        kp_record_t entry;
        kplog_record_t record = {};

        if(!kprintf_read(&next, &entry, record.text)) {
            break;
        }

        // The oldest record may move on again before the cursor gets there, which only adds to the count
        if(entry.seq != file->cursor.seq) {
            file->lost += entry.seq - file->cursor.seq;
            kprintf_seek_first(&file->cursor);
            continue;
        }

        size_t footprint = record_footprint(entry.size);

        if(size - copied < footprint) {
            break;
        }

        record.feed.size = (uint32_t)(footprint);
        record.feed.type = FEED_TYPE_KPLOG;
        record.feed.flags = entry.flags;
        record.feed.seq = entry.seq;
        record.feed.time_ns = entry.time_ns;
        record.subsystem = entry.subsystem;
        record.level = entry.level;
        record.text_size = entry.size;

        memcpy(&buffer[copied], &record, footprint);
        copied += footprint;
        file->cursor = next;
    }

    if(copied) {
        return (ssize_t)(copied);
    }

    // Something is there but doesn't fit, as waiting wouldn't change that
    if(file->lost || kprintf_next_seq() != file->cursor.seq) {
        return -EINVAL;
    }

    return -EAGAIN;
}

static ssize_t kplog_read(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset, void* restrict buffer,
    size_t size, unsigned flags)
{
    kplog_file_t* file = file_ctxdata;
    event_t* appended = &kprintf_appended;
    ssize_t retval;

    mutex_acquire(&file->lock);

    for(;;) {
        retval = read_locked(file, buffer, size);

        if(retval != -EAGAIN || (flags & O_NONBLOCK)) {
            break;
        }

        retval = event_await_interruptible(&appended, 1);

        if(retval < 0) {
            break;
        }
    }

    mutex_release(&file->lock);

    return retval;
}

// Drops the line end of a text from any
// convention: LF, CR, or CRLF of the DOS world
static size_t size_without_line_end(const char* restrict text, size_t size)
{
    if(size && text[size - 1U] == '\n') {
        size -= 1U;
    }

    if(size && text[size - 1U] == '\r') {
        size -= 1U;
    }

    return size;
}

// Takes a leading <N> as syslog writes it, but only its level; text that only looks like one stays text
static size_t parse_prefix(const char* restrict text, size_t size, unsigned* restrict level)
{
    unsigned value = 0;
    size_t i = 1;

    if(size < 3 || text[0] != '<') {
        return 0;
    }

    while(i < size && i <= 4 && isdigit(text[i])) {
        value = value * 10 + (text[i] - '0');
        i += 1;
    }

    if(i == 1 || i == size || text[i] != '>') {
        return 0;
    }

    *level = value & 7;

    return i + 1;
}

// A record is one line, so a longer write fails whole instead of the system call passing the
// rest as another. Userspace can't log as severely as the kernel failing, which takes the levels above KP_ERROR
static ssize_t kplog_write(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset,
    const void* restrict buffer, size_t size, unsigned flags)
{
    const char* text = buffer;
    size_t text_size = size_without_line_end(text, size);

    unsigned level = KP_NOTICE;
    size_t prefix = parse_prefix(text, text_size, &level);

    text += prefix;
    text_size -= prefix;

    if(level < KP_ERROR || text_size > KP_LINE_MAX) {
        return -EINVAL;
    }

    if(memchr(text, '\n', text_size) || memchr(text, '\r', text_size) || memchr(text, '\0', text_size)) {
        return -EINVAL;
    }

    kputs(KP_USER, level, 0, text, text_size);

    return (ssize_t)(size);
}

static int seek_first(kplog_file_t* restrict file)
{
    mutex_acquire(&file->lock);
    kprintf_seek_first(&file->cursor);
    file->lost = 0;
    mutex_release(&file->lock);

    return 0;
}

static int seek_end(kplog_file_t* restrict file)
{
    mutex_acquire(&file->lock);
    kprintf_seek_end(&file->cursor);
    file->lost = 0;
    mutex_release(&file->lock);

    return 0;
}

static int seek_seq(kplog_file_t* restrict file, uintptr_t argument)
{
    uint64_t seq;

    if(user_copyin(&seq, argument, sizeof(seq)) < 0) {
        return -EFAULT;
    }

    mutex_acquire(&file->lock);
    bool valid = kprintf_seek_seq(&file->cursor, seq);

    if(valid) {
        file->lost = 0;
    }

    mutex_release(&file->lock);

    return valid ? 0 : -EINVAL;
}

static int get_info(uintptr_t argument)
{
    kplog_info_t info = {};

    kprintf_get_range(&info.seq_first, &info.seq_next);

    if(user_copyout(argument, &info, sizeof(info)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static int get_console_level(uintptr_t argument)
{
    int32_t level = (int32_t)(loglevel);

    if(user_copyout(argument, &level, sizeof(level)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static int set_console_level(const kplog_file_t* restrict file, uintptr_t argument)
{
    int32_t level;

    if(!file->writable) {
        return -EPERM;
    }

    if(user_copyin(&level, argument, sizeof(level)) < 0) {
        return -EFAULT;
    }

    if(level < KP_EMERG || level > KP_VERBOSE) {
        return -EINVAL;
    }

    loglevel = (unsigned)(level);

    return 0;
}

static int kplog_ioctl(const char_interface_t* restrict interface, void* restrict file_ctxdata, uint32_t request, uintptr_t argument)
{
    kplog_file_t* file = file_ctxdata;

    switch(request) {
        case KPLOG_SEEK_FIRST:
            return seek_first(file);

        case KPLOG_SEEK_END:
            return seek_end(file);

        case KPLOG_SEEK_SEQ:
            return seek_seq(file, argument);

        case KPLOG_GET_INFO:
            return get_info(argument);

        case KPLOG_GET_CONSOLE_LEVEL:
            return get_console_level(argument);

        case KPLOG_SET_CONSOLE_LEVEL:
            return set_console_level(file, argument);

        default:
            return -ENOTTY;
    }
}

static const char_interface_ops_t kplog_ops = {
    .open_file = &kplog_open_file,
    .close_file = &kplog_close_file,
    .read = &kplog_read,
    .write = &kplog_write,
    .ioctl = &kplog_ioctl,
};

static void init_kplog(void)
{
    kplog_device.ops = &kplog_ops;
    kplog_device.device = nullptr;

    int retval = dev_prefix_claim(&dev_prefix_kplog);

    if(retval == 0) {
        retval = char_interface_add_unitless(&kplog_device, &dev_prefix_kplog);
    }

    if(retval < 0) {
        pr_warning("kplog: %s", strerror(retval));
    }
}

initcall_bsp_dev(init_kplog, INIT_MIDDLE);
