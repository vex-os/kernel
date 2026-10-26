#include <vex/atomic.h>
#include <vex/block_event.h>
#include <vex/block_interface.h>
#include <vex/capability.h>
#include <vex/char_event.h>
#include <vex/char_interface.h>
#include <vex/clocksrc.h>
#include <vex/cred.h>
#include <vex/dev_name.h>
#include <vex/driver.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/hwtree.h>
#include <vex/initcall.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/mutex.h>
#include <vex/page.h>
#include <vex/slab.h>
#include <vex/sprintf.h>
#include <vex/stdlib.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/uaccess.h>

#include "device_internal.h"

#define BUILDER_START_SIZE PAGE_SIZE // Grows by doubling, so a small tree takes one or two steps
#define RING_RECORDS       128       // Events kept for readers that fall behind; past that, a reader gets a new snapshot
#define RECORD_HEAD_SIZE   offsetof(hwtree_record_t, fields)

typedef struct builder_s builder_t;
typedef struct snapshot_s snapshot_t;
typedef struct hwtree_file_s hwtree_file_t;

struct builder_s {
    char* text;
    size_t length;
    size_t capacity;
    bool failed; // Out of memory
};

// The tree as it was at seq, shared by the readers that open the file before the next event
struct snapshot_s {
    unsigned refcount;
    uint64_t seq; // Of the first event after the snapshot
    char* records;
    size_t length;
};

struct hwtree_file_s {
    mutex_t lock;         // Threads that share the file share its position
    snapshot_t* snapshot; // Not read to the end yet; nullptr after that
    size_t snapshot_offset;
    uint64_t seq;  // Of the next event to read
    uint64_t lost; // Events a reader fell behind on, which the next read reports before the new snapshot
    bool writable; // Opened for writing, which takes CAP_DEVICE
};

static_assert(sizeof(hwtree_record_t) <= FILE_IO_CHUNK, "A read system call can't take the longest record");
static_assert(sizeof(hwtree_file_t) <= SLAB_MAX_SIZE);

static_assert(HWTREE_STATE_PRESENT == DEVICE_PRESENT);
static_assert(HWTREE_STATE_PROBING == DEVICE_PROBING);
static_assert(HWTREE_STATE_WAITING == DEVICE_WAITING);
static_assert(HWTREE_STATE_BOUND == DEVICE_BOUND);
static_assert(HWTREE_STATE_DETACHING == DEVICE_DETACHING);
static_assert(HWTREE_STATE_GONE == DEVICE_GONE);

static mutex_t ring_lock = MUTEX_INIT;
static hwtree_record_t ring[RING_RECORDS];
static uint64_t ring_next_seq = 0;         // Under ring_lock
static event_t ring_appended = {};         // All zero is an initialized event, so the nodes added before the dev stage can post
static mutex_t snapshot_lock = MUTEX_INIT; // Over cached_snapshot, held while a new snapshot is built
static snapshot_t* cached_snapshot = nullptr;
static char_interface_t hwtree_device;

static size_t record_footprint(const hwtree_record_t* restrict record)
{
    size_t size = RECORD_HEAD_SIZE + record->fields_size;
    return (size + FEED_ALIGN - 1U) & ~(size_t)(FEED_ALIGN - 1U);
}

static void begin_record(hwtree_record_t* restrict record, uint16_t type, const device_t* restrict device)
{
    record->feed.size = 0;
    record->feed.type = type;
    record->feed.flags = 0;
    record->feed.seq = 0;
    record->feed.time_ns = clock_source_now_ns();
    record->id = 0;
    record->parent = 0;
    record->state = 0;
    record->fields_size = 0;

    if(device) {
        device_t* parent = device->parent;

        record->id = device->id;
        record->state = device->priv.state;

        if(parent) {
            record->parent = parent->id;
        }
    }
}

// The padding after the fields is zeroed, so no kernel memory reaches userspace through it
static void end_record(hwtree_record_t* restrict record)
{
    size_t footprint = record_footprint(record);

    memset(&record->fields[record->fields_size], 0, footprint - RECORD_HEAD_SIZE - record->fields_size);
    record->feed.size = (uint32_t)(footprint);
}

static void post(hwtree_record_t* restrict record)
{
    end_record(record);

    mutex_acquire(&ring_lock);

    record->feed.seq = ring_next_seq;
    memcpy(&ring[ring_next_seq % RING_RECORDS], record, record->feed.size);
    ring_next_seq += 1;

    mutex_release(&ring_lock);

    event_trigger(&ring_appended, true);
}

static void describe_node(device_t* restrict device, hwtree_record_t* restrict record)
{
    char text[DEVICE_PATH_MAX];

    begin_record(record, FEED_TYPE_HWTREE_ADD, device);
    hwtree_put(record, "name", "%s", device->name);

    device_get_path(device, text, sizeof(text));
    hwtree_put(record, "path", "%s", text);

    if(device->driver) {
        hwtree_put(record, "driver", "%s", device->driver->name);
    }

    if(device->fwnode.ops && 0 == fwnode_get_id(&device->fwnode, text, sizeof(text))) {
        hwtree_put(record, "fwid", "%s", text);
    }

    device_t* bus = device_bus_of(device);

    if(bus && bus->bus_ops->describe_child) {
        bus->bus_ops->describe_child(bus, device, record);
    }
}

static void describe_interface(const device_t* restrict device, const char* restrict kind, const char* restrict name,
    hwtree_record_t* restrict record)
{
    begin_record(record, FEED_TYPE_HWTREE_INTERFACE, device);
    hwtree_put(record, "kind", "%s", kind);
    hwtree_put(record, "name", "%s", name);
}

static void append_record(builder_t* restrict builder, hwtree_record_t* restrict record)
{
    end_record(record);

    size_t needed = builder->length + record->feed.size;

    if(builder->failed) {
        return;
    }

    if(needed > builder->capacity) {
        size_t capacity = builder->capacity ? builder->capacity * 2 : BUILDER_START_SIZE;

        while(capacity < needed) {
            capacity *= 2;
        }

        char* text = krealloc(builder->text, capacity);

        if(text == nullptr) {
            builder->failed = true;
            return;
        }

        builder->text = text;
        builder->capacity = capacity;
    }

    memcpy(&builder->text[builder->length], record, record->feed.size);
    builder->length = needed;
}

static void append_marker(builder_t* restrict builder, uint16_t type)
{
    hwtree_record_t record;
    begin_record(&record, type, nullptr);
    append_record(builder, &record);
}

// Parents come before their children, so that a reader knows the parent of each node it gets
static void append_children(builder_t* restrict builder, device_t* parent)
{
    for(device_t* child = device_next_child(parent, nullptr); child;) {
        if(child->priv.state != DEVICE_GONE) {
            hwtree_record_t record;
            describe_node(child, &record);
            append_record(builder, &record);
            append_children(builder, child);
        }

        device_t* next = device_next_child(parent, child);
        device_decref(child);
        child = next;
    }
}

static void append_char_interface(const char_interface_t* restrict interface, void* restrict arg)
{
    if(interface->device) {
        hwtree_record_t record;
        describe_interface(interface->device, "char", interface->name, &record);
        append_record(arg, &record);
    }
}

static void append_block_interface(const block_interface_t* restrict interface, void* restrict arg)
{
    if(interface->device) {
        hwtree_record_t record;
        describe_interface(interface->device, "block", interface->name, &record);
        append_record(arg, &record);
    }
}

static void snapshot_decref(snapshot_t* restrict snapshot)
{
    if(1 == atomic_fetch_sub(&snapshot->refcount, 1, ATOMIC_ACQ_REL)) {
        kfree(snapshot->records);
        kfree(snapshot);
    }
}

// The number of the next event is taken before the walk. Thus a change during the walk is in the
// snapshot, in the events after it, or in both, and never lost. This is why the events are idempotent
static snapshot_t* build_snapshot(uint64_t seq)
{
    builder_t builder = {};

    append_marker(&builder, FEED_TYPE_HWTREE_SNAPSHOT_BEGIN);

    device_t* root = device_get_root();

    if(root) {
        append_children(&builder, root);
        device_decref(root);
    }

    char_event_lock();
    char_interface_for_each(&append_char_interface, &builder);
    char_event_unlock();

    block_event_lock();
    block_interface_for_each(&append_block_interface, &builder);
    block_event_unlock();

    append_marker(&builder, FEED_TYPE_HWTREE_SNAPSHOT_END);

    snapshot_t* snapshot = nullptr;

    if(root && !builder.failed) {
        snapshot = kmalloc(sizeof(snapshot_t));
    }

    if(snapshot == nullptr) {
        kfree(builder.text);
        return nullptr;
    }

    // Each record of the snapshot comes before the event numbered seq
    for(size_t offset = 0; offset < builder.length;) {
        hwtree_record_t* record = (hwtree_record_t*)(&builder.text[offset]);
        record->feed.seq = seq;
        offset += record->feed.size;
    }

    snapshot->refcount = 1;
    snapshot->seq = seq;
    snapshot->records = builder.text;
    snapshot->length = builder.length;

    return snapshot;
}

// The cached snapshot is current while no event came after it. The cache keeps
// a stale one until the next open, as posting an event can't take snapshot_lock
static snapshot_t* get_snapshot(void)
{
    mutex_acquire(&snapshot_lock);

    mutex_acquire(&ring_lock);
    uint64_t seq = ring_next_seq;
    mutex_release(&ring_lock);

    snapshot_t* snapshot = cached_snapshot;

    if(snapshot == nullptr || snapshot->seq != seq) {
        snapshot = build_snapshot(seq);

        if(snapshot == nullptr) {
            mutex_release(&snapshot_lock);
            return nullptr;
        }

        if(cached_snapshot) {
            snapshot_decref(cached_snapshot);
        }

        cached_snapshot = snapshot;
    }

    atomic_fetch_add(&snapshot->refcount, 1, ATOMIC_RELAXED);

    mutex_release(&snapshot_lock);

    return snapshot;
}

static void drop_snapshot(hwtree_file_t* restrict file)
{
    if(file->snapshot) {
        snapshot_decref(file->snapshot);
        file->snapshot = nullptr;
    }
}

static int take_snapshot(hwtree_file_t* restrict file)
{
    snapshot_t* snapshot = get_snapshot();

    if(snapshot == nullptr) {
        return -ENOMEM;
    }

    drop_snapshot(file);

    file->snapshot = snapshot;
    file->snapshot_offset = 0;
    file->seq = snapshot->seq;

    return 0;
}

static size_t put_lost(hwtree_file_t* restrict file, char* restrict buffer)
{
    feed_lost_t lost = {};

    lost.feed.size = sizeof(lost);
    lost.feed.type = FEED_TYPE_LOST;
    lost.feed.seq = file->seq;
    lost.feed.time_ns = clock_source_now_ns();
    lost.count = file->lost;

    memcpy(buffer, &lost, sizeof(lost));
    file->lost = 0;

    return sizeof(lost);
}

// The next record of the snapshot, if it fits in the space left
static size_t take_from_snapshot(hwtree_file_t* restrict file, char* restrict buffer, size_t space)
{
    snapshot_t* snapshot = file->snapshot;
    const hwtree_record_t* record = (const hwtree_record_t*)(&snapshot->records[file->snapshot_offset]);
    size_t size = record->feed.size;

    if(size > space) {
        return 0;
    }

    memcpy(buffer, record, size);
    file->snapshot_offset += size;

    if(file->snapshot_offset == snapshot->length) {
        drop_snapshot(file);
    }

    return size;
}

// The next event, if there is one and it fits in the space left. A reader that
// fell behind the ring gets the count of what it missed, then a new snapshot
static ssize_t take_event(hwtree_file_t* restrict file, char* restrict buffer, size_t space)
{
    size_t size = 0;

    mutex_acquire(&ring_lock);

    uint64_t next_seq = ring_next_seq;
    bool behind = next_seq - file->seq > RING_RECORDS;

    if(!behind && file->seq != next_seq) {
        const hwtree_record_t* record = &ring[file->seq % RING_RECORDS];

        if(record->feed.size <= space) {
            size = record->feed.size;
            memcpy(buffer, record, size);
            file->seq += 1;
        }
    }

    mutex_release(&ring_lock);

    if(behind) {
        uint64_t missed = next_seq - file->seq;
        int retval = take_snapshot(file);

        if(retval < 0) {
            return retval;
        }

        file->lost += missed;
    }

    return (ssize_t)(size);
}

static bool has_event(const hwtree_file_t* restrict file)
{
    mutex_acquire(&ring_lock);
    bool pending = file->seq != ring_next_seq;
    mutex_release(&ring_lock);

    return pending;
}

static ssize_t read_locked(hwtree_file_t* restrict file, char* restrict buffer, size_t size)
{
    size_t copied = 0;

    for(;;) {
        size_t space = size - copied;

        if(file->lost) {
            if(space < sizeof(feed_lost_t)) {
                break;
            }

            copied += put_lost(file, &buffer[copied]);
            continue;
        }

        if(file->snapshot) {
            size_t taken = take_from_snapshot(file, &buffer[copied], space);

            if(taken == 0) {
                break;
            }

            copied += taken;
            continue;
        }

        ssize_t taken = take_event(file, &buffer[copied], space);

        if(taken < 0) {
            if(copied == 0) {
                return taken;
            }

            break;
        }

        // A reader that fell behind has the loss to report and a new snapshot to read
        if(file->lost) {
            continue;
        }

        if(taken == 0) {
            break;
        }

        copied += (size_t)(taken);
    }

    if(copied) {
        return (ssize_t)(copied);
    }

    // Something is there but doesn't fit, as waiting wouldn't change that
    if(file->lost || file->snapshot || has_event(file)) {
        return -EINVAL;
    }

    return -EAGAIN;
}

static device_t* find_by_id(device_t* parent, uint64_t id)
{
    for(device_t* child = device_next_child(parent, nullptr); child;) {
        if(child->id == id) {
            return child;
        }

        device_t* found = find_by_id(child, id);

        if(found) {
            device_decref(child);
            return found;
        }

        device_t* next = device_next_child(parent, child);
        device_decref(child);
        child = next;
    }

    return nullptr;
}

static int run_command(const hwtree_file_t* restrict file, void (*request)(device_t* restrict device), uintptr_t argument)
{
    uint64_t id;

    if(!file->writable) {
        return -EPERM;
    }

    if(user_copyin(&id, argument, sizeof(id)) < 0) {
        return -EFAULT;
    }

    device_t* root = device_get_root();

    if(root == nullptr) {
        return -ENOENT;
    }

    device_t* device = find_by_id(root, id);
    device_decref(root);

    if(device == nullptr) {
        return -ENOENT;
    }

    request(device);
    device_decref(device);

    return 0;
}

static int resnapshot(hwtree_file_t* restrict file)
{
    mutex_acquire(&file->lock);

    int retval = take_snapshot(file);

    if(retval == 0) {
        file->lost = 0;
    }

    mutex_release(&file->lock);

    return retval;
}

static int hwtree_open_file(const char_interface_t* restrict interface, unsigned flags, void** restrict file_ctxdata)
{
    thread_t* thread = this_thread();
    bool writable = file_flags_allow_write(flags);

    if(writable && !cred_capable(thread->credentials, CAP_DEVICE)) {
        return -EPERM;
    }

    hwtree_file_t* file = slab_alloc(sizeof(hwtree_file_t));

    if(file == nullptr) {
        return -ENOMEM;
    }

    mutex_init(&file->lock);
    file->snapshot = nullptr;
    file->snapshot_offset = 0;
    file->seq = 0;
    file->lost = 0;
    file->writable = writable;

    if(file_flags_allow_read(flags) && take_snapshot(file) < 0) {
        mutex_deinit(&file->lock);
        slab_free(file);
        return -ENOMEM;
    }

    *file_ctxdata = file;

    return 0;
}

static void hwtree_close_file(const char_interface_t* restrict interface, void* restrict file_ctxdata)
{
    hwtree_file_t* file = file_ctxdata;

    drop_snapshot(file);
    mutex_deinit(&file->lock);
    slab_free(file);
}

static ssize_t hwtree_read(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset, void* restrict buffer,
    size_t size, unsigned flags)
{
    hwtree_file_t* file = file_ctxdata;
    event_t* appended = &ring_appended;
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

static int hwtree_ioctl(const char_interface_t* restrict interface, void* restrict file_ctxdata, uint32_t request, uintptr_t argument)
{
    hwtree_file_t* file = file_ctxdata;

    switch(request) {
        case HWTREE_RESNAPSHOT:
            return resnapshot(file);

        case HWTREE_RESCAN:
            return run_command(file, &device_rescan, argument);

        case HWTREE_UNBIND:
            return run_command(file, &device_unbind, argument);

        case HWTREE_EJECT:
            return run_command(file, &device_eject, argument);

        default:
            return -ENOTTY;
    }
}

static const char_interface_ops_t hwtree_ops = {
    .open_file = &hwtree_open_file,
    .close_file = &hwtree_close_file,
    .read = &hwtree_read,
    .ioctl = &hwtree_ioctl,
};

void hwtree_put(hwtree_record_t* restrict record, const char* restrict key, const char* restrict fmt, ...)
{
    char* field = &record->fields[record->fields_size];
    size_t space = HWTREE_FIELDS_MAX - record->fields_size;
    int length = snprintf(field, space, "%s=", key);

    if(length >= 0 && (size_t)(length) < space) {
        va_list ap;

        va_start(ap, fmt);
        int value_length = vsnprintf(&field[length], space - (size_t)(length), fmt, ap);
        va_end(ap);

        if(value_length >= 0) {
            length += value_length;
        }
    }

    // The NUL that ends the field takes the last byte of the space
    if(length < 0 || (size_t)(length) >= space) {
        record->feed.flags |= HWTREE_FLAG_TRUNCATED;
        return;
    }

    record->fields_size += (uint32_t)(length) + 1U;
}

void hwtree_interface_added(const device_t* restrict device, const char* restrict kind, const char* restrict name)
{
    hwtree_record_t record;
    describe_interface(device, kind, name, &record);
    post(&record);
}

void hwtree_interface_removed(const device_t* restrict device, const char* restrict name)
{
    hwtree_record_t record;
    begin_record(&record, FEED_TYPE_HWTREE_INTERFACE_REMOVE, device);
    hwtree_put(&record, "name", "%s", name);
    post(&record);
}

void hwtree_node_added(device_t* restrict device)
{
    hwtree_record_t record;
    describe_node(device, &record);
    post(&record);
}

void hwtree_node_removed(const device_t* restrict device)
{
    hwtree_record_t record;
    begin_record(&record, FEED_TYPE_HWTREE_REMOVE, device);
    post(&record);
}

void hwtree_node_bound(const device_t* restrict device)
{
    hwtree_record_t record;
    begin_record(&record, FEED_TYPE_HWTREE_BIND, device);
    hwtree_put(&record, "driver", "%s", device->driver->name);
    post(&record);
}

void hwtree_node_unbound(const device_t* restrict device, const driver_t* restrict driver)
{
    hwtree_record_t record;
    begin_record(&record, FEED_TYPE_HWTREE_UNBIND, device);
    hwtree_put(&record, "driver", "%s", driver->name);
    post(&record);
}

void hwtree_node_waiting(const device_t* restrict device, const driver_t* restrict driver)
{
    hwtree_record_t record;
    begin_record(&record, FEED_TYPE_HWTREE_WAITING, device);
    hwtree_put(&record, "driver", "%s", driver->name);

    if(device->priv.waits_for) {
        hwtree_put(&record, "for", "%llu", (unsigned long long)(device->priv.waits_for->id));
    }
    else {
        char name[DEVICE_NAME_MAX];

        if(0 == fwnode_get_name(&device->priv.waits_for_fwnode, name, sizeof(name))) {
            hwtree_put(&record, "for_name", "%s", name);
        }
    }

    post(&record);
}

void hwtree_attach_failed(const device_t* restrict device, const driver_t* restrict driver, int error)
{
    hwtree_record_t record;
    begin_record(&record, FEED_TYPE_HWTREE_ATTACH_FAILED, device);
    hwtree_put(&record, "driver", "%s", driver->name);
    hwtree_put(&record, "error", "%d", -error);
    post(&record);
}

static void init_hwtree(void)
{
    hwtree_device.ops = &hwtree_ops;
    hwtree_device.device = nullptr;

    int retval = dev_prefix_claim(&dev_prefix_hwtree);

    if(retval == 0) {
        retval = char_interface_add_unitless(&hwtree_device, &dev_prefix_hwtree);
    }

    if(retval < 0) {
        pr_warning("hwtree: %s", strerror(retval));
    }
}

initcall_bsp_dev(init_hwtree, INIT_MIDDLE);
