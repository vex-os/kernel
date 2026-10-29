#pragma once

#include <vex/atomic.h>
#include <vex/compiler.h>
#include <vex/dev_name.h>
#include <vex/errno.h>
#include <vex/mutex.h>
#include <vex/types.h>

typedef struct console_s console_t;
typedef struct device_s device_t;

typedef struct char_interface_s char_interface_t;
typedef struct char_interface_ops_s char_interface_ops_t;
typedef struct char_interface_private_s char_interface_private_t;

struct char_interface_ops_s {
    /// @pre This is the first open, and the device's console is released
    /// @note Set the hardware up here, not at probe: the console owns it until now
    /// If open fails, nobody owns the device
    int (*open)(const char_interface_t* restrict interface, unsigned flags);

    /// @pre This is the last close, and the device wasn't removed while open
    void (*close)(const char_interface_t* restrict interface);

    /// Sets up the state of one open file, which the
    /// other calls on that file get as file_ctxdata
    /// @pre On the first open, open has run before it
    int (*open_file)(const char_interface_t* restrict interface, unsigned flags, void** restrict file_ctxdata);

    /// @note Runs even on a device removed while open, as the file's
    /// state is freed here; on the last close, close runs after it
    void (*close_file)(const char_interface_t* restrict interface, void* restrict file_ctxdata);

    ssize_t (*read)(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset, void* restrict buffer,
        size_t size, unsigned flags);
    ssize_t (*write)(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset, const void* restrict buffer,
        size_t size, unsigned flags);

    /// Moves the file's offset by the device's own rules
    /// @param position The file's offset now
    /// @return The new offset
    /// @note nullptr makes the device a stream that can't seek
    off_t (*seek)(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t position, off_t offset, int origin);

    int (*ioctl)(const char_interface_t* restrict interface, void* restrict file_ctxdata, uint32_t request, uintptr_t argument);

    /// Checks a mapping of the device's memory before it is made
    /// @pre offset and length are page aligned
    /// @retval -ENXIO The range runs past the end of the device's memory
    /// @note nullptr, as is resolve_page, for a device without memory
    int (*mmap)(const char_interface_t* restrict interface, off_t offset, size_t length);

    /// Gives the physical address and memory type (a VPROT_CACHE_* value) of the page at offset in the device's memory
    /// @pre offset is page aligned; the caller holds a spinlock, so this can't sleep
    /// @retval -ENXIO offset is past the end of the device's memory
    int (*resolve_page)(const char_interface_t* restrict interface, off_t offset, phys_addr_t* restrict phys, unsigned* restrict cache);

    void (*release)(const char_interface_t* restrict interface);
};

struct char_interface_private_s {
    char_interface_t* next;
    dev_name_t name;
    dev_t dev;
    unsigned refcount;
    bool removed;
    mutex_t open_lock; // Serializes open_count with the driver's open and close
    unsigned open_count;
    bool exclusive; // Opened with O_EXCL; other opens fail with -EBUSY until the last close
};

struct char_interface_s {
    char_interface_private_t priv;
    const char_interface_ops_t* ops;
    char name[64];
    console_t* console; // Writes to the device until its first open; may be nullptr
    device_t* device;   // The node of the hardware, set before the add; nullptr for a device that isn't hardware
    void* ctxdata;
};

/// @param index The unit number in the name, such as the port of the card; DEV_NAME_NONE for a device that is its unit's only one
int char_interface_add(char_interface_t* restrict interface, dev_unit_t* restrict unit, unsigned index);

/// Names the device with the prefix alone, for a device the system has only one of
/// @pre The caller holds the prefix through dev_prefix_claim, until the device is removed
int char_interface_add_unitless(char_interface_t* restrict interface, dev_prefix_t* restrict prefix);

/// Drops the registry's reference; holders of others keep the structure
/// @pre The driver fails its own calls with -ENODEV and has woken the calls that sleep
/// @post Calls through the chr layer fail with -ENODEV, and a mapping of the device's memory gets SIGBUS on the next access
/// The driver gets no more calls, except release
/// @note A call that passed the check of the chr layer can still reach the driver
int char_interface_remove(char_interface_t* restrict interface);

/// Unmaps the device's memory from every address space, so the next access faults on it again
/// @pre Interrupts are enabled
/// @note The event subscribers can sleep
/// @note For a driver whose memory changed layout, such as a framebuffer after a mode change
void char_interface_revoke_mappings(const char_interface_t* restrict interface);

void char_interface_incref(const char_interface_t* restrict interface);
void char_interface_decref(const char_interface_t* restrict interface);

/// @param file_ctxdata Gets what the driver's open_file made, if it has one
int char_interface_open(const char_interface_t* restrict interface, unsigned flags, void** restrict file_ctxdata);

/// Makes further opens fail with -EBUSY until the last close, as an O_EXCL open does
void char_interface_set_exclusive(const char_interface_t* restrict interface, bool exclusive);

/// @return Whether an O_EXCL open or TIOCEXCL keeps others from opening the device
bool char_interface_is_exclusive(const char_interface_t* restrict interface) __nodiscard;

/// Makes further opens fail with -EBUSY, unless the device is open
/// @retval -EBUSY The device is open
/// @note For a device about to be removed, so that no open reaches its driver meanwhile
int char_interface_refuse_opens(const char_interface_t* restrict interface);
void char_interface_close(const char_interface_t* restrict interface, void* restrict file_ctxdata);

const char_interface_t* char_interface_find(const char* restrict name) __nodiscard;

/// @pre The caller holds char_event_lock, so the device list does not change
void char_interface_for_each(void (*callback)(const char_interface_t* restrict interface, void* restrict arg), void* restrict arg);

static __always_inline ssize_t char_interface_read(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset,
    void* restrict buffer, size_t size, unsigned flags)
{
    if(atomic_load(&interface->priv.removed, ATOMIC_ACQUIRE))
        return -ENODEV;
    if(interface->ops->read == nullptr)
        return -ENOSYS;
    return interface->ops->read(interface, file_ctxdata, offset, buffer, size, flags);
}

static __always_inline ssize_t char_interface_write(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset,
    const void* restrict buffer, size_t size, unsigned flags)
{
    if(atomic_load(&interface->priv.removed, ATOMIC_ACQUIRE))
        return -ENODEV;
    if(interface->ops->write == nullptr)
        return -ENOSYS;
    return interface->ops->write(interface, file_ctxdata, offset, buffer, size, flags);
}

/// @retval -ESPIPE The device is a stream
static __always_inline off_t char_interface_seek(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t position,
    off_t offset, int origin)
{
    if(atomic_load(&interface->priv.removed, ATOMIC_ACQUIRE))
        return -ENODEV;
    if(interface->ops->seek == nullptr)
        return -ESPIPE;
    return interface->ops->seek(interface, file_ctxdata, position, offset, origin);
}

static __always_inline int char_interface_ioctl(const char_interface_t* restrict interface, void* restrict file_ctxdata, uint32_t request,
    uintptr_t argument)
{
    if(atomic_load(&interface->priv.removed, ATOMIC_ACQUIRE))
        return -ENODEV;
    if(interface->ops->ioctl == nullptr)
        return -ENOTTY;
    return interface->ops->ioctl(interface, file_ctxdata, request, argument);
}

static __always_inline int char_interface_mmap(const char_interface_t* restrict interface, off_t offset, size_t length)
{
    if(atomic_load(&interface->priv.removed, ATOMIC_ACQUIRE))
        return -ENODEV;
    if(interface->ops->mmap == nullptr)
        return -ENODEV;
    return interface->ops->mmap(interface, offset, length);
}

static __always_inline int char_interface_resolve_page(const char_interface_t* restrict interface, off_t offset, phys_addr_t* restrict phys,
    unsigned* restrict cache)
{
    if(atomic_load(&interface->priv.removed, ATOMIC_ACQUIRE))
        return -ENODEV;
    if(interface->ops->resolve_page == nullptr)
        return -ENODEV;
    return interface->ops->resolve_page(interface, offset, phys, cache);
}
