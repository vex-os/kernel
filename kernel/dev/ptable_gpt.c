#include <vex/block_interface.h>
#include <vex/block_ptable.h>
#include <vex/compiler.h>
#include <vex/crc32_iso3309.h>
#include <vex/endian.h>
#include <vex/errno.h>
#include <vex/initcall.h>
#include <vex/kmalloc.h>
#include <vex/string.h>

#define PMBR_SIGNATURE_0 UINT8_C(0x55)
#define PMBR_SIGNATURE_1 UINT8_C(0xAA)
#define PMBR_ENTRY_COUNT 4

// Upper bound on the partition entry array;
// the specification asks for at least 16 KiB
// and anything far beyond that is corrupted or hostile
#define GPT_ENTRIES_MAX_SIZE (1024 * 1024)

typedef struct pmbr_entry_s pmbr_entry_t;
typedef struct pmbr_sector_s pmbr_sector_t;
typedef struct gpt_header_s gpt_header_t;
typedef struct gpt_entry_s gpt_entry_t;
typedef struct gpt_ptdata_s gpt_ptdata_t;

struct pmbr_entry_s {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_start;
    uint32_t lba_count;
} __packed;

struct pmbr_sector_s {
    uint8_t bootstrap[440];
    uint8_t disk_signature[4];
    uint8_t reserved[2];
    pmbr_entry_t entries[PMBR_ENTRY_COUNT];
    uint8_t signature[2];
} __packed;

#ifndef __INTELLISENSE__
static_assert(16 == sizeof(pmbr_entry_t));
static_assert(512 == sizeof(pmbr_sector_t));
#endif

struct gpt_header_s {
    uint8_t signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t entry_start_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entry_crc32;
} __packed;

struct gpt_entry_s {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t lba_start;
    uint64_t lba_end;
    uint64_t attributes;
    uint16_t part_name[0];
} __packed;

#ifndef __INTELLISENSE__
static_assert(92 == sizeof(gpt_header_t));
static_assert(56 == sizeof(gpt_entry_t));
#endif

struct gpt_ptdata_s {
    gpt_header_t header;
    uint8_t cached_entries[0];
};

static int gpt_check_header(const block_interface_t* restrict interface, uintptr_t header_lba, gpt_ptdata_t** restrict ptdata)
{
    uint8_t* sector = kmalloc(interface->sector_size);

    if(sector == nullptr) {
        return -ENOMEM;
    }

    int retval = block_read(interface, header_lba, 1, sector);

    if(retval < 0) {
        kfree(sector);
        return retval;
    }

    gpt_header_t header;
    memcpy(&header, sector, sizeof(gpt_header_t));

    uint32_t header_size = le32toh(header.header_size);

    if(memcmp(header.signature, "EFI PART", 8) || header_size < sizeof(gpt_header_t) || header_size > interface->sector_size) {
        kfree(sector);
        return -EINVAL;
    }

    // The checksum covers header_size bytes with its own field zeroed,
    // which may extend past the fields this structure knows about
    uint32_t checksum = le32toh(header.header_crc32);
    memset(sector + offsetof(gpt_header_t, header_crc32), 0, sizeof(uint32_t));
    uint32_t computed = crc32_iso3309_compute(sector, header_size);

    kfree(sector);

    if(checksum != computed) {
        return -EINVAL;
    }

    uint32_t entry_count = le32toh(header.entry_count);
    uint32_t entry_size = le32toh(header.entry_size);
    uint64_t entry_start_lba = le64toh(header.entry_start_lba);

    if(entry_size < sizeof(gpt_entry_t) || (entry_size % 8)) {
        return -EINVAL;
    }

    if(entry_count == 0 || entry_count > GPT_ENTRIES_MAX_SIZE / entry_size) {
        return -EINVAL;
    }

    size_t entries_size = (size_t)(entry_count) * (entry_size); // clang-format antics
    size_t entries_lbas = (entries_size + interface->sector_size - 1) / interface->sector_size;

    if(entry_start_lba >= interface->sector_count || entries_lbas > interface->sector_count - entry_start_lba) {
        return -EINVAL;
    }

    // Whole sectors are read, so the buffer is rounded up to them
    gpt_ptdata_t* gpt = kmalloc(entries_lbas * interface->sector_size + sizeof(gpt_ptdata_t));

    if(gpt == nullptr) {
        return -ENOMEM;
    }

    retval = block_read(interface, entry_start_lba, entries_lbas, gpt->cached_entries);

    if(retval < 0) {
        kfree(gpt);
        return retval;
    }

    if(le32toh(header.entry_crc32) != crc32_iso3309_compute(gpt->cached_entries, entries_size)) {
        kfree(gpt);
        return -EINVAL;
    }

    memcpy(&gpt->header, &header, sizeof(gpt_header_t));

    *ptdata = gpt;

    return 0;
}

static int gpt_detect(const block_interface_t* restrict interface, void** restrict ptdata)
{
    pmbr_sector_t sector;
    int retval = block_read_sector_prefix(interface, 0, &sector, sizeof(sector));

    if(retval < 0) {
        return retval;
    }

    bool is_valid = true;
    is_valid = is_valid && sector.signature[0] == PMBR_SIGNATURE_0;
    is_valid = is_valid && sector.signature[1] == PMBR_SIGNATURE_1;
    is_valid = is_valid && sector.entries[0].type == 0xEE;
    is_valid = is_valid && sector.entries[1].type == 0x00;
    is_valid = is_valid && sector.entries[2].type == 0x00;
    is_valid = is_valid && sector.entries[3].type == 0x00;

    if(!is_valid) {
        return -EINVAL;
    }

    gpt_ptdata_t* gpt;
    uint32_t first_lba = le32toh(sector.entries[0].lba_start);
    retval = gpt_check_header(interface, first_lba, &gpt);

    if(retval < 0) {
        retval = gpt_check_header(interface, interface->sector_count - 1, &gpt);
    }

    if(retval < 0) {
        return retval;
    }

    *ptdata = gpt;

    return 0;
}

static int gpt_read_table(const block_interface_t* restrict interface, void* restrict ptdata, unsigned index,
    block_partition_table_info_t* restrict out)
{
    static const uint8_t null_guid[16] = {}; // FIXME: a GUID library would be nice

    const gpt_ptdata_t* gpt = ptdata;
    uint32_t entry_count = le32toh(gpt->header.entry_count);
    uint32_t entry_size = le32toh(gpt->header.entry_size);

    unsigned found = 0;

    for(unsigned slot = 0; slot < entry_count; slot += 1) {
        const gpt_entry_t* entry = (const gpt_entry_t*)(gpt->cached_entries + entry_size * slot);

        if(0 == memcmp(entry->type_guid, null_guid, 16)) {
            continue;
        }

        uint64_t lba_start = le64toh(entry->lba_start);
        uint64_t lba_end = le64toh(entry->lba_end);

        if(lba_end < lba_start || lba_end >= interface->sector_count) {
            continue;
        }

        if(found == index) {
            const uint16_t* name = (const uint16_t*)((const uint8_t*)(entry) + sizeof(gpt_entry_t));
            size_t name_len = (entry_size - sizeof(gpt_entry_t)) / sizeof(uint16_t);

            unsigned i;

            for(i = 0; i < name_len && i < 63 && name[i]; ++i) {
                out->part_label[i] = le16toh(name[i]) & 0x7F;
            }

            out->part_label[i] = 0x00;
            out->part_index = slot;
            out->lba_start = lba_start;
            out->lba_count = lba_end - lba_start + 1;
            out->type = 0; // FIXME: figure out a way to shove a GUID into an uint64_t

            return 0;
        }

        found += 1;
    }

    return -ENOENT;
}

static void gpt_release(void* ptdata)
{
    kfree(ptdata);
}

static const block_partition_table_ops_t gpt_ptable_ops = {
    .detect = &gpt_detect,
    .read_table = &gpt_read_table,
    .release = &gpt_release,
};

static block_partition_table_t gpt_ptable = {
    .ops = &gpt_ptable_ops,
    .name = "gpt",
};

static void init_ptable_gpt(void)
{
    block_partition_table_add(&gpt_ptable);
}

initcall_bsp_dev(init_ptable_gpt, INIT_FIRST);
