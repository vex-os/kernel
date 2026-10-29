#include <vex/assert.h>
#include <vex/char_interface.h>
#include <vex/console.h>
#include <vex/dev_name.h>
#include <vex/errno.h>
#include <vex/fb_interface.h>
#include <vex/fbcons.h>
#include <vex/file.h>
#include <vex/hart.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/mutex.h>
#include <vex/page.h>
#include <vex/string.h>
#include <vex/uaccess.h>

#define RECT_BATCH  16
#define COLOR_BATCH 64

// Boot framebuffers are queued and claimed by initcalls on the
// BSP, one at a time, so the queue needs no lock
static fb_interface_t* boot_head = nullptr;
static bool boot_added = false;
static bool console_tried = false;

static mutex_t console_mutex = MUTEX_INIT;
static bool console_pending = false; // Under console_mutex; fbcons lost its device and waits for the next one

static const fb_interface_t* fb_of(const char_interface_t* restrict interface)
{
    return interface->ctxdata;
}

// Runs on a panic, so it takes no lock
static void restore_console_mode(void* restrict ctxdata)
{
    fb_interface_t* interface = ctxdata;
    const vexfb_mode_t* mode = &interface->priv.console_mode;

    if(interface->ops->set_mode == nullptr) {
        return;
    }

    if(interface->info.width != mode->width || interface->info.height != mode->height || interface->info.bpp != mode->bpp) {
        interface->ops->set_mode(interface, mode);
    }
}

// fbcons starts on the first framebuffer, which becomes fbd0, so console= names it so
static console_t* attach_console(fb_interface_t* restrict interface)
{
#ifdef CONFIG_FBCONS
    char options[1];

    if(console_lookup("fbd0", options, sizeof(options)) == CONSOLE_UNLISTED) {
        return nullptr;
    }

    interface->priv.console_mode.width = interface->info.width;
    interface->priv.console_mode.height = interface->info.height;
    interface->priv.console_mode.bpp = interface->info.bpp;
    interface->priv.console_mode.flags = 0;

    return fbcons_attach(&interface->info, interface->virt, &restore_console_mode, interface);
#else
    return nullptr;
#endif
}

static void detach_console(void)
{
#ifdef CONFIG_FBCONS
    fbcons_detach();
#endif
}

// The device's memory is about to go
// so fbcons moves to the next device added
static void lose_console(fb_interface_t* restrict interface)
{
    mutex_acquire(&console_mutex);

    if(interface->char_interface.console) {
        detach_console();
        interface->char_interface.console = nullptr;
        console_pending = true;
    }

    mutex_release(&console_mutex);
}

static bool overlaps(const fb_interface_t* restrict interface, phys_addr_t phys, size_t size)
{
    return address_less(interface->phys, address_add(phys, size)) && address_less(phys, address_add(interface->phys, interface->info.size));
}

static uint64_t mapped_size(const fb_interface_t* restrict fb)
{
    return page_align_up(fb->info.size);
}

static bool clip_rect(const fb_interface_t* restrict fb, vexfb_rect_t* restrict rect)
{
    bool outside = rect->x >= fb->info.width || rect->y >= fb->info.height;
    bool empty = rect->width == 0 || rect->height == 0;

    if(outside || empty) {
        return false;
    }

    if(rect->width > fb->info.width - rect->x) {
        rect->width = fb->info.width - rect->x;
    }

    if(rect->height > fb->info.height - rect->y) {
        rect->height = fb->info.height - rect->y;
    }

    return true;
}

static int flush_rects(const fb_interface_t* restrict fb, uintptr_t user_rects, uint32_t count)
{
    vexfb_rect_t batch[RECT_BATCH];

    while(count > 0) {
        uint32_t chunk = count < RECT_BATCH ? count : RECT_BATCH;

        if(user_copyin(batch, user_rects, chunk * sizeof(vexfb_rect_t)) < 0) {
            return -EFAULT;
        }

        size_t kept = 0;

        for(uint32_t i = 0; i < chunk; ++i) {
            if(clip_rect(fb, &batch[i])) {
                batch[kept] = batch[i];
                kept += 1;
            }
        }

        if(kept > 0) {
            int retval = fb->ops->flush(fb, batch, kept);

            if(retval < 0) {
                return retval;
            }
        }

        user_rects += chunk * sizeof(vexfb_rect_t);
        count -= chunk;
    }

    return 0;
}

static int ioctl_flush(const fb_interface_t* restrict fb, uintptr_t argument)
{
    vexfb_flush_t flush;

    if(user_copyin(&flush, argument, sizeof(flush)) < 0) {
        return -EFAULT;
    }

    if(flush.count > VEXFB_FLUSH_MAX_RECTANGLES) {
        return -EINVAL;
    }

    if(fb->ops->flush == nullptr) {
        return 0;
    }

    return flush_rects(fb, flush.rects, flush.count);
}

static int ioctl_palette(const fb_interface_t* restrict fb, uintptr_t argument, bool set)
{
    vexfb_palette_t palette;

    if(user_copyin(&palette, argument, sizeof(palette)) < 0) {
        return -EFAULT;
    }

    uint32_t size = fb->info.palette_size;

    if(size == 0 || palette.first > size || palette.count > size - palette.first) {
        return -EINVAL;
    }

    uint32_t colors[COLOR_BATCH];
    uint32_t first = palette.first;
    uint32_t remaining = palette.count;
    uintptr_t user_colors = palette.colors;

    while(remaining > 0) {
        uint32_t chunk = remaining < COLOR_BATCH ? remaining : COLOR_BATCH;
        size_t bytes = chunk * sizeof(uint32_t);
        int retval;

        if(set) {
            if(user_copyin(colors, user_colors, bytes) < 0) {
                return -EFAULT;
            }

            retval = fb->ops->set_palette(fb, first, chunk, colors);
        }
        else {
            retval = fb->ops->get_palette(fb, first, chunk, colors);

            if(retval == 0 && user_copyout(user_colors, colors, bytes) < 0) {
                retval = -EFAULT;
            }
        }

        if(retval < 0) {
            return retval;
        }

        first += chunk;
        user_colors += bytes;
        remaining -= chunk;
    }

    return 0;
}

static ssize_t read_memory(const fb_interface_t* restrict fb, off_t offset, void* restrict buffer, size_t size)
{
    if(offset < 0) {
        return -EINVAL;
    }

    if((uint64_t)(offset) >= fb->info.size) {
        return 0;
    }

    uint64_t left = fb->info.size - offset;

    if(size > left) {
        size = left;
    }

    memcpy(buffer, (const uint8_t*)(fb->virt) + offset, size);

    return size;
}

// Flushes the lines it touched, so a writer through write() needs no VEXFB_FLUSH
static ssize_t write_memory(const fb_interface_t* restrict fb, off_t offset, const void* restrict buffer, size_t size)
{
    if(offset < 0) {
        return -EINVAL;
    }

    if((uint64_t)(offset) >= fb->info.size) {
        return -ENOSPC;
    }

    uint64_t left = fb->info.size - offset;

    if(size > left) {
        size = left;
    }

    if(size == 0) {
        return 0;
    }

    memcpy((uint8_t*)(fb->virt) + offset, buffer, size);

    if(fb->ops->flush) {
        vexfb_rect_t rect;
        rect.x = 0;
        rect.y = offset / fb->info.pitch;
        rect.width = fb->info.width;
        rect.height = (offset + size - 1) / fb->info.pitch - rect.y + 1;

        if(clip_rect(fb, &rect)) {
            int retval = fb->ops->flush(fb, &rect, 1);

            if(retval < 0) {
                return retval;
            }
        }
    }

    return size;
}

static int ioctl_set_mode(const fb_interface_t* restrict fb, uintptr_t argument)
{
    vexfb_mode_t mode;

    if(user_copyin(&mode, argument, sizeof(mode)) < 0) {
        return -EFAULT;
    }

    if(fb->ops->set_mode == nullptr) {
        return -ENOTTY;
    }

    if(mode.flags) {
        return -EINVAL;
    }

    fb_interface_t* mutable_fb = (fb_interface_t*)(fb);
    int retval = fb->ops->set_mode(mutable_fb, &mode);

    if(retval < 0) {
        return retval;
    }

    char_interface_revoke_mappings(&fb->char_interface);

    return 0;
}

static int ioctl_get_modes(const fb_interface_t* restrict fb, uintptr_t argument)
{
    vexfb_modes_t modes;

    if(user_copyin(&modes, argument, sizeof(modes)) < 0) {
        return -EFAULT;
    }

    uint32_t copied = modes.count < fb->mode_count ? modes.count : fb->mode_count;

    if(copied && user_copyout(modes.modes, fb->modes, copied * sizeof(vexfb_mode_t)) < 0) {
        return -EFAULT;
    }

    modes.count = fb->mode_count;

    if(user_copyout(argument, &modes, sizeof(modes)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static int ioctl_request(const fb_interface_t* restrict fb, uint32_t request, uintptr_t argument)
{
    switch(request) {
        case VEXFB_GET_INFO:
            return user_copyout(argument, &fb->info, sizeof(fb->info)) < 0 ? -EFAULT : 0;

        case VEXFB_FLUSH:
            return ioctl_flush(fb, argument);

        case VEXFB_GET_PALETTE:
            return ioctl_palette(fb, argument, false);

        case VEXFB_SET_PALETTE:
            return ioctl_palette(fb, argument, true);

        case VEXFB_SET_MODE:
            return ioctl_set_mode(fb, argument);

        case VEXFB_GET_MODES:
            return ioctl_get_modes(fb, argument);

        default:
            return -ENOTTY;
    }
}

// A call that passed the chr layer's removed check may still be on its way
// here, so fb_interface_remove waits for those that got in before it was set
static bool enter(const fb_interface_t* restrict fb)
{
    fb_interface_t* mutable_fb = (fb_interface_t*)(fb);
    atomic_fetch_add(&mutable_fb->priv.in_flight, 1, ATOMIC_SEQ_CST);

    if(atomic_load(&mutable_fb->priv.removed, ATOMIC_SEQ_CST)) {
        atomic_fetch_sub(&mutable_fb->priv.in_flight, 1, ATOMIC_RELEASE);
        return false;
    }

    mutex_acquire(&mutable_fb->priv.lock);

    return true;
}

static void leave(const fb_interface_t* restrict fb)
{
    fb_interface_t* mutable_fb = (fb_interface_t*)(fb);
    mutex_release(&mutable_fb->priv.lock);
    atomic_fetch_sub(&mutable_fb->priv.in_flight, 1, ATOMIC_RELEASE);
}

static ssize_t fb_char_read(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset, void* restrict buffer,
    size_t size, unsigned flags)
{
    const fb_interface_t* fb = fb_of(interface);

    if(!enter(fb)) {
        return -ENODEV;
    }

    ssize_t retval = read_memory(fb, offset, buffer, size);
    leave(fb);

    return retval;
}

static ssize_t fb_char_write(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t offset,
    const void* restrict buffer, size_t size, unsigned flags)
{
    const fb_interface_t* fb = fb_of(interface);

    if(!enter(fb)) {
        return -ENODEV;
    }

    ssize_t retval = write_memory(fb, offset, buffer, size);
    leave(fb);

    return retval;
}

static off_t fb_char_seek(const char_interface_t* restrict interface, void* restrict file_ctxdata, off_t position, off_t offset, int origin)
{
    const fb_interface_t* fb = fb_of(interface);

    if(!enter(fb)) {
        return -ENODEV;
    }

    off_t size = fb->info.size;
    leave(fb);

    off_t base;

    if(origin == SEEK_SET) {
        base = 0;
    }
    else if(origin == SEEK_CUR) {
        base = position;
    }
    else if(origin == SEEK_END) {
        base = size;
    }
    else {
        return -EINVAL;
    }

    // The offset stays within the framebuffer, and the end counts as within
    if(offset < -base || offset > size - base) {
        return -EINVAL;
    }

    return base + offset;
}

static int fb_char_ioctl(const char_interface_t* restrict interface, void* restrict file_ctxdata, uint32_t request, uintptr_t argument)
{
    const fb_interface_t* fb = fb_of(interface);

    if(!enter(fb)) {
        return -ENODEV;
    }

    int retval = ioctl_request(fb, request, argument);
    leave(fb);

    return retval;
}

static int fb_char_mmap(const char_interface_t* restrict interface, off_t offset, size_t length)
{
    uint64_t size = mapped_size(fb_of(interface));

    if(offset < 0 || (uint64_t)(offset) > size || length > size - offset) {
        return -ENXIO;
    }

    return 0;
}

static int fb_char_resolve_page(const char_interface_t* restrict interface, off_t offset, phys_addr_t* restrict phys,
    unsigned* restrict cache)
{
    const fb_interface_t* fb = fb_of(interface);

    if(offset < 0 || (uint64_t)(offset) >= mapped_size(fb)) {
        return -ENXIO;
    }

    *phys = address_add(fb->phys, offset);
    *cache = fb->cache;

    return 0;
}

static void fb_char_release(const char_interface_t* restrict interface)
{
    const fb_interface_t* fb = fb_of(interface);

    if(fb->ops->release) {
        fb->ops->release(fb);
    }
}

static const char_interface_ops_t fb_char_ops = {
    .read = &fb_char_read,
    .write = &fb_char_write,
    .seek = &fb_char_seek,
    .ioctl = &fb_char_ioctl,
    .mmap = &fb_char_mmap,
    .resolve_page = &fb_char_resolve_page,
    .release = &fb_char_release,
};

static int add_device(fb_interface_t* restrict interface)
{
    interface->char_interface.ops = &fb_char_ops;
    interface->char_interface.ctxdata = interface;
    interface->priv.in_flight = 0;
    interface->priv.removed = false;
    mutex_init(&interface->priv.lock);

    int retval = dev_unit_alloc(&dev_prefix_fbd, interface->char_interface.device, &interface->priv.unit);

    if(retval < 0) {
        return retval;
    }

    retval = char_interface_add(&interface->char_interface, interface->priv.unit, DEV_NAME_NONE);

    if(retval < 0) {
        dev_unit_free(interface->priv.unit);
        return retval;
    }

    pr_inform("fb: %s: %ux%u, %u bpp", interface->char_interface.name, interface->info.width, interface->info.height, interface->info.bpp);

    return 0;
}

void fb_boot_add(fb_interface_t* restrict interface)
{
    kassert(!boot_added);

    interface->char_interface.console = nullptr;
    interface->char_interface.device = nullptr;
    interface->priv.boot_next = nullptr;

    if(!console_tried) {
        console_tried = true;
        interface->char_interface.console = attach_console(interface);
    }

    fb_interface_t** tail = &boot_head;

    while(*tail) {
        tail = &(*tail)->priv.boot_next;
    }

    *tail = interface;
}

int fb_boot_claim(phys_addr_t phys, size_t size)
{
    // UNDONE: remove the added ones instead which
    // revokes their mappings and leaves a gap in the names
    if(boot_added) {
        return -EBUSY;
    }

    fb_interface_t** prev = &boot_head;

    while(*prev) {
        fb_interface_t* interface = *prev;

        if(!overlaps(interface, phys, size)) {
            prev = &interface->priv.boot_next;
            continue;
        }

        *prev = interface->priv.boot_next;

        lose_console(interface);

        if(interface->ops->release) {
            interface->ops->release(interface);
        }
    }

    return 0;
}

int fb_interface_add(fb_interface_t* restrict interface)
{
    mutex_acquire(&console_mutex);

    interface->char_interface.console = nullptr;

    if(console_pending) {
        console_pending = false;
        interface->char_interface.console = attach_console(interface);
    }

    mutex_release(&console_mutex);

    int retval = add_device(interface);

    // The driver tears its memory down after a failure
    if(retval < 0) {
        lose_console(interface);
    }

    return retval;
}

int fb_interface_remove(fb_interface_t* restrict interface)
{
    atomic_store(&interface->priv.removed, true, ATOMIC_SEQ_CST);

    int retval = char_interface_remove(&interface->char_interface);

    if(retval < 0) {
        return retval;
    }

    while(atomic_load(&interface->priv.in_flight, ATOMIC_ACQUIRE)) {
        hart_relax();
    }

    // Even released, fbcons takes the screen back on a panic
    lose_console(interface);

    dev_unit_free(interface->priv.unit);

    return 0;
}

static void init_fb_boot(void)
{
    boot_added = true;

    for(fb_interface_t* interface = boot_head; interface; interface = interface->priv.boot_next) {
        int retval = add_device(interface);

        if(retval < 0) {
            pr_warning("fb: failed to add a boot framebuffer: %s", strerror(retval));
        }
    }
}

initcall_bsp_dev(init_fb_boot, INIT_LAST);
