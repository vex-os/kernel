#include <vex/atomic.h>
#include <vex/block_ata.h>
#include <vex/block_atapi.h>
#include <vex/block_event.h>
#include <vex/block_interface.h>
#include <vex/clocksrc.h>
#include <vex/delay.h>
#include <vex/dev_name.h>
#include <vex/device.h>
#include <vex/driver.h>
#include <vex/event.h>
#include <vex/hart.h>
#include <vex/hhdm.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/mutex.h>
#include <vex/page.h>
#include <vex/pdma.h>
#include <vex/string.h>

#define REG_DATA     0 // In the command block
#define REG_ERROR    1
#define REG_FEATURES 1
#define REG_COUNT    2
#define REG_LBA0     3
#define REG_LBA1     4 // LBA Mid, the byte count low of a packet
#define REG_LBA2     5 // LBA High, the byte count high of a packet
#define REG_DEVICE   6
#define REG_STATUS   7
#define REG_COMMAND  7

#define BM_COMMAND 0 // Programming Interface for Bus Master IDE Controller 1.0, 2.0
#define BM_STATUS  2
#define BM_PRDT    4

#define BM_COMMAND_START UINT8_C(0x01)
#define BM_COMMAND_READ  UINT8_C(0x08) // The engine writes memory, as for a read from the drive

#define BM_STATUS_ERROR UINT8_C(0x02) // Write 1 to clear
#define BM_STATUS_IRQ   UINT8_C(0x04) // Write 1 to clear

#define PRD_EOT      UINT16_C(0x8000)
#define PRD_BOUNDARY UINT64_C(0x10000) // An entry may not cross it

#define ATAPI_FEATURES_DMA         UINT8_C(0x01)
#define ATAPI_CMD_READ_CAPACITY_10 0x25

#define PATA_DRIVES       2 // The master and the slave
#define PATA_SCRATCH_SIZE 0x200
#define PATA_MAX_TRANSFER 0x10000 // Bytes per command, 128 sectors of 512 bytes
#define PATA_MAX_SEGMENTS 128
#define PATA_PACKET_BYTES 0xF800 // The byte count a packet asks for per DRQ block, a multiple of 2048
#define PATA_PRDT_OFFSET  PATA_SCRATCH_SIZE
#define PATA_PRDT_MAX     ((PAGE_SIZE - PATA_PRDT_OFFSET) / sizeof(pata_prd_t))

#define PATA_COMMAND_TIMEOUT_NS UINT64_C(10000000000)
#define PATA_RESET_TIMEOUT_NS   UINT64_C(5000000000)

typedef struct pata_prd_s pata_prd_t;
typedef struct pata_cursor_s pata_cursor_t;
typedef struct pata_channel_s pata_channel_t;
typedef struct pata_drive_s pata_drive_t;

struct pata_prd_s {
    uint32_t address;
    uint16_t length; // In bytes, 0 for 64 KiB
    uint16_t flags;
} __packed;

static_assert(8 == sizeof(pata_prd_t));
static_assert(sizeof(ata_identify_t) <= PATA_SCRATCH_SIZE);

// Each segment of a transfer splits at most once at a 64 KiB boundary
static_assert(PATA_MAX_SEGMENTS * 2 <= PATA_PRDT_MAX);

struct pata_cursor_s {
    const block_segment_t* segments;
    size_t num_segments;
    size_t index;
    size_t offset; // In the segment at the index
};

struct pata_drive_s {
    block_interface_t interface;
    atapi_interface_t atapi;
    pata_channel_t* channel;
    uint8_t select; // ATA_DEVICE_SLAVE or 0
    bool lba48;
    bool dma;
    bool is_atapi;
    bool registered;
};

struct pata_channel_s {
    mutex_t lock; // Held for a whole command, as the two drives share the registers and the scratch page
    event_t done; // Triggered by the interrupt handler, reset before each phase that ends in an interrupt
    io_region_t command;
    io_region_t control; // Device Control and Alternate Status at offset 0
    io_region_t busmaster;
    phys_addr_t scratch_phys; // Below 16 MiB, as pdma gives it; null once the channel is torn down
    bool has_busmaster;
    bool irq;
    pata_drive_t drives[PATA_DRIVES];
    dev_unit_t* ata_unit;
    dev_unit_t* atapi_unit;
    unsigned refcount;
    device_t* device;
};

static void channel_decref(pata_channel_t* restrict channel)
{
    if(1 == atomic_fetch_sub(&channel->refcount, 1, ATOMIC_ACQ_REL)) {
        mutex_deinit(&channel->lock);
        event_deinit(&channel->done);
        kfree(channel);
    }
}

static void free_units(pata_channel_t* restrict channel)
{
    if(channel->ata_unit) {
        dev_unit_free(channel->ata_unit);
        channel->ata_unit = nullptr;
    }

    if(channel->atapi_unit) {
        dev_unit_free(channel->atapi_unit);
        channel->atapi_unit = nullptr;
    }
}

static void pata_block_release(const block_interface_t* restrict interface)
{
    const pata_drive_t* drive = interface->ctxdata;
    channel_decref(drive->channel);
}

static void pata_atapi_release(const atapi_interface_t* restrict interface)
{
    const pata_drive_t* drive = interface->base.ctxdata;
    channel_decref(drive->channel);
}

// A channel torn down by pata_detach may still be reached
// through a block device someone holds a reference to
static __nodiscard bool channel_acquire(pata_channel_t* restrict channel)
{
    mutex_acquire(&channel->lock);

    if(address_null(channel->scratch_phys)) {
        mutex_release(&channel->lock);
        return false;
    }

    return true;
}

static uint8_t read_alt_status(const pata_channel_t* restrict channel)
{
    return io_read8(&channel->control, 0);
}

static void write_control(const pata_channel_t* restrict channel, uint8_t value)
{
    io_write8(&channel->control, 0, value);
}

// ATA/ATAPI-6, 9.1: the status is valid
// 400 ns after a command or a drive select
static void delay_400ns(const pata_channel_t* restrict channel)
{
    for(unsigned i = 0; i < 4; ++i) {
        read_alt_status(channel);
    }
}

static bool wait_alt_status(const pata_channel_t* restrict channel, uint8_t mask, uint8_t value, uint64_t timeout)
{
    uint64_t deadline = clock_source_now_ns() + timeout;

    while((read_alt_status(channel) & mask) != value) {
        if(clock_source_now_ns() >= deadline) {
            return false;
        }

        hart_spin();
    }

    return true;
}

static void reset_channel(const pata_channel_t* restrict channel)
{
    uint8_t control = channel->irq ? 0 : ATA_CONTROL_NIEN;

    write_control(channel, control | ATA_CONTROL_SRST);
    delay_usec(5);
    write_control(channel, control);
    delay_msec(2);

    if(!wait_alt_status(channel, ATA_STATUS_BSY, 0, PATA_RESET_TIMEOUT_NS)) {
        pr_verbose("pata: %s: channel stays busy after a reset", channel->device->name);
    }
}

// Reading Status, not Alternate Status, takes INTRQ down, as an edge-triggered
// ISA line raises no new interrupt while the previous one is still up
static int wait_not_busy(pata_channel_t* restrict channel, uint8_t* restrict status)
{
    uint64_t deadline = clock_source_now_ns() + PATA_COMMAND_TIMEOUT_NS;

    // Nothing can switch away from a caller with interrupts
    // disabled, such as kmain mounting the root during boot
    bool sleep = channel->irq && interrupt_state();

    delay_400ns(channel);

    for(;;) {
        if(0 == (read_alt_status(channel) & ATA_STATUS_BSY)) {
            *status = io_read8(&channel->command, REG_STATUS);
            return 0;
        }

        uint64_t now = clock_source_now_ns();

        if(now >= deadline) {
            pr_verbose("pata: %s: command timeout", channel->device->name);
            reset_channel(channel);
            return -EIO;
        }

        if(sleep) {
            event_t* done = &channel->done;
            event_await_timeout(&done, 1, deadline - now); // The status is read from the registers either way
        }
        else {
            hart_spin();
        }
    }
}

static int check_status(pata_channel_t* restrict channel, uint8_t status, bool want_data)
{
    if(status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        unsigned error = io_read8(&channel->command, REG_ERROR);
        pr_verbose("pata: %s: command error: status=%02X error=%02X", channel->device->name, status, error);
        return -EIO;
    }

    if(want_data && 0 == (status & ATA_STATUS_DRQ)) {
        pr_verbose("pata: %s: no data from the drive: status=%02X", channel->device->name, status);
        return -EIO;
    }

    return 0;
}

static int wait_data(pata_channel_t* restrict channel)
{
    uint8_t status;
    int retval = wait_not_busy(channel, &status);

    if(retval < 0) {
        return retval;
    }

    return check_status(channel, status, true);
}

static int wait_done(pata_channel_t* restrict channel)
{
    uint8_t status;
    int retval = wait_not_busy(channel, &status);

    if(retval < 0) {
        return retval;
    }

    return check_status(channel, status, false);
}

// ATA/ATAPI-6, 9.6: a drive is selected only while the channel is neither busy nor transferring
static int select_drive(pata_channel_t* restrict channel, uint8_t device)
{
    if(!wait_alt_status(channel, ATA_STATUS_BSY | ATA_STATUS_DRQ, 0, PATA_COMMAND_TIMEOUT_NS)) {
        pr_verbose("pata: %s: channel stuck busy", channel->device->name);
        reset_channel(channel);
        return -EIO;
    }

    io_write8(&channel->command, REG_DEVICE, ATA_DEVICE_FIXED | device);
    delay_400ns(channel);

    return 0;
}

static void issue(pata_channel_t* restrict channel, uint8_t command)
{
    event_reset(&channel->done);
    io_write8(&channel->command, REG_COMMAND, command);
}

// The drive moves whole words. What it has beyond the
// segments is read and dropped, or written as zeros
static void move_data(pata_channel_t* restrict channel, pata_cursor_t* restrict cursor, size_t length, bool write)
{
    size_t left = (length + 1) & ~(size_t)(1);

    while(left && cursor->index < cursor->num_segments) {
        const block_segment_t* segment = &cursor->segments[cursor->index];
        size_t count = segment->length - cursor->offset;

        if(count > left) {
            count = left;
        }

        uint16_t* data = phys_to_virt(address_add(segment->phys, cursor->offset));

        if(write) {
            io_write16s(&channel->command, REG_DATA, data, count / 2);
        }
        else {
            io_read16s(&channel->command, REG_DATA, data, count / 2);
        }

        cursor->offset += count;
        left -= count;

        if(cursor->offset == segment->length) {
            cursor->index += 1;
            cursor->offset = 0;
        }
    }

    while(left) {
        if(write) {
            io_write16(&channel->command, REG_DATA, 0);
        }
        else {
            io_read16(&channel->command, REG_DATA);
        }

        left -= 2;
    }
}

static void write_taskfile(pata_channel_t* restrict channel, uint64_t lba, uint16_t count, bool lba48)
{
    if(lba48) {
        io_write8(&channel->command, REG_COUNT, count >> 8);
        io_write8(&channel->command, REG_LBA0, lba >> 24);
        io_write8(&channel->command, REG_LBA1, lba >> 32);
        io_write8(&channel->command, REG_LBA2, lba >> 40);
    }

    io_write8(&channel->command, REG_COUNT, count);
    io_write8(&channel->command, REG_LBA0, lba);
    io_write8(&channel->command, REG_LBA1, lba >> 8);
    io_write8(&channel->command, REG_LBA2, lba >> 16);
}

static bool build_prdt(pata_channel_t* restrict channel, const block_segment_t* restrict segments, size_t num_segments)
{
    volatile pata_prd_t* prdt = phys_to_virt(address_add(channel->scratch_phys, PATA_PRDT_OFFSET));
    size_t count = 0;

    for(size_t i = 0; i < num_segments; ++i) {
        phys_addr_t phys = segments[i].phys;
        size_t left = segments[i].length;

        while(left) {
            size_t length = PRD_BOUNDARY - (phys.value & (PRD_BOUNDARY - 1));

            if(length > left) {
                length = left;
            }

            prdt[count].address = htole32(phys.value);
            prdt[count].length = htole16(length); // 64 KiB becomes 0, as the format wants
            prdt[count].flags = 0;

            phys = address_add(phys, length);
            left -= length;
            count += 1;
        }
    }

    if(count == 0) {
        return false;
    }

    prdt[count - 1].flags = htole16(PRD_EOT);

    return true;
}

static void prepare_dma(pata_channel_t* restrict channel, bool write)
{
    io_write32(&channel->busmaster, BM_PRDT, address_add(channel->scratch_phys, PATA_PRDT_OFFSET).value);
    io_write8(&channel->busmaster, BM_STATUS, BM_STATUS_ERROR | BM_STATUS_IRQ);
    io_write8(&channel->busmaster, BM_COMMAND, write ? 0 : BM_COMMAND_READ);
}

static void start_dma(pata_channel_t* restrict channel)
{
    uint8_t command = io_read8(&channel->busmaster, BM_COMMAND);
    io_write8(&channel->busmaster, BM_COMMAND, command | BM_COMMAND_START);
}

// The engine stops also after a timeout, as it would go on writing memory otherwise
static int finish_dma(pata_channel_t* restrict channel)
{
    uint8_t status;
    int retval = wait_not_busy(channel, &status);

    uint8_t command = io_read8(&channel->busmaster, BM_COMMAND);
    io_write8(&channel->busmaster, BM_COMMAND, command & ~BM_COMMAND_START);

    uint8_t busmaster_status = io_read8(&channel->busmaster, BM_STATUS);
    io_write8(&channel->busmaster, BM_STATUS, BM_STATUS_ERROR | BM_STATUS_IRQ);

    if(retval < 0) {
        return retval;
    }

    if(busmaster_status & BM_STATUS_ERROR) {
        pr_verbose("pata: %s: bus master error: status=%02X", channel->device->name, busmaster_status);
        return -EIO;
    }

    return check_status(channel, status, false);
}

static int run_ata_dma(pata_drive_t* restrict drive, uint8_t command, uint64_t lba, uint16_t count, const block_request_t* restrict req,
    bool write)
{
    pata_channel_t* channel = drive->channel;
    uint8_t device = ATA_DEVICE_LBA | drive->select;

    if(!drive->lba48) {
        device |= (lba >> 24) & 0x0F;
    }

    if(!build_prdt(channel, req->segments, req->num_segments)) {
        return -EINVAL;
    }

    int retval = select_drive(channel, device);

    if(retval < 0) {
        return retval;
    }

    prepare_dma(channel, write);
    write_taskfile(channel, lba, count, drive->lba48);
    issue(channel, command);
    start_dma(channel);

    return finish_dma(channel);
}

// LBA28 keeps the top four bits of the address in the device register
static int run_ata(pata_drive_t* restrict drive, uint8_t command, uint64_t lba, uint16_t count, size_t sector_size,
    pata_cursor_t* restrict cursor, bool write)
{
    pata_channel_t* channel = drive->channel;
    uint8_t device = ATA_DEVICE_LBA | drive->select;

    if(!drive->lba48) {
        device |= (lba >> 24) & 0x0F;
    }

    int retval = select_drive(channel, device);

    if(retval < 0) {
        return retval;
    }

    write_taskfile(channel, lba, count, drive->lba48);
    issue(channel, command);

    // The drive interrupts once a block of a read is ready, and once a block of a write is taken
    for(uint16_t i = 0; i < count; ++i) {
        retval = wait_data(channel);

        if(retval < 0) {
            return retval;
        }

        event_reset(&channel->done);
        move_data(channel, cursor, sector_size, write);
    }

    return wait_done(channel);
}

// With a PRD table built, the data goes by DMA and the cursor is left alone
static int run_packet(pata_drive_t* restrict drive, const atapi_cdb_t cdb, pata_cursor_t* restrict cursor, bool write, bool dma)
{
    pata_channel_t* channel = drive->channel;
    int retval = select_drive(channel, drive->select);

    if(retval < 0) {
        return retval;
    }

    if(dma) {
        prepare_dma(channel, write);
    }

    io_write8(&channel->command, REG_FEATURES, dma ? ATAPI_FEATURES_DMA : 0);
    io_write8(&channel->command, REG_LBA1, PATA_PACKET_BYTES & 0xFF);
    io_write8(&channel->command, REG_LBA2, PATA_PACKET_BYTES >> 8);
    issue(channel, ATA_CMD_PACKET);

    retval = wait_data(channel);

    if(retval < 0) {
        return retval;
    }

    uint16_t words[sizeof(atapi_cdb_t) / 2];
    memcpy(words, cdb, sizeof(atapi_cdb_t));

    event_reset(&channel->done);
    io_write16s(&channel->command, REG_DATA, words, sizeof(atapi_cdb_t) / 2);

    if(dma) {
        start_dma(channel);
        return finish_dma(channel);
    }

    for(;;) {
        uint8_t status;
        retval = wait_not_busy(channel, &status);

        if(retval < 0) {
            return retval;
        }

        retval = check_status(channel, status, false);

        if(retval < 0 || 0 == (status & ATA_STATUS_DRQ)) {
            return retval;
        }

        size_t length = io_read8(&channel->command, REG_LBA1);
        length |= (size_t)(io_read8(&channel->command, REG_LBA2)) << 8;

        event_reset(&channel->done);
        move_data(channel, cursor, length, write);
    }
}

static void scratch_cursor(const pata_channel_t* restrict channel, block_segment_t* restrict segment, size_t length,
    pata_cursor_t* restrict cursor)
{
    segment->phys = channel->scratch_phys;
    segment->length = length;

    cursor->segments = segment;
    cursor->num_segments = 1;
    cursor->index = 0;
    cursor->offset = 0;
}

static void request_cursor(const block_request_t* restrict req, pata_cursor_t* restrict cursor)
{
    cursor->segments = req->segments;
    cursor->num_segments = req->num_segments;
    cursor->index = 0;
    cursor->offset = 0;
}

static int identify(pata_drive_t* restrict drive, uint8_t command, ata_identify_t identify)
{
    pata_channel_t* channel = drive->channel;
    block_segment_t segment;
    pata_cursor_t cursor;

    scratch_cursor(channel, &segment, sizeof(ata_identify_t), &cursor);

    int retval = select_drive(channel, drive->select);

    if(retval < 0) {
        return retval;
    }

    issue(channel, command);
    retval = wait_data(channel);

    if(retval < 0) {
        return retval;
    }

    move_data(channel, &cursor, sizeof(ata_identify_t), false);

    void* scratch = phys_to_virt(channel->scratch_phys);
    memcpy(identify, scratch, sizeof(ata_identify_t));

    return wait_done(channel);
}

static int atapi_capacity(pata_drive_t* restrict drive, size_t* restrict sector_count, size_t* restrict sector_size)
{
    pata_channel_t* channel = drive->channel;
    uint8_t result[8];
    block_segment_t segment;
    pata_cursor_t cursor;

    scratch_cursor(channel, &segment, sizeof(result), &cursor);

    atapi_cdb_t cdb;
    memset(cdb, 0, sizeof(atapi_cdb_t));
    cdb[0] = ATAPI_CMD_READ_CAPACITY_10;

    int retval = run_packet(drive, cdb, &cursor, false, false);

    if(retval < 0) {
        return retval;
    }

    if(cursor.index == 0) {
        return -EIO;
    }

    void* scratch = phys_to_virt(channel->scratch_phys);
    memcpy(result, scratch, sizeof(result));

    *sector_count = (size_t)(result[0]) << 24;
    *sector_count |= (size_t)(result[1]) << 16;
    *sector_count |= (size_t)(result[2]) << 8;
    *sector_count |= (size_t)(result[3]);
    *sector_count += 1;

    *sector_size = (size_t)(result[4]) << 24;
    *sector_size |= (size_t)(result[5]) << 16;
    *sector_size |= (size_t)(result[6]) << 8;
    *sector_size |= (size_t)(result[7]);

    return 0;
}

static void finish(block_request_t* restrict req, int status)
{
    req->status = status;
    req->done(req);
}

static void pata_atapi_submit(const atapi_interface_t* restrict interface, const atapi_cdb_t cdb, block_request_t* restrict req)
{
    pata_drive_t* drive = interface->base.ctxdata;
    pata_cursor_t cursor;

    if(!channel_acquire(drive->channel)) {
        finish(req, -ENODEV);
        return;
    }

    request_cursor(req, &cursor);

    bool dma = drive->dma && req->num_segments && build_prdt(drive->channel, req->segments, req->num_segments);
    int retval = run_packet(drive, cdb, &cursor, req->op == BLOCK_REQ_WRITE, dma);

    mutex_release(&drive->channel->lock);

    finish(req, retval);
}

static const atapi_interface_ops_t pata_atapi_ops = {
    .submit_packet = &pata_atapi_submit,
    .release = &pata_atapi_release,
};

static int transfer(pata_drive_t* restrict drive, const block_interface_t* restrict interface, const block_request_t* restrict req,
    bool write)
{
    uint16_t count = (uint16_t)(req->length / interface->sector_size);

    if(drive->dma) {
        uint8_t command = drive->lba48 ? ATA_CMD_READ_DMA_EXT : ATA_CMD_READ_DMA;

        if(write) {
            command = drive->lba48 ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_WRITE_DMA;
        }

        return run_ata_dma(drive, command, req->lba, count, req, write);
    }

    uint8_t command = drive->lba48 ? ATA_CMD_READ_PIO_EXT : ATA_CMD_READ_PIO;
    pata_cursor_t cursor;

    if(write) {
        command = drive->lba48 ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_WRITE_PIO;
    }

    request_cursor(req, &cursor);

    return run_ata(drive, command, req->lba, count, interface->sector_size, &cursor, write);
}

static void pata_submit(const block_interface_t* restrict interface, block_request_t* restrict req)
{
    pata_drive_t* drive = interface->ctxdata;
    pata_cursor_t cursor;
    int retval;

    if(!channel_acquire(drive->channel)) {
        finish(req, -ENODEV);
        return;
    }

    switch(req->op) {
        case BLOCK_REQ_READ:
            retval = transfer(drive, interface, req, false);
            break;

        case BLOCK_REQ_WRITE:
            retval = transfer(drive, interface, req, true);
            break;

        default:
            request_cursor(req, &cursor);
            retval = run_ata(drive, drive->lba48 ? ATA_CMD_FLUSH_EXT : ATA_CMD_FLUSH, 0, 0, 0, &cursor, false);
            break;
    }

    mutex_release(&drive->channel->lock);

    finish(req, retval);
}

static const block_interface_ops_t pata_block_interface_ops = {
    .submit = &pata_submit,
    .release = &pata_block_release,
};

static void set_limits(block_interface_t* restrict interface, bool dma)
{
    interface->max_transfer = PATA_MAX_TRANSFER;
    interface->max_segments = PATA_MAX_SEGMENTS;
    interface->dma_limit = phys_addr(dma ? UINT32_MAX : UINTPTR_MAX); // PIO goes through the direct map
    interface->dma_align = 1;                                         // PRD entries and the data register both move words
}

static unsigned drive_index(const pata_drive_t* restrict drive)
{
    return (unsigned)(drive - drive->channel->drives);
}

static int register_ata(pata_drive_t* restrict drive, const ata_identify_t identify)
{
    pata_channel_t* channel = drive->channel;
    unsigned index = drive_index(drive);

    if(channel->ata_unit == nullptr) {
        int retval = dev_unit_alloc(&dev_prefix_ata, channel->device, &channel->ata_unit);

        if(retval < 0) {
            return retval;
        }
    }

    block_interface_t* interface = &drive->interface;

    interface->ops = &pata_block_interface_ops;
    interface->sector_count = ata_identify_sector_count(identify);
    interface->sector_size = ata_identify_logical_sector_size(identify);
    interface->flags = 0;
    interface->device = channel->device;
    interface->ctxdata = drive;

    set_limits(interface, drive->dma);

    if(interface->sector_size == 0 || interface->sector_size > PATA_MAX_TRANSFER) {
        pr_verbose("pata: %s.%u: unsupported sector size %zu", channel->device->name, index, interface->sector_size);
        return -ENOTSUP;
    }

    int retval = block_interface_add_root(interface, channel->ata_unit, index + 1);

    if(retval < 0) {
        return retval;
    }

    channel->refcount += 1;
    drive->registered = true;

    interface->flags = BLOCK_FLAG_PRESENT;
    block_event_notify(BLOCK_EVENT_MEDIA_CONNECTED, interface);

    return 0;
}

// A drive without media stays registered, to be read once it has some
static int register_atapi(pata_drive_t* restrict drive)
{
    pata_channel_t* channel = drive->channel;
    unsigned index = drive_index(drive);

    if(channel->atapi_unit == nullptr) {
        int retval = dev_unit_alloc(&dev_prefix_atapi, channel->device, &channel->atapi_unit);

        if(retval < 0) {
            return retval;
        }
    }

    atapi_interface_t* atapi = &drive->atapi;

    atapi->ops = &pata_atapi_ops;
    atapi->flags = 0;
    atapi->base.ctxdata = drive;
    atapi->base.device = channel->device;
    atapi->base.sector_count = 0;
    atapi->base.sector_size = 0;
    atapi->base.flags = 0;

    set_limits(&atapi->base, drive->dma);

    int retval = atapi_interface_add(atapi, channel->atapi_unit, index + 1);

    if(retval < 0) {
        return retval;
    }

    channel->refcount += 1;
    drive->registered = true;

    size_t sector_count;
    size_t sector_size;

    mutex_acquire(&channel->lock);
    retval = atapi_capacity(drive, &sector_count, &sector_size);
    mutex_release(&channel->lock);

    if(retval < 0) {
        pr_verbose("pata: %s.%u: no media present", channel->device->name, index);
        return 0;
    }

    if(sector_size == 0 || sector_size > PATA_MAX_TRANSFER) {
        pr_verbose("pata: %s.%u: unsupported sector size %zu", channel->device->name, index, sector_size);
        return 0;
    }

    atapi->base.sector_count = sector_count;
    atapi->base.sector_size = sector_size;
    atapi->base.flags = BLOCK_FLAG_PRESENT;
    block_event_notify(BLOCK_EVENT_MEDIA_CONNECTED, &atapi->base);

    return 0;
}

// ATA/ATAPI-6, 9.12: after a reset, the signature in the registers tells a packet device
static int detect_drive(pata_drive_t* restrict drive, ata_identify_t data)
{
    pata_channel_t* channel = drive->channel;
    int retval = select_drive(channel, drive->select);

    if(retval < 0) {
        return retval;
    }

    uint32_t signature = io_read8(&channel->command, REG_COUNT);
    signature |= (uint32_t)(io_read8(&channel->command, REG_LBA0)) << 8;
    signature |= (uint32_t)(io_read8(&channel->command, REG_LBA1)) << 16;
    signature |= (uint32_t)(io_read8(&channel->command, REG_LBA2)) << 24;

    drive->is_atapi = signature == ATA_SIGNATURE_ATAPI;

    // A packet device clears DRDY after a reset, so only an ATA device without it is missing
    uint8_t status = read_alt_status(channel);
    bool missing = status == 0xFF;
    missing = missing || (status == 0 && !drive->is_atapi);

    if(missing) {
        return -ENODEV;
    }

    return identify(drive, drive->is_atapi ? ATA_CMD_IDENTIFY_ATAPI : ATA_CMD_IDENTIFY, data);
}

// Registering reads the partition table, so the channel isn't locked then
static void probe_drive(pata_drive_t* restrict drive)
{
    pata_channel_t* channel = drive->channel;
    unsigned index = drive_index(drive);
    ata_identify_t data;

    mutex_acquire(&channel->lock);
    int retval = detect_drive(drive, data);
    mutex_release(&channel->lock);

    if(retval < 0) {
        return;
    }

    drive->lba48 = ata_identify_has_lba48(data);
    drive->dma = channel->has_busmaster && ata_identify_has_dma(data);

    char model[41];
    ata_identify_string(&data[ATA_IDENTIFY_MODEL_WORD], ATA_IDENTIFY_MODEL_NWORDS, model, sizeof(model));
    pr_verbose("pata: %s.%u: model=%s atapi=%d lba48=%d dma=%d", channel->device->name, index, model, drive->is_atapi, drive->lba48,
        drive->dma);

    if(drive->is_atapi) {
        retval = register_atapi(drive);
    }
    else {
        retval = register_ata(drive, data);
    }

    if(retval < 0) {
        pr_warning("pata: %s.%u: %s", channel->device->name, index, strerror(retval));
    }
}

static void pata_irq(unsigned number, const hart_context_t* restrict ctx, void* restrict ctxdata)
{
    pata_channel_t* channel = ctxdata;

    // Takes INTRQ down: the line is either edge-triggered or shared with the other channel
    io_read8(&channel->command, REG_STATUS);

    event_trigger(&channel->done, true);
}

static int init_channel(pata_channel_t* restrict channel)
{
    int retval = device_map_io(channel->device, 0, &channel->command);

    if(retval == 0) {
        retval = device_map_io(channel->device, 1, &channel->control);
    }

    if(retval < 0) {
        return retval;
    }

    // UNDONE: the transfer modes stay as the firmware set them, in the drives and in the controller's timing registers
    channel->has_busmaster = 0 == device_map_io(channel->device, 2, &channel->busmaster);

    // A channel with no drives floats its data lines high
    if(read_alt_status(channel) == 0xFF) {
        return -ENODEV;
    }

    retval = device_request_irq(channel->device, 0, &pata_irq, channel);

    if(retval == -EAGAIN) {
        return retval;
    }

    channel->irq = retval == 0;

    if(!channel->irq) {
        pr_verbose("pata: %s: no interrupt, commands are polled", channel->device->name);
    }

    channel->scratch_phys = pdma_alloc_phys(1);

    if(address_null(channel->scratch_phys)) {
        return -ENOMEM;
    }

    reset_channel(channel);

    return 0;
}

static int pata_probe(device_t* restrict device, const driver_match_t* restrict match)
{
    return DRIVER_SCORE_DEFAULT;
}

static int pata_attach(device_t* restrict device, const driver_match_t* restrict match)
{
    pata_channel_t* channel = kmalloc(sizeof(pata_channel_t));

    if(channel == nullptr) {
        pr_verbose("pata: out of memory");
        return -ENOMEM;
    }

    memset(channel, 0, sizeof(pata_channel_t));
    mutex_init(&channel->lock);
    event_init(&channel->done);
    channel->refcount = 1;
    channel->device = device;

    int retval = init_channel(channel);

    if(retval < 0) {
        if(address_valid(channel->scratch_phys)) {
            pdma_free_phys(channel->scratch_phys, 1);
        }

        channel_decref(channel);
        return retval;
    }

    device->ctxdata = channel;

    for(unsigned i = 0; i < PATA_DRIVES; ++i) {
        pata_drive_t* drive = &channel->drives[i];

        drive->channel = channel;
        drive->select = i ? ATA_DEVICE_SLAVE : 0;

        probe_drive(drive);
    }

    return 0;
}

static void pata_detach(device_t* restrict device)
{
    pata_channel_t* channel = device->ctxdata;

    for(unsigned i = 0; i < PATA_DRIVES; ++i) {
        pata_drive_t* drive = &channel->drives[i];

        if(!drive->registered) {
            continue;
        }

        if(drive->is_atapi) {
            atapi_interface_remove(&drive->atapi);
        }
        else {
            block_interface_remove(&drive->interface);
        }
    }

    mutex_acquire(&channel->lock);

    write_control(channel, ATA_CONTROL_NIEN);
    pdma_free_phys(channel->scratch_phys, 1);
    channel->scratch_phys = phys_addr(0);

    mutex_release(&channel->lock);
}

static void pata_release(device_t* restrict device)
{
    pata_channel_t* channel = device->ctxdata;

    if(channel) {
        free_units(channel);
        channel_decref(channel);
        device->ctxdata = nullptr;
    }
}

static const driver_ops_t pata_driver_ops = {
    .probe = &pata_probe,
    .attach = &pata_attach,
    .detach = &pata_detach,
    .release = &pata_release,
};

static const driver_match_t pata_matches[] = {
    DRIVER_MATCH_FIRMWARE("PNP0600"),
    DRIVER_MATCH_END(),
};

define_driver(pata, &pata_driver_ops, pata_matches);
