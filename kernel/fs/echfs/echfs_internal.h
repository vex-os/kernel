#pragma once

#include <vex/compiler.h>
#include <vex/mutex.h>
#include <vex/path.h>
#include <vex/types.h>

typedef struct block_interface_s block_interface_t;
typedef struct vnode_s vnode_t;
typedef struct vnode_ops_s vnode_ops_t;

#define ECHFS_TABLE_FREE     UINT64_C(0x0000000000000000)
#define ECHFS_TABLE_RESERVED UINT64_C(0xFFFFFFFFFFFFFFF0)
#define ECHFS_TABLE_EOC      UINT64_C(0xFFFFFFFFFFFFFFFF)

#define ECHFS_DIR_ID_END     UINT64_C(0x00000000000000000)
#define ECHFS_DIR_ID_DELETED UINT64_C(0xFFFFFFFFFFFFFFFE)
#define ECHFS_DIR_ID_ROOT    UINT64_C(0xFFFFFFFFFFFFFFFF)

#define ECHFS_NAME_SIZE 201

#define ECHFS_ROOT_NODENUMBER UINT64_MAX // Other node numbers are indices into the directory table

typedef struct echfs_identity_s echfs_identity_t;
typedef struct echfs_dirent_s echfs_dirent_t;
typedef struct echfs_vfs_context_s echfs_vfs_context_t;
typedef struct echfs_vnode_context_s echfs_vnode_context_t;

struct echfs_identity_s {
    uint8_t reserved_1[4];
    uint8_t signature[8];
    uint64_t total_blocks;
    uint64_t maindir_size;
    uint64_t block_size;
    uint32_t reserved_2;
    uint8_t part_uuid[16];
    uint8_t reserved_3[454];
    uint8_t mbr_signature[2];
} __packed;

#ifndef __INTELLISENSE__
static_assert(512 == sizeof(echfs_identity_t));
#endif

struct echfs_dirent_s {
    uint64_t directory_id;
    uint8_t object_type;
    uint8_t name[ECHFS_NAME_SIZE];
    uint64_t access_time;
    uint64_t modify_time;
    uint16_t permissions;
    uint16_t owner_uid;
    uint16_t owner_gid;
    uint64_t change_time; // unix ctime
    uint64_t start_block; // or directory_id for directories
    uint64_t file_size;
} __packed;

#ifndef __INTELLISENSE__
static_assert(256 == sizeof(echfs_dirent_t));
#endif

struct echfs_vfs_context_s {
    const block_interface_t* volume;

    uint64_t block_size;
    uint64_t sectors_per_block;
    uint64_t total_blocks;
    uint64_t fat_start;
    uint64_t fat_size;
    uint64_t dir_start;
    uint64_t dir_size;
    uint64_t data_start;

    uint64_t dir_entries;
    uint64_t next_free_hint;

    vnode_t* device_node;
    mutex_t lock; // Serializes changes to the table and the directory, and the file cache calls for files
};

struct echfs_vnode_context_s {
    echfs_vfs_context_t* fs_ctx;
    uint64_t dir_entry;   // Index into dir_table; UINT64_MAX for the root
    uint64_t start_block; // First data block (files) or directory ID (dirs)
    uint64_t size;
    uint64_t cached_block;
    uint64_t cached_block_idx;
    bool unlinked; // The entry at dir_entry is gone, blocks are freed on release
};

extern const vnode_ops_t echfs_vnode_ops;

/// @return The block's allocation table entry, or ECHFS_TABLE_RESERVED when the block is past
/// the end of the volume or its entry can't be read, which ends any chain through it
uint64_t echfs_block_entry(const echfs_vfs_context_t* restrict context, uintptr_t block) __nodiscard;

uintptr_t echfs_block_alloc(echfs_vfs_context_t* restrict context) __nodiscard;
void echfs_block_free(echfs_vfs_context_t* restrict context, uintptr_t block);
void echfs_block_link(echfs_vfs_context_t* restrict context, uintptr_t block, uintptr_t next);
uintptr_t echfs_block_next(const echfs_vfs_context_t* restrict context, uintptr_t block) __nodiscard;

int echfs_dirent_find(const echfs_vfs_context_t* restrict context, uint64_t parent_id, const char* restrict name,
    uint64_t* restrict out_index);
int echfs_dirent_read(const echfs_vfs_context_t* restrict context, uint64_t index, echfs_dirent_t* restrict out);
int echfs_dirent_write(echfs_vfs_context_t* restrict context, uint64_t index, const echfs_dirent_t* restrict entry);
int echfs_dirent_alloc(echfs_vfs_context_t* restrict context, uint64_t* restrict out_index) __nodiscard;

static __always_inline uintptr_t echfs_block_lba(const echfs_vfs_context_t* restrict context, uintptr_t block)
{
    return block * context->sectors_per_block;
}
