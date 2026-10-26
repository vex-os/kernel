#pragma once

#include <vex/mutex.h>

#include <vex/compiler.h>
#include <vex/endian.h>
#include <vex/path.h>
#include <vex/time.h>
#include <vex/types.h>

typedef struct block_interface_s block_interface_t;
typedef struct vnode_s vnode_t;
typedef struct vnode_ops_s vnode_ops_t;

#define FAT_TYPE_12 12
#define FAT_TYPE_16 16
#define FAT_TYPE_32 32

#define FAT_ATTR_READ_ONLY UINT8_C(0x01)
#define FAT_ATTR_HIDDEN    UINT8_C(0x02)
#define FAT_ATTR_SYSTEM    UINT8_C(0x04)
#define FAT_ATTR_VOLUME_ID UINT8_C(0x08)
#define FAT_ATTR_DIRECTORY UINT8_C(0x10)
#define FAT_ATTR_ARCHIVE   UINT8_C(0x20)
#define FAT_ATTR_LONG_NAME UINT8_C(0x0F)

#define FAT_NAME_END        UINT8_C(0x00) // DIR_Name[0] of the first free entry, all later ones are free too
#define FAT_NAME_DELETED    UINT8_C(0xE5)
#define FAT_NAME_ESCAPED_E5 UINT8_C(0x05) // Stands for a real leading 0xE5 (KANJI lead byte)

#define FAT_LFN_MAX_SEGMENTS 20

#define FAT_ROOT_NODENUMBER 0 // Other node numbers hold the LBA of their entry in the upper half

typedef struct fat_bootparams_s fat_bootparams_t;
typedef struct fat_extparams_fat1X_s fat_extparams_fat1X_t;
typedef struct fat_extparams_fat32_s fat_extparams_fat32_t;
typedef union fat_extparams_u fat_extparams_t;
typedef struct fat_fsinfo_fat32_s fat_fsinfo_fat32_t;
typedef struct fat_dir_entry_s fat_dir_entry_t;
typedef struct fat_timestamp_s fat_timestamp_t;
typedef struct fat_ldir_entry_s fat_ldir_entry_t;
typedef union fat_entry_u fat_entry_t;
typedef struct fat_dirent_s fat_dirent_t;
typedef struct fat_vfs_context_s fat_vfs_context_t;
typedef struct fat_vnode_context_s fat_vnode_context_t;

struct fat_bootparams_s {
    uint8_t jmp_boot[3];        // BS_jmpBoot
    uint8_t oem_name[8];        // BS_OEMName
    uint16_t sector_size;       // BPB_BytsPerSec
    uint8_t cluster_size;       // BPB_SecPerClus
    uint16_t reserved_sectors;  // BPB_RsvdSecCnt
    uint8_t tables_count;       // BPB_NumFATs
    uint16_t root_entries;      // BPB_RootEntCnt
    uint16_t total_sectors_16;  // BPB_TotSec16
    uint8_t media_descriptor;   // BPB_Media
    uint16_t table_size_16;     // BPB_FATSz16
    uint16_t sectors_per_track; // BPB_SecPerTrk
    uint16_t heads_count;       // BPB_NumHeads
    uint32_t hidden_sectors;    // BPB_HiddSec
    uint32_t total_sectors_32;  // BPB_TotSec32
} __packed;

#ifndef __INTELLISENSE__
static_assert(36 == sizeof(fat_bootparams_t));
#endif

struct fat_extparams_fat1X_s {
    fat_bootparams_t base;
    uint8_t drive_number;     // BS_DrvNum
    uint8_t reserved_1;       // BS_Reserved1
    uint8_t boot_signature;   // BS_BootSig
    uint32_t volume_id;       // BS_VolID
    uint8_t volume_label[11]; // BS_VolLab
    uint8_t fs_type[8];       // BS_FilSysType
    uint8_t boot_code[448];
    uint8_t mbr_signature[2];
} __packed;

struct fat_extparams_fat32_s {
    fat_bootparams_t base;
    uint32_t table_size_32;     // BPB_FATSz32
    uint16_t flags;             // BPB_ExtFlags
    uint16_t version;           // BPB_FSVer
    uint32_t root_cluster;      // BPB_RootClus
    uint16_t fs_info;           // BPB_FSInfo
    uint16_t backup_bootsector; // BPB_BkBootSec
    uint8_t reserved[12];       // BPB_Reserved
    uint8_t drive_number;       // BS_DrvNum
    uint8_t reserved_1;         // BS_Reserved1
    uint8_t boot_signature;     // BS_BootSig
    uint32_t volume_id;         // BS_VolID
    uint8_t volume_label[11];   // BS_VolLab
    uint8_t fs_type[8];         // BS_FilSysType
    uint8_t boot_code[420];
    uint8_t mbr_signature[2];
} __packed;

#ifndef __INTELLISENSE__
static_assert(512 == sizeof(fat_extparams_fat1X_t));
static_assert(512 == sizeof(fat_extparams_fat32_t));
#endif

union fat_extparams_u {
    fat_extparams_fat1X_t fat1X;
    fat_extparams_fat32_t fat32;
};

struct fat_fsinfo_fat32_s {
    uint32_t lead_signature;   // FSI_LeadSig
    uint8_t reserved_1[480];   // FSI_Reserved1
    uint32_t struct_signature; // FSI_StrucSig
    uint32_t free_count;       // FSI_Free_Count
    uint32_t next_free;        // FSI_Nxt_Free
    uint8_t reserved_2[12];    // FSI_Reserved2
    uint32_t trail_signature;  // FSI_TrailSig
} __packed;

#ifndef __INTELLISENSE__
static_assert(512 == sizeof(fat_fsinfo_fat32_t));
#endif

struct fat_dir_entry_s {
    uint8_t name[11];          // DIR_Name
    uint8_t attribute;         // DIR_Attr
    uint8_t nt_reserved;       // DIR_NTRes
    uint8_t create_time_tenth; // DIR_CrtTimeTenth
    uint16_t create_time;      // DIR_CrtTime
    uint16_t create_date;      // DIR_CrtDate
    uint16_t access_date;      // DIR_LstAccDate
    uint16_t first_cluster_hi; // DIR_FstClusHI
    uint16_t write_time;       // DIR_WrtTime
    uint16_t write_date;       // DIR_WrtDate
    uint16_t first_cluster_lo; // DIR_FstClusLO
    uint32_t file_size;        // DIR_FileSize
} __packed;

struct fat_timestamp_s {
    uint16_t date; // Years since 1980, month and day
    uint16_t time; // Hours, minutes and seconds halved
    uint8_t tenth; // The odd second and its hundredths, 0 to 199
};

struct fat_ldir_entry_s {
    uint8_t order;             // LDIR_Ord
    uint16_t name_1[5];        // LDIR_Name1
    uint8_t attr;              // LDIR_Attr
    uint8_t type;              // LDIR_Type
    uint8_t checksum;          // LDIR_Chksum
    uint16_t name_2[6];        // LDIR_Name2
    uint16_t first_cluster_lo; // LDIR_FstClusLO
    uint16_t name_3[2];        // LDIR_Name3
} __packed;

#ifndef __INTELLISENSE__
static_assert(32 == sizeof(fat_dir_entry_t));
static_assert(32 == sizeof(fat_ldir_entry_t));
#endif

union fat_entry_u {
    fat_dir_entry_t dir;
    fat_ldir_entry_t ldir;
};

struct fat_dirent_s {
    fat_dir_entry_t base;
    char full_name[NAME_SIZE];
    unsigned num_lfn; // Long name entries right before base
};

struct fat_vfs_context_s {
    mutex_t lock; // Serializes changes to the table and directories, and the file cache calls for files
    const block_interface_t* volume;
    fat_extparams_t extparams;
    unsigned filesystem_type;

    gid_t param_gid;
    uid_t param_uid;
    bool readonly;
    bool param_quiet;

    uintptr_t table_start;
    uintptr_t data_start;
    uintptr_t root_cluster;
    size_t rootdir_sectors;
    size_t cluster_size_s;
    size_t cluster_size_b;
    size_t sector_size;

    size_t table_size_s;
    unsigned num_tables;
    uintptr_t fsinfo_lba;
    size_t cluster_count;

    bool fsinfo_invalidated;

    uintptr_t next_free_hint;

    vnode_t* device_node;
};

struct fat_vnode_context_s {
    fat_vfs_context_t* fs_ctx;
    uintptr_t first_cluster;
    size_t size_bytes;
    uintptr_t dir_cluster;
    size_t dir_offset;
    uintptr_t cached_cluster;
    size_t cached_cluster_idx;
    bool unlinked; // The entry at dir_offset is gone, clusters are freed on release
};

extern const vnode_ops_t fat_vnode_ops;

void fat_cluster_link(fat_vfs_context_t* restrict context, uintptr_t cluster, uintptr_t next);
void fat_cluster_end(fat_vfs_context_t* restrict context, uintptr_t cluster);
void fat_cluster_free(fat_vfs_context_t* restrict context, uintptr_t cluster);

uintptr_t fat_cluster_next(const fat_vfs_context_t* restrict context, uintptr_t cluster) __nodiscard;
uintptr_t fat_cluster_alloc(fat_vfs_context_t* restrict context) __nodiscard;
uintptr_t fat_cluster_alloc_zeroed(fat_vfs_context_t* restrict context) __nodiscard;
uintptr_t fat_cluster_alloc_dir(fat_vfs_context_t* restrict context) __nodiscard;
size_t fat_cluster_count_free(const fat_vfs_context_t* restrict context) __nodiscard;

// Leaves offset one past the short entry it returns
int fat_dirent_read(const fat_vfs_context_t* restrict context, uintptr_t cluster, size_t* restrict offset, fat_dirent_t* restrict entry);
int fat_dirent_write(const fat_vfs_context_t* restrict context, uintptr_t cluster, size_t offset, const fat_dirent_t* restrict entry);
int fat_dirent_update(const fat_vfs_context_t* restrict context, uintptr_t cluster, size_t offset, const fat_dir_entry_t* restrict entry);
int fat_dirent_alloc(fat_vfs_context_t* restrict context, uintptr_t cluster, unsigned count, size_t* restrict out_offset);
int fat_dirent_remove(const fat_vfs_context_t* restrict context, uintptr_t cluster, size_t offset, unsigned num_lfn);

void fat_time_pack(const timespec_t* restrict time, fat_timestamp_t* restrict out);
void fat_time_unpack(const fat_timestamp_t* restrict stamp, timespec_t* restrict out);

// Clusters 0 and 1 are reserved, data clusters are numbered from 2
static __always_inline bool fat_cluster_valid(const fat_vfs_context_t* restrict context, uintptr_t cluster)
{
    return cluster >= 2 && cluster < context->cluster_count + 2;
}

static __always_inline uintptr_t fat_cluster_lba(const fat_vfs_context_t* restrict context, uintptr_t cluster)
{
    return context->data_start + (cluster - 2) * context->cluster_size_s;
}

static __always_inline uintptr_t fat_dirent_cluster(const fat_dir_entry_t* restrict entry)
{
    uintptr_t cluster = 0;
    cluster |= (uintptr_t)(le16toh(entry->first_cluster_hi)) << 16;
    cluster |= (uintptr_t)(le16toh(entry->first_cluster_lo));
    return cluster;
}
