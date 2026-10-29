#pragma once

#include <vex/address.h>
#include <vex/char_interface.h>
#include <vex/compiler.h>
#include <vex/fb.h>
#include <vex/mutex.h>
#include <vex/types.h>

typedef struct dev_unit_s dev_unit_t;

typedef struct fb_interface_s fb_interface_t;
typedef struct fb_interface_ops_s fb_interface_ops_t;
typedef struct fb_interface_private_s fb_interface_private_t;

struct fb_interface_ops_s {
    /// @pre The rectangles are clipped to the screen
    /// @note nullptr when the memory is scanned out directly, which also leaves VEXFB_FLAG_FLUSH clear
    int (*flush)(const fb_interface_t* restrict interface, const vexfb_rect_t* restrict rects, size_t count);

    /// @pre first + count is within info.palette_size
    /// @note nullptr when info.palette_size is zero
    int (*get_palette)(const fb_interface_t* restrict interface, uint32_t first, uint32_t count, uint32_t* restrict colors);

    /// @pre first + count is within info.palette_size
    /// @note nullptr when info.palette_size is zero
    int (*set_palette)(const fb_interface_t* restrict interface, uint32_t first, uint32_t count, const uint32_t* restrict colors);

    /// Sets the mode and updates info, phys and virt to match it
    /// @pre The fb core serializes it with its other calls, and mode->flags is zero
    /// @retval -EINVAL The driver can't set the mode
    /// @note Also called on a panic to bring back the mode fbcons draws in, so it takes no lock
    /// and allocates nothing; nullptr when the mode is fixed
    int (*set_mode)(fb_interface_t* restrict interface, const vexfb_mode_t* restrict mode);

    /// @note Also called for a boot framebuffer a native driver claimed before it was registered
    void (*release)(const fb_interface_t* restrict interface);
};

struct fb_interface_private_s {
    fb_interface_t* boot_next;
    dev_unit_t* unit;
    unsigned in_flight; // Calls into the fb core that may still touch virt or the driver
    bool removed;
    mutex_t lock;              // Serializes those calls, so info doesn't change under them
    vexfb_mode_t console_mode; // The mode fbcons was attached in
};

struct fb_interface_s {
    char_interface_t char_interface; // Its name, ops and ctxdata belong to the fb core
    fb_interface_private_t priv;
    const fb_interface_ops_t* ops;
    vexfb_info_t info;
    phys_addr_t phys;          // Of the info.size bytes userspace maps
    unsigned cache;            // A VPROT_CACHE value for them
    void* virt;                // The kernel's mapping of them, for read and write
    const vexfb_mode_t* modes; // What the display suggests, for VEXFB_GET_MODES
    uint32_t mode_count;
    void* ctxdata;
};

/// Queues a framebuffer that was set up before the kernel. It is added at dev INIT_LAST, unless a native driver claims its memory first
/// The first framebuffer in the queue gets fbcons
/// @pre The heap stage, before any boot framebuffer is added
void fb_boot_add(fb_interface_t* restrict interface);

/// Takes the boot framebuffers within the memory out of the queue and releases them
/// @pre A native driver's probe, before its fb_interface_add for the same memory
/// @retval -EBUSY The boot framebuffers are already added
/// @note fbcons, if it drew to one of them, moves to the next device added
int fb_boot_claim(phys_addr_t phys, size_t size);

int fb_interface_add(fb_interface_t* restrict interface);

/// @post The fb core makes no more calls into the driver and does not use virt, so the driver can unmap and tear down
/// If fbcons drew to the device, it waits for the next device
/// @note The driver's release still comes, once the last reference is gone
int fb_interface_remove(fb_interface_t* restrict interface);
