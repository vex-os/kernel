#include <vex/assert.h>
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
#include <vex/endian.h>
#include <vex/event.h>
#include <vex/hart.h>
#include <vex/hhdm.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/mutex.h>
#include <vex/page.h>
#include <vex/pci.h>
#include <vex/pdma.h>
#include <vex/sprintf.h>
#include <vex/string.h>

#define HBA_CAP  0x00 // Host Capabilities
#define HBA_GHC  0x04 // Global Host Control
#define HBA_IS   0x08 // Interrupt Status (global)
#define HBA_PI   0x0C // Ports Implemented
#define HBA_CAP2 0x24 // Host Capabilities Extended
#define HBA_BOHC 0x28 // BIOS/OS Handoff Control and Status

#define HBA_CAP_S64A UINT32_C(0x80000000) // Supports 64-bit Addressing

#define HBA_GHC_AE UINT32_C(0x80000000) // AHCI Enable
#define HBA_GHC_IE UINT32_C(0x00000002) // Interrupt Enable

#define HBA_CAP2_BOH UINT32_C(0x00000001) // BIOS/OS Handoff

#define HBA_BOHC_BOS UINT32_C(0x00000001) // BIOS Owned Semaphore
#define HBA_BOHC_OOS UINT32_C(0x00000002) // OS Owned Semaphore
#define HBA_BOHC_BB  UINT32_C(0x00000010) // BIOS Busy

#define HBA_PORT0_OFFSET 0x100
#define HBA_PORT_STRIDE  0x80

#define PORT_REG_CLB  0x00 // Command List Base Address
#define PORT_REG_CLBU 0x04 // Command List Base Address Upper 32 bits
#define PORT_REG_FB   0x08 // FIS Base Address
#define PORT_REG_FBU  0x0C // FIS Base Address Upper 32 bits
#define PORT_REG_IS   0x10 // Interrupt Status
#define PORT_REG_IE   0x14 // Interrupt Enable
#define PORT_REG_CMD  0x18 // Command and Status
#define PORT_REG_TFD  0x20 // Task File Data
#define PORT_REG_SIG  0x24 // Signature
#define PORT_REG_SSTS 0x28 // SATA Status (SCR0)
#define PORT_REG_SCTL 0x2C // SATA Control (SCR2)
#define PORT_REG_SERR 0x30 // SATA Error (SCR1)
#define PORT_REG_SACT 0x34 // SATA Active
#define PORT_REG_CI   0x38 // Command Issue

#define PORT_CMD_ST  UINT32_C(0x00000001) // Start
#define PORT_CMD_FRE UINT32_C(0x00000010) // FIS Receive Enable
#define PORT_CMD_FR  UINT32_C(0x00004000) // FIS Receive Running
#define PORT_CMD_CR  UINT32_C(0x00008000) // Command List Running

#define PORT_IS_DHRS UINT32_C(0x00000001) // Device to Host Register FIS
#define PORT_IS_PSS  UINT32_C(0x00000002) // PIO Setup FIS
#define PORT_IS_DSS  UINT32_C(0x00000004) // DMA Setup FIS
#define PORT_IS_SDBS UINT32_C(0x00000008) // Set Device Bits FIS
#define PORT_IS_IFS  UINT32_C(0x08000000) // Interface Fatal Error
#define PORT_IS_HBDS UINT32_C(0x10000000) // Host Bus Data Error
#define PORT_IS_HBFS UINT32_C(0x20000000) // Host Bus Fatal Error
#define PORT_IS_TFES UINT32_C(0x40000000) // Task File Error

#define PORT_IS_DONE   (PORT_IS_DHRS | PORT_IS_PSS | PORT_IS_DSS | PORT_IS_SDBS)
#define PORT_IS_ERRORS (PORT_IS_IFS | PORT_IS_HBDS | PORT_IS_HBFS | PORT_IS_TFES)

#define PORT_SCTL_DET_MASK  UINT32_C(0x0000000F)
#define PORT_SCTL_DET_RESET UINT32_C(0x00000001) // COMRESET

#define PORT_TFD_ERR UINT32_C(0x00000001) // Task File Error
#define PORT_TFD_DRQ UINT32_C(0x00000008) // Data Request
#define PORT_TFD_BSY UINT32_C(0x00000080) // Busy

#define PORT_SSTS_DET_PRESENT 3 // Device detected, PHY communication established
#define PORT_SSTS_IPM_ACTIVE  1 // Interface in active power state

#define CMD_HEADER_A UINT16_C(0x0020) // ATAPI
#define CMD_HEADER_W UINT16_C(0x0040) // Write

#define CMDTABLE_ACMD 0x40 // ATAPI command

#define FIS_H2D_C UINT8_C(0x80) // Command, not device control

#define ATAPI_FEATURES_DMA         UINT8_C(0x01)
#define ATAPI_CMD_READ_CAPACITY_10 0x25

#define DMA_OFFSET_CMDLIST  0x000 // [0x000..0x3FF] command list (32 slots * 32 bytes)
#define DMA_OFFSET_FIS      0x400 // [0x400..0x4FF] received FIS
#define DMA_OFFSET_CMDTABLE 0x500 // [0x500..0x57F] command table (FIS at 0x00, ATAPI CDB at 0x40)
#define DMA_OFFSET_PRDT     0x580 // [0x580..0x77F] PRDT, AHCI_MAX_PRDT entries
#define DMA_OFFSET_DATABUF  0x780 // [0x780..0x97F] identify and capacity data
#define DMA_DATABUF_SIZE    0x200
#define DMA_NUM_PAGES       1

#define AHCI_MAX_PRDT     32
#define AHCI_MAX_TRANSFER 0x10000 // Bytes per command (64 KiB)

#define AHCI_COMMAND_TIMEOUT_NS  UINT64_C(10000000000)
#define AHCI_ENGINE_TIMEOUT_NS   UINT64_C(500000000) // PxCMD.CR and PxCMD.FR, per the spec
#define AHCI_LINK_TIMEOUT_NS     UINT64_C(1000000000)
#define AHCI_BIOS_BOS_TIMEOUT_NS UINT64_C(25000000)
#define AHCI_BIOS_BB_TIMEOUT_NS  UINT64_C(2000000000)

typedef struct ahci_cmd_header_s ahci_cmd_header_t;
typedef struct ata_fis_h2d_s ata_fis_h2d_t;
typedef struct ahci_prdt_entry_s ahci_prdt_entry_t;
typedef struct ahci_pci_context_s ahci_pci_context_t;
typedef struct ahci_port_s ahci_port_t;

struct ahci_cmd_header_s {
    uint16_t flags; // CFL:5, A:1, W:1, P:1, R:1, B:1, C:1, rsvd:1, PMP:4
    uint16_t prdtl; // PRDT length in entries
    uint32_t prdbc; // PRD byte count (filled by HBA after transfer)
    uint32_t ctba;  // command table base address low
    uint32_t ctbau; // command table base address upper
    uint32_t reserved[4];
} __packed;

struct ata_fis_h2d_s {
    uint8_t type;    // ATA_FIS_TYPE_H2D
    uint8_t pm_port; // bit7 = C (new command), bits3:0 = port multiplier port
    uint8_t command;
    uint8_t features;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t features_ext;
    uint8_t count_lo;
    uint8_t count_hi;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
} __packed;

struct ahci_prdt_entry_s {
    uint32_t dba;  // data base address low
    uint32_t dbau; // data base address upper
    uint32_t reserved;
    uint32_t dbc; // bits30:0 = byte count - 1; bit31 = interrupt on completion
} __packed;

static_assert(32 == sizeof(ahci_cmd_header_t));
static_assert(20 == sizeof(ata_fis_h2d_t));
static_assert(16 == sizeof(ahci_prdt_entry_t));

static_assert(DMA_OFFSET_PRDT + AHCI_MAX_PRDT * sizeof(ahci_prdt_entry_t) <= DMA_OFFSET_DATABUF);
static_assert(DMA_OFFSET_DATABUF + DMA_DATABUF_SIZE <= DMA_NUM_PAGES * PAGE_SIZE);

struct ahci_port_s {
    mutex_t lock;        // Held for a whole command, every one of which uses slot 0 and the DMA buffer
    event_t done;        // Triggered by the interrupt handler, reset before each command
    uint32_t irq_status; // PxIS bits the interrupt handler cleared since the command was issued
    ahci_pci_context_t* context;
    const io_region_t* bar;
    uintptr_t port_base;
    phys_addr_t dma_phys;
    uint8_t* dma_virt;
    unsigned iterator;
    unsigned number;
};

struct ahci_pci_context_s {
    io_region_t abar;
    phys_addr_t dma_limit;
    bool irq;
    block_interface_t interfaces[32];
    atapi_interface_t atapi_interfaces[32];
    ahci_port_t ports[32];
    dev_unit_t* atapi_unit;
    dev_unit_t* ata_unit;
    uint32_t atapi_mask;
    uint32_t ata_mask;
    unsigned refcount;
    device_t* device;
};

static __always_inline uint32_t port_read(ahci_port_t* restrict port, uint32_t offset)
{
    return io_read32(port->bar, port->port_base + offset);
}

static __always_inline void port_write(ahci_port_t* restrict port, uint32_t offset, uint32_t value)
{
    io_write32(port->bar, port->port_base + offset, value);
}

// A port torn down by ahci_remove may still be reached
// through a block device someone holds a reference to
static __nodiscard bool port_acquire(ahci_port_t* restrict port)
{
    mutex_acquire(&port->lock);

    if(address_null(port->dma_phys)) {
        mutex_release(&port->lock);
        return false;
    }

    return true;
}

static void context_decref(ahci_pci_context_t* restrict context)
{
    if(1 == atomic_fetch_sub(&context->refcount, 1, ATOMIC_ACQ_REL)) {
        for(unsigned port_num = 0; port_num < 32; port_num += 1) {
            mutex_deinit(&context->ports[port_num].lock);
            event_deinit(&context->ports[port_num].done);
        }

        kfree(context);
    }
}

// The names stay with devices someone still
// holds, so the units can go before the context
static void free_units(ahci_pci_context_t* restrict context)
{
    if(context->ata_unit) {
        dev_unit_free(context->ata_unit);
        context->ata_unit = nullptr;
    }

    if(context->atapi_unit) {
        dev_unit_free(context->atapi_unit);
        context->atapi_unit = nullptr;
    }
}

static void ahci_block_release(const block_interface_t* restrict interface)
{
    const ahci_port_t* port = interface->ctxdata;
    context_decref(port->context);
}

static void ahci_atapi_release(const atapi_interface_t* restrict interface)
{
    const ahci_port_t* port = interface->base.ctxdata;
    context_decref(port->context);
}

static bool bar_wait(const io_region_t* restrict bar, uintptr_t offset, uint32_t mask, uint32_t value, uint64_t timeout)
{
    uint64_t deadline = clock_source_now_ns() + timeout;

    while((io_read32(bar, offset) & mask) != value) {
        if(clock_source_now_ns() >= deadline) {
            return false;
        }

        hart_spin();
    }

    return true;
}

static __always_inline bool port_wait(ahci_port_t* restrict port, uint32_t offset, uint32_t mask, uint32_t value, uint64_t timeout)
{
    return bar_wait(port->bar, port->port_base + offset, mask, value, timeout);
}

static bool ahci_stop(ahci_port_t* restrict port)
{
    port_write(port, PORT_REG_CMD, port_read(port, PORT_REG_CMD) & ~PORT_CMD_ST);

    if(!port_wait(port, PORT_REG_CMD, PORT_CMD_CR, 0, AHCI_ENGINE_TIMEOUT_NS)) {
        return false;
    }

    port_write(port, PORT_REG_CMD, port_read(port, PORT_REG_CMD) & ~PORT_CMD_FRE);

    return port_wait(port, PORT_REG_CMD, PORT_CMD_FR, 0, AHCI_ENGINE_TIMEOUT_NS);
}

static void ahci_start(ahci_port_t* restrict port)
{
    port_write(port, PORT_REG_CMD, port_read(port, PORT_REG_CMD) | PORT_CMD_FRE);
    port_write(port, PORT_REG_CMD, port_read(port, PORT_REG_CMD) | PORT_CMD_ST);
}

static void ahci_comreset(ahci_port_t* restrict port)
{
    uint32_t sctl = port_read(port, PORT_REG_SCTL) & ~PORT_SCTL_DET_MASK;

    port_write(port, PORT_REG_SCTL, sctl | PORT_SCTL_DET_RESET);
    delay_msec(1); // The spec asks for at least 1 ms of COMRESET
    port_write(port, PORT_REG_SCTL, sctl);

    if(!port_wait(port, PORT_REG_SSTS, 0x0F, PORT_SSTS_DET_PRESENT, AHCI_LINK_TIMEOUT_NS)) {
        pr_verbose("ahci: %u.%u: link did not come back after COMRESET", port->iterator, port->number);
    }
}

static void ahci_recover(ahci_port_t* restrict port)
{
    if(!ahci_stop(port)) {
        pr_verbose("ahci: %u.%u: command engine did not stop", port->iterator, port->number);
    }

    if(port_read(port, PORT_REG_TFD) & (PORT_TFD_BSY | PORT_TFD_DRQ)) {
        ahci_comreset(port);
    }

    port_write(port, PORT_REG_SERR, 0xFFFFFFFF);
    port_write(port, PORT_REG_IS, 0xFFFFFFFF);

    ahci_start(port);
}

static int ahci_wait(ahci_port_t* restrict port, uintptr_t lba)
{
    uint64_t deadline = clock_source_now_ns() + AHCI_COMMAND_TIMEOUT_NS;

    // Nothing can switch away from a caller with interrupts
    // disabled, such as kmain mounting the root during boot
    bool sleep = port->context->irq && interrupt_state();

    for(;;) {
        uint32_t status = atomic_load(&port->irq_status, ATOMIC_ACQUIRE) | port_read(port, PORT_REG_IS);

        if((status & PORT_IS_ERRORS) || (port_read(port, PORT_REG_TFD) & PORT_TFD_ERR)) {
            unsigned long tfd = port_read(port, PORT_REG_TFD);
            pr_verbose("ahci: %u.%u: command error: lba=%zu is=%08lX tfd=%08lX", port->iterator, port->number, (size_t)(lba),
                (unsigned long)(status), tfd);
            return -EIO;
        }

        if(0 == (port_read(port, PORT_REG_CI) & 1)) {
            return 0;
        }

        uint64_t now = clock_source_now_ns();

        if(now >= deadline) {
            pr_verbose("ahci: %u.%u: command timeout: lba=%zu", port->iterator, port->number, (size_t)(lba));
            return -EIO;
        }

        if(sleep) {
            event_t* done = &port->done;
            ssize_t result = event_await_timeout(&done, 1, deadline - now);
            (void)(result); // Completion is read from the registers either way
        }
        else {
            hart_spin();
        }
    }
}

static void ahci_setup_header(ahci_port_t* restrict port, uint16_t flags, const block_segment_t* restrict segments, size_t num_segments)
{
    volatile ahci_cmd_header_t* header = (volatile ahci_cmd_header_t*)(port->dma_virt + DMA_OFFSET_CMDLIST);
    volatile ahci_prdt_entry_t* prdt = (volatile ahci_prdt_entry_t*)(port->dma_virt + DMA_OFFSET_PRDT);

    phys_addr_t cmdtable_phys = address_add(port->dma_phys, DMA_OFFSET_CMDTABLE);

    kassert(num_segments <= AHCI_MAX_PRDT);

    header->flags = htole16(flags | (sizeof(ata_fis_h2d_t) / sizeof(uint32_t))); // CFL, in dwords
    header->prdtl = htole16((uint16_t)(num_segments));
    header->prdbc = 0;
    header->ctba = htole32(cmdtable_phys.value);
    header->ctbau = htole32(cmdtable_phys.value >> 32);
    header->reserved[0] = 0;
    header->reserved[1] = 0;
    header->reserved[2] = 0;
    header->reserved[3] = 0;

    for(size_t i = 0; i < num_segments; ++i) {
        prdt[i].dba = htole32(segments[i].phys.value);
        prdt[i].dbau = htole32(segments[i].phys.value >> 32);
        prdt[i].reserved = 0;
        prdt[i].dbc = htole32((uint32_t)(segments[i].length) - 1);
    }
}

static void ahci_setup(ahci_port_t* restrict port, uint64_t lba, uint8_t cmd, uint16_t count, const block_segment_t* restrict segments,
    size_t num_segments, bool write)
{
    volatile ata_fis_h2d_t* fis = (volatile ata_fis_h2d_t*)(port->dma_virt + DMA_OFFSET_CMDTABLE);

    memset((void*)(fis), 0, sizeof(ata_fis_h2d_t));

    ahci_setup_header(port, write ? CMD_HEADER_W : 0, segments, num_segments);

    fis->type = ATA_FIS_TYPE_H2D;
    fis->pm_port = FIS_H2D_C;
    fis->command = cmd;
    fis->device = ATA_DEVICE_LBA;
    fis->lba0 = lba >> 0;
    fis->lba1 = lba >> 8;
    fis->lba2 = lba >> 16;
    fis->lba3 = lba >> 24;
    fis->lba4 = lba >> 32;
    fis->lba5 = lba >> 40;
    fis->count_lo = count >> 0;
    fis->count_hi = count >> 8;
}

static int ahci_issue(ahci_port_t* restrict port, uintptr_t lba)
{
    port_write(port, PORT_REG_SERR, 0xFFFFFFFF);
    port_write(port, PORT_REG_IS, 0xFFFFFFFF);

    if(!port_wait(port, PORT_REG_TFD, PORT_TFD_BSY | PORT_TFD_DRQ, 0, AHCI_COMMAND_TIMEOUT_NS)) {
        pr_verbose("ahci: %u.%u: device stuck busy: lba=%zu", port->iterator, port->number, (size_t)(lba));
        ahci_recover(port);
        return -EIO;
    }

    atomic_store(&port->irq_status, 0, ATOMIC_RELEASE);
    event_reset(&port->done);

    port_write(port, PORT_REG_CI, 1);

    int retval = ahci_wait(port, lba);

    if(retval < 0) {
        ahci_recover(port);
    }

    return retval;
}

static int ahci_identify(ahci_port_t* restrict port, ata_identify_t identify)
{
    volatile uint8_t* databuf = (volatile uint8_t*)(port->dma_virt + DMA_OFFSET_DATABUF);

    block_segment_t segment;
    segment.phys = address_add(port->dma_phys, DMA_OFFSET_DATABUF);
    segment.length = sizeof(ata_identify_t);

    memset((void*)(databuf), 0, sizeof(ata_identify_t));

    ahci_setup(port, 0, ATA_CMD_IDENTIFY, 0, &segment, 1, false);

    int retval = ahci_issue(port, 0);

    if(retval < 0) {
        return retval;
    }

    memcpy(identify, (const void*)(databuf), sizeof(ata_identify_t));

    return 0;
}

static int ahci_atapi_identify(ahci_port_t* restrict port, ata_identify_t identify)
{
    volatile uint8_t* databuf = (volatile uint8_t*)(port->dma_virt + DMA_OFFSET_DATABUF);

    block_segment_t segment;
    segment.phys = address_add(port->dma_phys, DMA_OFFSET_DATABUF);
    segment.length = sizeof(ata_identify_t);

    memset((void*)(databuf), 0, sizeof(ata_identify_t));

    ahci_setup(port, 0, ATA_CMD_IDENTIFY_ATAPI, 0, &segment, 1, false);

    int retval = ahci_issue(port, 0);

    if(retval < 0) {
        return retval;
    }

    memcpy(identify, (const void*)(databuf), sizeof(ata_identify_t));

    return 0;
}

static void ahci_atapi_setup(ahci_port_t* restrict port, const atapi_cdb_t cdb, const block_segment_t* restrict segments,
    size_t num_segments, size_t byte_count, bool write)
{
    volatile ata_fis_h2d_t* fis = (volatile ata_fis_h2d_t*)(port->dma_virt + DMA_OFFSET_CMDTABLE);
    volatile uint8_t* atapi_area = port->dma_virt + DMA_OFFSET_CMDTABLE + CMDTABLE_ACMD;

    memset((void*)(fis), 0, sizeof(ata_fis_h2d_t));

    ahci_setup_header(port, (write ? CMD_HEADER_W : 0) | CMD_HEADER_A, segments, num_segments);

    fis->type = ATA_FIS_TYPE_H2D;
    fis->pm_port = FIS_H2D_C;
    fis->command = ATA_CMD_PACKET;
    fis->features = ATAPI_FEATURES_DMA;
    fis->lba1 = byte_count & 0xFF;
    fis->lba2 = (byte_count >> 8) & 0xFF;

    memcpy((void*)(atapi_area), cdb, sizeof(atapi_cdb_t));
}

static int ahci_atapi_capacity(ahci_port_t* restrict port, size_t* restrict sector_count, size_t* restrict sector_size)
{
    volatile uint8_t* databuf = (volatile uint8_t*)(port->dma_virt + DMA_OFFSET_DATABUF);

    atapi_cdb_t cdb;
    memset(cdb, 0, sizeof(atapi_cdb_t));
    cdb[0] = ATAPI_CMD_READ_CAPACITY_10;

    uint8_t result[8];

    block_segment_t segment;
    segment.phys = address_add(port->dma_phys, DMA_OFFSET_DATABUF);
    segment.length = sizeof(result);

    memset((void*)(databuf), 0, sizeof(result));

    ahci_atapi_setup(port, cdb, &segment, 1, sizeof(result), false);

    int retval = ahci_issue(port, 0);

    if(retval < 0) {
        return retval;
    }

    memcpy(result, (const void*)(databuf), sizeof(result));

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

static void ahci_atapi_submit(const atapi_interface_t* restrict interface, const atapi_cdb_t cdb, block_request_t* restrict req)
{
    ahci_port_t* port = interface->base.ctxdata;

    if(!port_acquire(port)) {
        finish(req, -ENODEV);
        return;
    }

    ahci_atapi_setup(port, cdb, req->segments, req->num_segments, req->length, req->op == BLOCK_REQ_WRITE);

    int retval = ahci_issue(port, req->lba);

    mutex_release(&port->lock);

    finish(req, retval);
}

static const atapi_interface_ops_t ahci_atapi_ops = {
    .submit_packet = &ahci_atapi_submit,
    .release = &ahci_atapi_release,
};

static void ahci_submit(const block_interface_t* restrict interface, block_request_t* restrict req)
{
    ahci_port_t* port = interface->ctxdata;
    uint16_t count = (uint16_t)(req->length / interface->sector_size);

    if(!port_acquire(port)) {
        finish(req, -ENODEV);
        return;
    }

    switch(req->op) {
        case BLOCK_REQ_READ:
            ahci_setup(port, req->lba, ATA_CMD_READ_DMA_EXT, count, req->segments, req->num_segments, false);
            break;

        case BLOCK_REQ_WRITE:
            ahci_setup(port, req->lba, ATA_CMD_WRITE_DMA_EXT, count, req->segments, req->num_segments, true);
            break;

        default:
            ahci_setup(port, 0, ATA_CMD_FLUSH_EXT, 0, nullptr, 0, false);
            break;
    }

    int retval = ahci_issue(port, req->lba);

    mutex_release(&port->lock);

    finish(req, retval);
}

static int ahci_port_init(ahci_pci_context_t* restrict context, const io_region_t* restrict abar, unsigned port_num)
{
    ahci_port_t* port = &context->ports[port_num];

    uintptr_t port_base = HBA_PORT0_OFFSET + port_num * HBA_PORT_STRIDE;
    phys_addr_t dma_phys = pdma_alloc_phys(DMA_NUM_PAGES);

    if(address_null(dma_phys)) {
        pr_verbose("ahci: out of memory");
        return -ENOMEM;
    }

    uint8_t* dma_virt = phys_to_virt(dma_phys);

    memset(dma_virt, 0, PAGE_SIZE * DMA_NUM_PAGES);

    mutex_init(&port->lock);
    event_init(&port->done);
    port->irq_status = 0;
    port->number = port_num;
    port->bar = abar;
    port->port_base = port_base;
    port->dma_phys = dma_phys;
    port->dma_virt = dma_virt;

    if(!ahci_stop(port)) {
        pr_verbose("ahci: %u: command engine did not stop", port_num);
        pdma_free_phys(dma_phys, DMA_NUM_PAGES);
        port->dma_phys = phys_addr(0);
        return -EIO;
    }

    phys_addr_t cmdlist_phys = address_add(dma_phys, DMA_OFFSET_CMDLIST);
    phys_addr_t fis_phys = address_add(dma_phys, DMA_OFFSET_FIS);

    io_write32(abar, port_base + PORT_REG_CLB, cmdlist_phys.value);
    io_write32(abar, port_base + PORT_REG_CLBU, cmdlist_phys.value >> 32);
    io_write32(abar, port_base + PORT_REG_FB, fis_phys.value);
    io_write32(abar, port_base + PORT_REG_FBU, fis_phys.value >> 32);

    io_write32(abar, port_base + PORT_REG_SERR, 0xFFFFFFFF);
    io_write32(abar, port_base + PORT_REG_IS, 0xFFFFFFFF);
    io_write32(abar, port_base + PORT_REG_IE, PORT_IS_DONE | PORT_IS_ERRORS);

    ahci_start(port);

    return 0;
}

static void ahci_port_teardown(ahci_port_t* restrict port)
{
    mutex_acquire(&port->lock);

    port_write(port, PORT_REG_IE, 0);

    if(!ahci_stop(port)) {
        pr_verbose("ahci: %u.%u: command engine did not stop", port->iterator, port->number);
    }

    pdma_free_phys(port->dma_phys, DMA_NUM_PAGES);

    port->dma_phys = phys_addr(0);

    mutex_release(&port->lock);
}

static const block_interface_ops_t ahci_block_interface_ops = {
    .submit = &ahci_submit,
    .release = &ahci_block_release,
};

static void set_limits(const ahci_pci_context_t* restrict context, block_interface_t* restrict interface)
{
    interface->max_transfer = AHCI_MAX_TRANSFER;
    interface->max_segments = AHCI_MAX_PRDT;
    interface->dma_limit = context->dma_limit;
    interface->dma_align = 1; // PRDT DBA and DBC both have to be even
}

static int ahci_port_register_ata(ahci_pci_context_t* restrict context, unsigned port_num)
{
    if(context->ata_unit == nullptr) {
        int retval = dev_unit_alloc(&dev_prefix_ata, context->device, &context->ata_unit);

        if(retval < 0) {
            return retval;
        }
    }

    block_interface_t* interface = &context->interfaces[port_num];
    ahci_port_t* port = &context->ports[port_num];

    port->context = context;

    interface->ops = &ahci_block_interface_ops;
    interface->sector_count = 0;
    interface->sector_size = 0;
    interface->flags = 0;
    interface->device = context->device;
    interface->ctxdata = port;

    set_limits(context, interface);

    int retval = block_interface_add_root(interface, context->ata_unit, port_num + 1);

    if(retval == 0) {
        context->refcount += 1;
    }

    return retval;
}

static int ahci_port_register_atapi(ahci_pci_context_t* restrict context, unsigned port_num)
{
    if(context->atapi_unit == nullptr) {
        int retval = dev_unit_alloc(&dev_prefix_atapi, context->device, &context->atapi_unit);

        if(retval < 0) {
            return retval;
        }
    }

    atapi_interface_t* atapi = &context->atapi_interfaces[port_num];
    ahci_port_t* port = &context->ports[port_num];

    port->context = context;

    atapi->ops = &ahci_atapi_ops;
    atapi->flags = 0;
    atapi->base.ctxdata = port;
    atapi->base.device = context->device;
    atapi->base.sector_count = 0;
    atapi->base.sector_size = 0;
    atapi->base.flags = 0;

    set_limits(context, &atapi->base);

    int retval = atapi_interface_add(atapi, context->atapi_unit, port_num + 1);

    if(retval == 0) {
        context->refcount += 1;
    }

    return retval;
}

static void ahci_port_probe_ata(ahci_pci_context_t* restrict context, const io_region_t* restrict abar, unsigned port_num)
{
    if(ahci_port_init(context, abar, port_num)) {
        return;
    }

    ata_identify_t identify;
    ahci_port_t* port = &context->ports[port_num];

    port->iterator = context->ata_unit->number;

    if(ahci_identify(port, identify)) {
        pr_verbose("ahci: %u.%u: identify failed", context->ata_unit->number, port_num);
        ahci_port_teardown(port);
        return;
    }

    if(ata_identify_is_atapi(identify)) {
        ahci_port_teardown(port);
        return;
    }

    block_interface_t* interface = &context->interfaces[port_num];
    interface->sector_count = ata_identify_sector_count(identify);
    interface->sector_size = ata_identify_logical_sector_size(identify);

    if(interface->sector_size == 0 || interface->sector_size > AHCI_MAX_TRANSFER) {
        pr_verbose("ahci: %u.%u: unsupported sector size %zu", context->ata_unit->number, port_num, interface->sector_size);
        ahci_port_teardown(port);
        return;
    }

    interface->flags = BLOCK_FLAG_PRESENT;
    interface->ctxdata = port;

    char model[41];
    ata_identify_string(&identify[ATA_IDENTIFY_MODEL_WORD], ATA_IDENTIFY_MODEL_NWORDS, model, sizeof(model));
    pr_verbose("ahci: %u.%u: model=%s sector_count=%zu sector_size=%zu", context->ata_unit->number, port_num, model,
        interface->sector_count, interface->sector_size);

    block_event_notify(BLOCK_EVENT_MEDIA_CONNECTED, interface);
}

static void ahci_port_probe_atapi(ahci_pci_context_t* restrict context, const io_region_t* restrict abar, unsigned port_num)
{
    if(ahci_port_init(context, abar, port_num)) {
        return;
    }

    ata_identify_t identify;
    ahci_port_t* port = &context->ports[port_num];
    atapi_interface_t* atapi = &context->atapi_interfaces[port_num];

    port->iterator = context->atapi_unit->number;

    char model[41];

    if(0 == ahci_atapi_identify(port, identify)) {
        ata_identify_string(&identify[ATA_IDENTIFY_MODEL_WORD], ATA_IDENTIFY_MODEL_NWORDS, model, sizeof(model));
    }
    else {
        model[0] = 0x00;
    }

    size_t sector_count;
    size_t sector_size;

    if(ahci_atapi_capacity(port, &sector_count, &sector_size)) {
        pr_verbose("ahci: %u.%u: no media present", context->atapi_unit->number, port_num);
        ahci_port_teardown(port);
        return;
    }

    if(sector_size == 0 || sector_size > AHCI_MAX_TRANSFER) {
        pr_verbose("ahci: %u.%u: unsupported sector size %zu", context->atapi_unit->number, port_num, sector_size);
        ahci_port_teardown(port);
        return;
    }

    atapi->base.sector_count = sector_count;
    atapi->base.sector_size = sector_size;
    atapi->base.flags = BLOCK_FLAG_PRESENT;
    atapi->base.ctxdata = port;

    pr_verbose("ahci: %u.%u: model=%s sector_count=%zu sector_size=%zu", context->atapi_unit->number, port_num, model, sector_count,
        sector_size);

    block_event_notify(BLOCK_EVENT_MEDIA_CONNECTED, &atapi->base);
}

static void ahci_irq(unsigned number, const hart_context_t* restrict ctx, void* restrict ctxdata)
{
    ahci_pci_context_t* context = ctxdata;
    uint32_t pending = io_read32(&context->abar, HBA_IS);

    for(unsigned port_num = 0; port_num < 32; port_num += 1) {
        if(0 == (pending & (UINT32_C(1) << port_num))) {
            continue;
        }

        ahci_port_t* port = &context->ports[port_num];
        uintptr_t port_base = HBA_PORT0_OFFSET + port_num * HBA_PORT_STRIDE;

        // PxIS goes before IS, or the port raises IS again right away
        uint32_t status = io_read32(&context->abar, port_base + PORT_REG_IS);
        io_write32(&context->abar, port_base + PORT_REG_IS, status);

        atomic_fetch_or(&port->irq_status, status, ATOMIC_RELEASE);
        event_trigger(&port->done, true);
    }

    io_write32(&context->abar, HBA_IS, pending);
}

// Firmware that drives the HBA itself (SMI-based legacy
// emulation and the like) has to let go of it first
static void ahci_handoff(const io_region_t* restrict abar)
{
    if(0 == (io_read32(abar, HBA_CAP2) & HBA_CAP2_BOH)) {
        return;
    }

    io_write32(abar, HBA_BOHC, io_read32(abar, HBA_BOHC) | HBA_BOHC_OOS);

    uint64_t deadline = clock_source_now_ns() + AHCI_BIOS_BOS_TIMEOUT_NS;
    bool busy = false;

    while((io_read32(abar, HBA_BOHC) & HBA_BOHC_BOS) && clock_source_now_ns() < deadline) {
        if(io_read32(abar, HBA_BOHC) & HBA_BOHC_BB) {
            busy = true;
            break;
        }

        hart_spin();
    }

    // Firmware that needs longer sets BB within 25 ms and then gets 2 s
    if(busy) {
        bar_wait(abar, HBA_BOHC, HBA_BOHC_BOS, 0, AHCI_BIOS_BB_TIMEOUT_NS);
    }

    if(io_read32(abar, HBA_BOHC) & HBA_BOHC_BOS) {
        pr_warning("ahci: firmware did not release the HBA, taking it anyway");
    }
}

static int ahci_probe(device_t* restrict device, const driver_match_t* restrict match)
{
    return DRIVER_SCORE_DEFAULT;
}

static int ahci_attach(device_t* restrict device, const driver_match_t* restrict match)
{
    pci_function_t* function = pci_function_of(device);
    ahci_pci_context_t* context = kmalloc(sizeof(ahci_pci_context_t));

    if(context == nullptr) {
        pr_verbose("ahci: out of memory");
        return -ENOMEM;
    }

    context->irq = false;
    context->ata_unit = nullptr;
    context->atapi_unit = nullptr;
    context->refcount = 1;
    context->device = device;

    device->ctxdata = context;

    pci_enable_busmaster(function);

    int retval = device_map_io(device, 5, &context->abar);

    if(retval < 0) {
        pr_verbose("ahci: no ABAR");

        kfree(context);

        device->ctxdata = nullptr;

        return retval;
    }

    const io_region_t* abar = &context->abar;
    context->dma_limit = phys_addr(UINT32_MAX);

    if(io_read32(abar, HBA_CAP) & HBA_CAP_S64A) {
        context->dma_limit = phys_addr(UINTPTR_MAX);
    }

    ahci_handoff(abar);

    uint32_t ahci_ghc = io_read32(abar, HBA_GHC);

    if(0 == (ahci_ghc & HBA_GHC_AE)) {
        io_write32(abar, HBA_GHC, ahci_ghc | HBA_GHC_AE);
    }

    uint32_t ahci_pi = io_read32(abar, HBA_PI);

    for(unsigned port_num = 0; port_num < 32; port_num += 1) {
        if(0 == (ahci_pi & (UINT32_C(1) << port_num))) {
            continue;
        }

        uintptr_t port_base = HBA_PORT0_OFFSET + port_num * HBA_PORT_STRIDE;
        uint32_t ahci_signature = io_read32(abar, port_base + PORT_REG_SIG);

        if(ahci_signature == ATA_SIGNATURE_ATAPI) {
            if(0 == ahci_port_register_atapi(context, port_num)) {
                context->atapi_mask |= (UINT32_C(1) << port_num);
            }
        }
        else {
            if(0 == ahci_port_register_ata(context, port_num)) {
                context->ata_mask |= (UINT32_C(1) << port_num);
            }
        }
    }

    if(0 == (context->ata_mask | context->atapi_mask)) {
        free_units(context);
        kfree(context);
        device->ctxdata = nullptr;
        return -ENODEV;
    }

    retval = device_request_irq(device, 0, &ahci_irq, context);

    if(retval == -EAGAIN) {
        return retval;
    }

    if(retval == 0) {
        context->irq = true;
        io_write32(abar, HBA_GHC, io_read32(abar, HBA_GHC) | HBA_GHC_IE);
    }
    else {
        pr_verbose("ahci: no interrupt, commands are polled");
    }

    for(unsigned port_num = 0; port_num < 32; port_num += 1) {
        uint32_t port_bit = UINT32_C(1) << port_num;

        if(0 == ((context->ata_mask | context->atapi_mask) & port_bit)) {
            continue;
        }

        uintptr_t port_base = HBA_PORT0_OFFSET + port_num * HBA_PORT_STRIDE;
        uint32_t ahci_ssts = io_read32(abar, port_base + PORT_REG_SSTS);

        if((ahci_ssts & 0x0F) != PORT_SSTS_DET_PRESENT) {
            continue;
        }

        if(((ahci_ssts >> 8) & 0x0F) != PORT_SSTS_IPM_ACTIVE) {
            continue;
        }

        if(context->atapi_mask & port_bit) {
            ahci_port_probe_atapi(context, abar, port_num);
        }
        else {
            ahci_port_probe_ata(context, abar, port_num);
        }
    }

    return 0;
}

static void ahci_detach(device_t* restrict device)
{
    ahci_pci_context_t* context = device->ctxdata;

    if(context->irq) {
        io_write32(&context->abar, HBA_GHC, io_read32(&context->abar, HBA_GHC) & ~HBA_GHC_IE);
        context->irq = false;
    }

    for(unsigned port_num = 0; port_num < 32; port_num += 1) {
        uint32_t port_bit = UINT32_C(1) << port_num;

        if(context->ata_mask & port_bit) {
            block_interface_remove(&context->interfaces[port_num]);
        }
        else if(context->atapi_mask & port_bit) {
            atapi_interface_remove(&context->atapi_interfaces[port_num]);
        }
        else {
            continue;
        }

        if(address_valid(context->ports[port_num].dma_phys)) {
            ahci_port_teardown(&context->ports[port_num]);
        }
    }
}

static void ahci_release(device_t* restrict device)
{
    ahci_pci_context_t* context = device->ctxdata;

    if(context) {
        free_units(context);
        context_decref(context);
    }
}

static const driver_ops_t ahci_driver_ops = {
    .probe = &ahci_probe,
    .attach = &ahci_attach,
    .detach = &ahci_detach,
    .release = &ahci_release,
};

static const driver_match_t ahci_matches[] = {
    DRIVER_MATCH_PCI_CLASS(0x01, 0x06),
    DRIVER_MATCH_END(),
};

define_driver(ahci, &ahci_driver_ops, ahci_matches);
