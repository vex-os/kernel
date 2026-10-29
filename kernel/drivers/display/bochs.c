#include <vex/device.h>
#include <vex/driver.h>
#include <vex/errno.h>
#include <vex/fb_edid.h>
#include <vex/fb_interface.h>
#include <vex/kprintf.h>
#include <vex/pci.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/string.h>
#include <vex/vprot.h>

#define BAR_VRAM 0
#define BAR_MMIO 2

#define MMIO_EDID  0x000 // The base EDID block QEMU makes up for the display
#define MMIO_VGA   0x400 // Ports 0x3C0-0x3DF
#define MMIO_DISPI 0x500 // 16-bit registers, one per index

#define VGA_DAC_READ_INDEX  (MMIO_VGA + 0x07)
#define VGA_DAC_WRITE_INDEX (MMIO_VGA + 0x08)
#define VGA_DAC_DATA        (MMIO_VGA + 0x09)

#define DISPI_ID         0x00
#define DISPI_XRES       0x01
#define DISPI_YRES       0x02
#define DISPI_BPP        0x03
#define DISPI_ENABLE     0x04
#define DISPI_BANK       0x05
#define DISPI_VIRT_WIDTH 0x06
#define DISPI_X_OFFSET   0x08
#define DISPI_Y_OFFSET   0x09

#define DISPI_ID_MIN 0xB0C0
#define DISPI_ID_MAX 0xB0C5

#define DISPI_ENABLED     0x0001
#define DISPI_GETCAPS     0x0002 // XRES, YRES and BPP read back their limits while set
#define DISPI_8BIT_DAC    0x0020
#define DISPI_LFB_ENABLED 0x0040

#define DISPI_MAX_XRES 16000
#define DISPI_MAX_YRES 12000
#define DISPI_MAX_BPP  32

#define PALETTE_SIZE 256
#define MAX_MODES    32

typedef struct bochsfb_s bochsfb_t;

struct bochsfb_s {
    fb_interface_t interface;
    io_region_t mmio;
    io_region_t vram;
    phys_addr_t vram_phys;
    spinlock_t dac_lock; // The DAC takes an index, then three data accesses per color
    unsigned dac_bits;
    bool has_vga; // 8 and 24 bpp need the VGA side, which bochs-display lacks
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_bpp;
    vexfb_mode_t modes[MAX_MODES];
};

static_assert(sizeof(bochsfb_t) <= SLAB_MAX_SIZE);

static uint16_t dispi_read(const io_region_t* restrict mmio, unsigned index)
{
    return io_read16(mmio, MMIO_DISPI + index * 2);
}

static void dispi_write(const io_region_t* restrict mmio, unsigned index, uint16_t value)
{
    io_write16(mmio, MMIO_DISPI + index * 2, value);
}

static void set_channel(vexfb_channel_t* restrict channel, uint32_t bit_offset, uint32_t bit_length)
{
    channel->bit_offset = bit_offset;
    channel->bit_length = bit_length;
}

static bool describe(const io_region_t* restrict mmio, vexfb_info_t* restrict info, uint64_t* restrict offset)
{
    uint16_t enable = dispi_read(mmio, DISPI_ENABLE);

    if(0 == (enable & DISPI_ENABLED) || 0 == (enable & DISPI_LFB_ENABLED)) {
        return false;
    }

    memset(info, 0, sizeof(vexfb_info_t));

    uint32_t bpp = dispi_read(mmio, DISPI_BPP);

    switch(bpp) {
        case 8:
            info->palette_size = PALETTE_SIZE;
            break;
        case 16:
            set_channel(&info->red, 11, 5);
            set_channel(&info->green, 5, 6);
            set_channel(&info->blue, 0, 5);
            break;
        case 24:
        case 32:
            set_channel(&info->red, 16, 8);
            set_channel(&info->green, 8, 8);
            set_channel(&info->blue, 0, 8);
            break;
        default:
            return false;
    }

    uint32_t bytes_per_pixel = (bpp + 7) / 8;
    uint32_t virt_width = dispi_read(mmio, DISPI_VIRT_WIDTH);

    info->width = dispi_read(mmio, DISPI_XRES);
    info->height = dispi_read(mmio, DISPI_YRES);
    info->bpp = 8 * bytes_per_pixel;
    info->pitch = (virt_width > info->width ? virt_width : info->width) * bytes_per_pixel;
    info->size = (uint64_t)(info->pitch) * info->height;

    *offset = (uint64_t)(dispi_read(mmio, DISPI_Y_OFFSET)) * info->pitch + dispi_read(mmio, DISPI_X_OFFSET) * bytes_per_pixel;

    return info->width && info->height;
}

static uint32_t dac_to_rgb(unsigned dac_bits, uint8_t value)
{
    if(dac_bits == 8) {
        return value;
    }

    return (value << 2) | (value >> 4);
}

static int bochsfb_get_palette(const fb_interface_t* restrict interface, uint32_t first, uint32_t count, uint32_t* restrict colors)
{
    bochsfb_t* bochsfb = interface->ctxdata;
    bool intr_state = spinlock_acquire_intr(&bochsfb->dac_lock);

    io_write8(&bochsfb->mmio, VGA_DAC_READ_INDEX, first);

    for(uint32_t i = 0; i < count; ++i) {
        uint32_t red = dac_to_rgb(bochsfb->dac_bits, io_read8(&bochsfb->mmio, VGA_DAC_DATA));
        uint32_t green = dac_to_rgb(bochsfb->dac_bits, io_read8(&bochsfb->mmio, VGA_DAC_DATA));
        uint32_t blue = dac_to_rgb(bochsfb->dac_bits, io_read8(&bochsfb->mmio, VGA_DAC_DATA));
        colors[i] = (red << 16) | (green << 8) | blue;
    }

    spinlock_release_intr(&bochsfb->dac_lock, intr_state);

    return 0;
}

static int bochsfb_set_palette(const fb_interface_t* restrict interface, uint32_t first, uint32_t count, const uint32_t* restrict colors)
{
    bochsfb_t* bochsfb = interface->ctxdata;
    unsigned shift = 8 - bochsfb->dac_bits;
    bool intr_state = spinlock_acquire_intr(&bochsfb->dac_lock);

    io_write8(&bochsfb->mmio, VGA_DAC_WRITE_INDEX, first);

    for(uint32_t i = 0; i < count; ++i) {
        io_write8(&bochsfb->mmio, VGA_DAC_DATA, ((colors[i] >> 16) & 0xFF) >> shift);
        io_write8(&bochsfb->mmio, VGA_DAC_DATA, ((colors[i] >> 8) & 0xFF) >> shift);
        io_write8(&bochsfb->mmio, VGA_DAC_DATA, (colors[i] & 0xFF) >> shift);
    }

    spinlock_release_intr(&bochsfb->dac_lock, intr_state);

    return 0;
}

static bool mode_supported(const bochsfb_t* restrict bochsfb, const vexfb_mode_t* restrict mode)
{
    bool depth_supported = mode->bpp == 16 || mode->bpp == 32 || (bochsfb->has_vga && (mode->bpp == 8 || mode->bpp == 24));

    // DISPI takes widths in steps of 8
    bool empty = mode->width == 0 || mode->height == 0;

    if(!depth_supported || empty || mode->width % 8) {
        return false;
    }

    if(mode->width > bochsfb->max_width || mode->height > bochsfb->max_height || mode->bpp > bochsfb->max_bpp) {
        return false;
    }

    return (uint64_t)(mode->width) * (mode->bpp / 8) * mode->height <= bochsfb->vram.length;
}

// Nothing but register accesses, as a panic calls this too
static int bochsfb_set_mode(fb_interface_t* restrict interface, const vexfb_mode_t* restrict mode)
{
    bochsfb_t* bochsfb = interface->ctxdata;
    const io_region_t* mmio = &bochsfb->mmio;

    if(!mode_supported(bochsfb, mode)) {
        return -EINVAL;
    }

    uint16_t dac = dispi_read(mmio, DISPI_ENABLE) & DISPI_8BIT_DAC;

    dispi_write(mmio, DISPI_ENABLE, 0);
    dispi_write(mmio, DISPI_BPP, mode->bpp);
    dispi_write(mmio, DISPI_XRES, mode->width);
    dispi_write(mmio, DISPI_YRES, mode->height);
    dispi_write(mmio, DISPI_BANK, 0);
    dispi_write(mmio, DISPI_VIRT_WIDTH, mode->width);
    dispi_write(mmio, DISPI_X_OFFSET, 0);
    dispi_write(mmio, DISPI_Y_OFFSET, 0);
    dispi_write(mmio, DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED | dac);

    vexfb_info_t info;
    uint64_t offset;

    if(!describe(mmio, &info, &offset) || offset + info.size > bochsfb->vram.length) {
        return -EIO;
    }

    interface->info = info;
    interface->phys = address_add(bochsfb->vram_phys, offset);
    interface->virt = (uint8_t*)(bochsfb->vram.base) + offset;

    return 0;
}

static void bochsfb_release(const fb_interface_t* restrict interface)
{
    slab_free(interface->ctxdata);
}

static const fb_interface_ops_t bochsfb_ops = {
    .get_palette = &bochsfb_get_palette,
    .set_palette = &bochsfb_set_palette,
    .set_mode = &bochsfb_set_mode,
    .release = &bochsfb_release,
};

// Rewriting ENABLE with DISPI_ENABLED still set
// keeps the mode, rather than setting it again
static void read_limits(bochsfb_t* restrict bochsfb, const vexfb_info_t* restrict info)
{
    const io_region_t* mmio = &bochsfb->mmio;
    uint16_t enable = dispi_read(mmio, DISPI_ENABLE);

    dispi_write(mmio, DISPI_ENABLE, enable | DISPI_GETCAPS);
    bochsfb->max_width = dispi_read(mmio, DISPI_XRES);
    bochsfb->max_height = dispi_read(mmio, DISPI_YRES);
    bochsfb->max_bpp = dispi_read(mmio, DISPI_BPP);
    dispi_write(mmio, DISPI_ENABLE, enable);

    // bochs-display reads back the mode instead
    if(bochsfb->max_width <= info->width && bochsfb->max_height <= info->height) {
        bochsfb->max_width = DISPI_MAX_XRES;
        bochsfb->max_height = DISPI_MAX_YRES;
        bochsfb->max_bpp = DISPI_MAX_BPP;
    }
}

static size_t keep_mode(bochsfb_t* restrict bochsfb, size_t count, uint32_t width, uint32_t height, uint32_t bpp, uint32_t flags)
{
    vexfb_mode_t mode = {};
    mode.width = width;
    mode.height = height;
    mode.bpp = bpp;
    mode.flags = flags;

    if(count >= MAX_MODES || !mode_supported(bochsfb, &mode)) {
        return count;
    }

    for(size_t i = 0; i < count; ++i) {
        if(bochsfb->modes[i].width == width && bochsfb->modes[i].height == height) {
            return count;
        }
    }

    bochsfb->modes[count] = mode;

    return count + 1;
}

// What the EDID suggests, or a list of the usual modes without one, both
// cut to what the device can set, and the current mode in any case
static size_t collect_modes(bochsfb_t* restrict bochsfb, const vexfb_info_t* restrict info)
{
    static const uint16_t usual_modes[][2] = {
        { 640, 480 },
        { 800, 600 },
        { 1024, 768 },
        { 1280, 720 },
        { 1280, 800 },
        { 1280, 1024 },
        { 1600, 900 },
        { 1920, 1080 },
    };

    uint8_t block[EDID_BLOCK_SIZE];
    vexfb_mode_t found[MAX_MODES];

    io_copy_from(&bochsfb->mmio, MMIO_EDID, block, sizeof(block));

    size_t found_count = fb_edid_parse_modes(block, found, MAX_MODES);
    size_t count = 0;

    for(size_t i = 0; i < found_count; ++i) {
        count = keep_mode(bochsfb, count, found[i].width, found[i].height, 32, found[i].flags);
    }

    if(found_count == 0) {
        for(size_t i = 0; i < sizeof(usual_modes) / sizeof(usual_modes[0]); ++i) {
            count = keep_mode(bochsfb, count, usual_modes[i][0], usual_modes[i][1], 32, 0);
        }
    }

    return keep_mode(bochsfb, count, info->width, info->height, info->bpp, 0);
}

static int bochsfb_probe(device_t* restrict device, const driver_match_t* restrict match)
{
    return DRIVER_SCORE_SPECIFIC;
}

static int bochsfb_attach(device_t* restrict node, const driver_match_t* restrict match)
{
    pci_function_t* function = pci_function_of(node);
    io_region_t mmio;

    // UNDONE: old QEMU std-vga has no MMIO BAR
    // and only takes DISPI through ports 0x1CE/0x1CF
    if(device_map_io(node, BAR_MMIO, &mmio) < 0 || mmio.space != IO_SPACE_MEMORY) {
        pr_verbose("bochsfb: no register BAR");
        return -ENODEV;
    }

    uint16_t dispi_id = dispi_read(&mmio, DISPI_ID);

    if(dispi_id < DISPI_ID_MIN || dispi_id > DISPI_ID_MAX) {
        pr_verbose("bochsfb: unknown DISPI version 0x%04X", dispi_id);
        return -ENODEV;
    }

    vexfb_info_t info;
    uint64_t offset;

    // UNDONE: set a mode of our own rather than give up
    if(!describe(&mmio, &info, &offset)) {
        pr_verbose("bochsfb: no linear framebuffer mode is set");
        return -ENODEV;
    }

    io_region_t vram;
    uint64_t vram_address;

    if(device_get_io_address(node, BAR_VRAM, &vram_address) < 0) {
        pr_warning("bochsfb: no video memory BAR");
        return -ENODEV;
    }

    if(device_map_io_vprot(node, BAR_VRAM, VPROT_RW | VPROT_CACHE_WC, &vram) < 0) {
        pr_warning("bochsfb: no video memory BAR");
        return -ENODEV;
    }

    if(offset + info.size > vram.length) {
        pr_warning("bochsfb: the mode doesn't fit the video memory");
        return -ENODEV;
    }

    phys_addr_t vram_phys = phys_addr(vram_address);
    bochsfb_t* bochsfb = slab_alloc(sizeof(bochsfb_t));

    if(bochsfb == nullptr) {
        return -ENOMEM;
    }

    memset(bochsfb, 0, sizeof(bochsfb_t));

    bochsfb->mmio = mmio;
    bochsfb->vram = vram;
    bochsfb->vram_phys = vram_phys;
    bochsfb->dac_lock = SPINLOCK_INIT;
    bochsfb->has_vga = function->class_id == 0x03 && function->subclass_id == 0x00;

    if(dispi_read(&mmio, DISPI_ENABLE) & DISPI_8BIT_DAC) {
        bochsfb->dac_bits = 8;
    }
    else {
        bochsfb->dac_bits = 6;
    }

    read_limits(bochsfb, &info);

    fb_interface_t* interface = &bochsfb->interface;
    interface->ops = &bochsfb_ops;
    interface->info = info;
    interface->phys = address_add(vram_phys, offset);
    interface->cache = VPROT_CACHE_WC;
    interface->virt = (uint8_t*)(vram.base) + offset;
    interface->modes = bochsfb->modes;
    interface->mode_count = collect_modes(bochsfb, &info);
    interface->ctxdata = bochsfb;
    interface->char_interface.device = node;

    // NOTE: after a successful claim the
    // screen is ours even if the add below fails
    if(fb_boot_claim(vram_phys, vram.length) < 0) {
        pr_warning("bochsfb: boot framebuffers are already added, one may share the memory");
    }

    int retval = fb_interface_add(interface);

    if(retval < 0) {
        slab_free(bochsfb);
        return retval;
    }

    node->ctxdata = bochsfb;

    pr_inform("bochsfb: %s: DISPI 0x%04X, %zu KiB of video memory, up to %ux%u, %u modes listed", interface->char_interface.name, dispi_id,
        (size_t)(vram.length / 1024), bochsfb->max_width, bochsfb->max_height, interface->mode_count);

    return 0;
}

// Calls through the fb core after this are refused, and the framework
// unmaps the BARs; the rest goes with the fb core's last reference
static void bochsfb_detach(device_t* restrict node)
{
    bochsfb_t* bochsfb = node->ctxdata;

    fb_interface_remove(&bochsfb->interface);
}

// The fb core frees the driver's data through bochsfb_release
static void bochsfb_driver_release(device_t* restrict node)
{
    // empty
}

static const driver_ops_t bochsfb_driver_ops = {
    .probe = &bochsfb_probe,
    .attach = &bochsfb_attach,
    .detach = &bochsfb_detach,
    .release = &bochsfb_driver_release,
};

static const driver_match_t bochsfb_matches[] = {
    DRIVER_MATCH_PCI_ID(0x1234, 0x1111),
    DRIVER_MATCH_END(),
};

define_driver(bochsfb, &bochsfb_driver_ops, bochsfb_matches);
