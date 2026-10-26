#pragma once

#include <vex/compiler.h>
#include <vex/endian.h>
#include <vex/limits.h>
#include <vex/mutex.h>
#include <vex/stat.h>
#include <vex/time.h>
#include <vex/types.h>
#include <vex/xattr.h>

typedef struct block_interface_s block_interface_t;
typedef struct fcache_extent_s fcache_extent_t;
typedef struct vnode_s vnode_t;
typedef struct vnode_ops_s vnode_ops_t;

#define EXT2_SUPER_OFFSET 1024 // Bytes from the start of the partition, whatever the block size
#define EXT2_SUPER_MAGIC  UINT16_C(0xEF53)

#define EXT2_MIN_BLOCK_LOG_SIZE 10
#define EXT2_MAX_BLOCK_LOG_SIZE 16

#define EXT2_VALID_FS UINT16_C(0x0001)
#define EXT2_ERROR_FS UINT16_C(0x0002)

#define EXT2_ERRORS_CONTINUE UINT16_C(0x0001)
#define EXT2_ERRORS_RO       UINT16_C(0x0002)
#define EXT2_ERRORS_PANIC    UINT16_C(0x0003)

#define EXT2_OS_LINUX   UINT32_C(0x00000000)
#define EXT2_OS_HURD    UINT32_C(0x00000001)
#define EXT2_OS_MASIX   UINT32_C(0x00000002)
#define EXT2_OS_FREEBSD UINT32_C(0x00000003)
#define EXT2_OS_LITES   UINT32_C(0x00000004)

#define EXT2_GOOD_OLD_REV UINT32_C(0x00000000)
#define EXT2_DYNAMIC_REV  UINT32_C(0x00000001)

#define EXT2_GOOD_OLD_FIRST_INO  11
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_EXTRA_ISIZE         32 // Bytes of ext2_inode_t past the good old size

#define EXT2_FEATURE_COMPAT_DIR_PREALLOC  UINT32_C(0x00000001)
#define EXT2_FEATURE_COMPAT_IMAGIC_INODES UINT32_C(0x00000002)
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL   UINT32_C(0x00000004)
#define EXT2_FEATURE_COMPAT_EXT_ATTR      UINT32_C(0x00000008)
#define EXT2_FEATURE_COMPAT_RESIZE_INO    UINT32_C(0x00000010)
#define EXT2_FEATURE_COMPAT_DIR_INDEX     UINT32_C(0x00000020)

#define EXT2_FEATURE_INCOMPAT_COMPRESSION UINT32_C(0x00000001)
#define EXT2_FEATURE_INCOMPAT_FILETYPE    UINT32_C(0x00000002)
#define EXT2_FEATURE_INCOMPAT_RECOVER     UINT32_C(0x00000004)
#define EXT2_FEATURE_INCOMPAT_JOURNAL_DEV UINT32_C(0x00000008)
#define EXT2_FEATURE_INCOMPAT_META_BG     UINT32_C(0x00000010)

#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER UINT32_C(0x00000001)
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE   UINT32_C(0x00000002)
#define EXT2_FEATURE_RO_COMPAT_BTREE_DIR    UINT32_C(0x00000004)

// An incompat feature outside the mask refuses the mount,
// a ro_compat one outside it mounts the filesystem read-only
#define EXT2_FEATURE_INCOMPAT_SUPP  (EXT2_FEATURE_INCOMPAT_FILETYPE)
#define EXT2_FEATURE_RO_COMPAT_SUPP (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT2_FEATURE_RO_COMPAT_LARGE_FILE)

#define EXT2_LZV1_ALG   UINT32_C(0x00000001)
#define EXT2_LZRW3A_ALG UINT32_C(0x00000002)
#define EXT2_GZIP_ALG   UINT32_C(0x00000004)
#define EXT2_BZIP2_ALG  UINT32_C(0x00000008)
#define EXT2_LZO_ALG    UINT32_C(0x00000010)

#define EXT2_BAD_INO         1
#define EXT2_ROOT_INO        2
#define EXT2_ACL_IDX_INO     3
#define EXT2_ACL_DATA_INO    4
#define EXT2_BOOT_LOADER_INO 5
#define EXT2_UNDEL_DIR_INO   6
#define EXT2_RESIZE_INO      7
#define EXT2_JOURNAL_INO     8

#define EXT2_S_IFSOCK UINT16_C(0xC000)
#define EXT2_S_IFLNK  UINT16_C(0xA000)
#define EXT2_S_IFREG  UINT16_C(0x8000)
#define EXT2_S_IFBLK  UINT16_C(0x6000)
#define EXT2_S_IFDIR  UINT16_C(0x4000)
#define EXT2_S_IFCHR  UINT16_C(0x2000)
#define EXT2_S_IFIFO  UINT16_C(0x1000)
#define EXT2_S_ISUID  UINT16_C(0x0800)
#define EXT2_S_ISGID  UINT16_C(0x0400)
#define EXT2_S_ISVTX  UINT16_C(0x0200)
#define EXT2_S_IRUSR  UINT16_C(0x0100)
#define EXT2_S_IWUSR  UINT16_C(0x0080)
#define EXT2_S_IXUSR  UINT16_C(0x0040)
#define EXT2_S_IRGRP  UINT16_C(0x0020)
#define EXT2_S_IWGRP  UINT16_C(0x0010)
#define EXT2_S_IXGRP  UINT16_C(0x0008)
#define EXT2_S_IROTH  UINT16_C(0x0004)
#define EXT2_S_IWOTH  UINT16_C(0x0002)
#define EXT2_S_IXOTH  UINT16_C(0x0001)

#ifndef __INTELLISENSE__
static_assert(S_IFSOCK == EXT2_S_IFSOCK);
static_assert(S_IFLNK == EXT2_S_IFLNK);
static_assert(S_IFREG == EXT2_S_IFREG);
static_assert(S_IFBLK == EXT2_S_IFBLK);
static_assert(S_IFDIR == EXT2_S_IFDIR);
static_assert(S_IFCHR == EXT2_S_IFCHR);
static_assert(S_IFIFO == EXT2_S_IFIFO);
static_assert(S_ISUID == EXT2_S_ISUID);
static_assert(S_ISGID == EXT2_S_ISGID);
static_assert(S_ISVTX == EXT2_S_ISVTX);
static_assert(S_IRUSR == EXT2_S_IRUSR);
static_assert(S_IWUSR == EXT2_S_IWUSR);
static_assert(S_IXUSR == EXT2_S_IXUSR);
static_assert(S_IRGRP == EXT2_S_IRGRP);
static_assert(S_IWGRP == EXT2_S_IWGRP);
static_assert(S_IXGRP == EXT2_S_IXGRP);
static_assert(S_IROTH == EXT2_S_IROTH);
static_assert(S_IWOTH == EXT2_S_IWOTH);
static_assert(S_IXOTH == EXT2_S_IXOTH);
#endif

#define EXT2_SECRM_FL        UINT32_C(0x00000001)
#define EXT2_UNRM_FL         UINT32_C(0x00000002)
#define EXT2_COMPR_FL        UINT32_C(0x00000004)
#define EXT2_SYNC_FL         UINT32_C(0x00000008)
#define EXT2_IMMUTABLE_FL    UINT32_C(0x00000010)
#define EXT2_APPEND_FL       UINT32_C(0x00000020)
#define EXT2_NODUMP_FL       UINT32_C(0x00000040)
#define EXT2_NOATIME_FL      UINT32_C(0x00000080)
#define EXT2_DIRTY_FL        UINT32_C(0x00000100)
#define EXT2_COMPRBLK_FL     UINT32_C(0x00000200)
#define EXT2_NOCOMPR_FL      UINT32_C(0x00000400)
#define EXT2_ECOMPR_FL       UINT32_C(0x00000800)
#define EXT2_BTREE_FL        UINT32_C(0x00001000)
#define EXT2_INDEX_FL        UINT32_C(0x00001000) // Cleared on every change to the directory, the htree isn't kept up to date
#define EXT2_IMAGIC_FL       UINT32_C(0x00002000)
#define EXT2_JOURNAL_DATA_FL UINT32_C(0x00004000)
#define EXT2_RESERVED_FL     UINT32_C(0x80000000)

#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK   12
#define EXT2_DIND_BLOCK  13
#define EXT2_TIND_BLOCK  14
#define EXT2_N_BLOCKS    15

// A time's extra field holds the nanoseconds above two epoch bits that
// extend the signed 32-bit seconds; without it the range ends in 2038
#define EXT2_EPOCH_BITS     2
#define EXT2_EPOCH_MASK     ((UINT32_C(1) << EXT2_EPOCH_BITS) - 1)
#define EXT2_TIME_MIN       ((int64_t)(INT32_MIN))
#define EXT2_TIME_MAX       ((int64_t)(INT32_MAX))
#define EXT2_EXTRA_TIME_MAX (EXT2_TIME_MIN + (INT64_C(1) << (32 + EXT2_EPOCH_BITS)) - 1)

#define EXT2_ATIME  0
#define EXT2_CTIME  1
#define EXT2_MTIME  2
#define EXT2_CRTIME 3

#define EXT2_NAME_LEN 255
#define EXT2_LINK_MAX 32000

#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

#define EXT2_XATTR_MAGIC UINT32_C(0xEA020000)

#define EXT2_XATTR_INDEX_USER              1
#define EXT2_XATTR_INDEX_POSIX_ACL_ACCESS  2
#define EXT2_XATTR_INDEX_POSIX_ACL_DEFAULT 3
#define EXT2_XATTR_INDEX_TRUSTED           4
#define EXT2_XATTR_INDEX_SECURITY          6

typedef struct ext2_superblock_s ext2_superblock_t;
typedef struct ext2_group_desc_s ext2_group_desc_t;
typedef union ext2_osd1_u ext2_osd1_t;
typedef struct ext2_osd2_linux_s ext2_osd2_linux_t;
typedef struct ext2_osd2_hurd_s ext2_osd2_hurd_t;
typedef struct ext2_osd2_masix_s ext2_osd2_masix_t;
typedef union ext2_osd2_u ext2_osd2_t;
typedef struct ext2_inode_s ext2_inode_t;
typedef struct ext2_dir_entry_s ext2_dir_entry_t;
typedef struct ext2_xattr_header_s ext2_xattr_header_t;
typedef struct ext2_xattr_entry_s ext2_xattr_entry_t;
typedef struct ext2_dirent_s ext2_dirent_t;
typedef struct ext2_vfs_context_s ext2_vfs_context_t;
typedef struct ext2_vnode_context_s ext2_vnode_context_t;

struct ext2_superblock_s {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino; // From here on, valid only with EXT2_DYNAMIC_REV
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t s_uuid[16];
    char s_volume_name[16]; // Not NUL-terminated when all 16 bytes are used
    char s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t s_prealloc_blocks;
    uint8_t s_prealloc_dir_blocks;
    uint16_t s_padding_1;
    uint8_t s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t s_def_hash_version;
    uint8_t s_padding_2[3];
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint8_t s_reserved[760];
} __packed;

struct ext2_group_desc_s {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t bg_reserved[12];
} __packed;

#ifndef __INTELLISENSE__
static_assert(1024 == sizeof(ext2_superblock_t));
static_assert(32 == sizeof(ext2_group_desc_t));
#endif

// Which variant of the OS-dependent inode
// fields is used depends on s_creator_os;
// FreeBSD and Lites use the Linux one
union ext2_osd1_u {
    uint32_t l_i_reserved1;
    uint32_t h_i_translator;
    uint32_t m_i_reserved1;
};

struct ext2_osd2_linux_s {
    uint8_t l_i_frag;
    uint8_t l_i_fsize;
    uint16_t l_i_reserved1;
    uint16_t l_i_uid_high;
    uint16_t l_i_gid_high;
    uint32_t l_i_reserved2;
} __packed;

struct ext2_osd2_hurd_s {
    uint8_t h_i_frag;
    uint8_t h_i_fsize;
    uint16_t h_i_mode_high;
    uint16_t h_i_uid_high;
    uint16_t h_i_gid_high;
    uint32_t h_i_author;
} __packed;

struct ext2_osd2_masix_s {
    uint8_t m_i_frag;
    uint8_t m_i_fsize;
    uint16_t m_pad1;
    uint32_t m_i_reserved2[2];
} __packed;

union ext2_osd2_u {
    ext2_osd2_linux_t os_linux;
    ext2_osd2_hurd_t os_hurd;
    ext2_osd2_masix_t os_masix;
};

#ifndef __INTELLISENSE__
static_assert(12 == sizeof(ext2_osd2_t));
static_assert(offsetof(ext2_osd2_linux_t, l_i_uid_high) == offsetof(ext2_osd2_hurd_t, h_i_uid_high));
static_assert(offsetof(ext2_osd2_linux_t, l_i_gid_high) == offsetof(ext2_osd2_hurd_t, h_i_gid_high));
#endif

struct ext2_inode_s {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    ext2_osd1_t osd1;
    uint32_t i_block[EXT2_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high;
    uint32_t i_faddr;
    ext2_osd2_t osd2;
    uint16_t i_extra_isize; // Large inodes only; the fields below are valid as far as it reaches
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} __packed;

#ifndef __INTELLISENSE__
static_assert(EXT2_GOOD_OLD_INODE_SIZE == offsetof(ext2_inode_t, i_extra_isize));
static_assert(EXT2_GOOD_OLD_INODE_SIZE + EXT2_EXTRA_ISIZE == sizeof(ext2_inode_t));
#endif

struct ext2_dir_entry_s {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[];
} __packed;

#ifndef __INTELLISENSE__
static_assert(8 == sizeof(ext2_dir_entry_t));
#endif

struct ext2_xattr_header_s {
    uint32_t h_magic;
    uint32_t h_refcount;
    uint32_t h_blocks;
    uint32_t h_hash;
    uint32_t h_reserved[4];
} __packed;

struct ext2_xattr_entry_s {
    uint8_t e_name_len;
    uint8_t e_name_index;
    uint16_t e_value_offs;
    uint32_t e_value_block;
    uint32_t e_value_size;
    uint32_t e_hash;
    char e_name[];
} __packed;

#ifndef __INTELLISENSE__
static_assert(32 == sizeof(ext2_xattr_header_t));
static_assert(16 == sizeof(ext2_xattr_entry_t));
#endif

struct ext2_dirent_s {
    uint64_t offset; // Of the record within the directory
    uint32_t ino;
    uint8_t file_type;
    char name[NAME_SIZE];
};

struct ext2_vfs_context_s {
    mutex_t lock; // Serializes the bitmaps, the group descriptors, the superblock, directory changes and the file cache calls
    const block_interface_t* volume;
    vnode_t* device_node;

    ext2_superblock_t super;
    ext2_group_desc_t* groups;

    uint16_t mount_state; // s_state as found when mounted writable, put back on unmount

    uint32_t creator_os;
    size_t block_size;
    size_t sectors_per_block;
    size_t addr_per_block;
    size_t inode_size;
    uint64_t groups_offset;
    uint32_t groups_count;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t first_data_block;
    uint32_t first_ino;
};

struct ext2_vnode_context_s {
    ext2_vfs_context_t* fs_ctx;
    ext2_inode_t inode;
};

extern const vnode_ops_t ext2_vnode_ops;

int ext2_super_commit(ext2_vfs_context_t* restrict context);
int ext2_group_commit(ext2_vfs_context_t* restrict context, uint32_t group);

uint32_t ext2_block_alloc(ext2_vfs_context_t* restrict context, uint32_t goal) __nodiscard;
void ext2_block_free(ext2_vfs_context_t* restrict context, uint32_t block);
uint32_t ext2_inode_alloc(ext2_vfs_context_t* restrict context, uint32_t parent_ino, bool directory) __nodiscard;
void ext2_inode_free(ext2_vfs_context_t* restrict context, uint32_t ino, bool directory);

int ext2_inode_offset(const ext2_vfs_context_t* restrict context, uint32_t ino, uint64_t* restrict out);
int ext2_inode_read(ext2_vfs_context_t* restrict context, uint32_t ino, ext2_inode_t* restrict out);
int ext2_inode_clear(ext2_vfs_context_t* restrict context, uint32_t ino);
int ext2_inode_nodetype(uint16_t mode, unsigned* restrict out);
int ext2_inode_write(ext2_vfs_context_t* restrict context, uint32_t ino, const ext2_inode_t* restrict inode);
int ext2_inode_set_size(ext2_vfs_context_t* restrict context, ext2_inode_t* restrict inode, uint64_t size);
bool ext2_inode_set_owner(const ext2_vfs_context_t* restrict context, ext2_inode_t* restrict inode, uid_t uid, gid_t gid) __nodiscard;
void ext2_inode_get_time(const ext2_inode_t* restrict inode, unsigned which, timespec_t* restrict out);
void ext2_inode_set_time(ext2_inode_t* restrict inode, unsigned which, const timespec_t* restrict time);

int ext2_bmap(vnode_t* restrict node, uint64_t sector, bool create, fcache_extent_t* restrict out);
int ext2_truncate_blocks(vnode_t* restrict node, uint64_t size);

void ext2_xattr_release(ext2_vfs_context_t* restrict context, uint32_t block);
ssize_t ext2_xattr_get(vnode_t* restrict node, unsigned space, const char* restrict name, void* restrict value, size_t size);
int ext2_xattr_iterate(vnode_t* restrict node, xattr_entry_t* restrict out, size_t index);
int ext2_xattr_set(vnode_t* restrict node, unsigned space, const char* restrict name, const void* restrict value, size_t size,
    unsigned flags);
int ext2_xattr_remove(vnode_t* restrict node, unsigned space, const char* restrict name);

int ext2_dirent_read(vnode_t* restrict dir, uint64_t* restrict offset, ext2_dirent_t* restrict out);
/// Moves offset forward to the start of the record that contains it
/// @note An unlink merges a record into the one before it, so a cookie from before the unlink can point into a record
int ext2_dirent_align(vnode_t* restrict dir, uint64_t* restrict offset);
int ext2_dirent_find(vnode_t* restrict dir, const char* restrict name, ext2_dirent_t* restrict out);
int ext2_dirent_add(vnode_t* restrict dir, const char* restrict name, uint32_t ino, uint8_t file_type);
int ext2_dirent_remove(vnode_t* restrict dir, uint64_t offset);
int ext2_dirent_retarget(vnode_t* restrict dir, uint64_t offset, uint32_t ino, uint8_t file_type);
int ext2_dir_init(vnode_t* restrict dir, uint32_t parent_ino);
int ext2_dir_check_empty(vnode_t* restrict dir);

static __always_inline uint64_t ext2_block_offset(const ext2_vfs_context_t* restrict context, uint32_t block)
{
    return context->block_size * (uint64_t)(block);
}

static __always_inline uint64_t ext2_block_lba(const ext2_vfs_context_t* restrict context, uint32_t block)
{
    return context->sectors_per_block * (uint64_t)(block);
}

static __always_inline size_t ext2_dirent_reclen(size_t name_len)
{
    return (sizeof(ext2_dir_entry_t) + name_len + 3) & ~(size_t)(3);
}

static __always_inline bool ext2_has_id_high(const ext2_vfs_context_t* restrict context)
{
    bool result = false;
    result = result || context->creator_os == EXT2_OS_LINUX;
    result = result || context->creator_os == EXT2_OS_HURD;
    result = result || context->creator_os == EXT2_OS_FREEBSD;
    result = result || context->creator_os == EXT2_OS_LITES;
    return result;
}

static __always_inline uid_t ext2_inode_uid(const ext2_vfs_context_t* restrict context, const ext2_inode_t* restrict inode)
{
    uint32_t uid = le16toh(inode->i_uid);

    if(ext2_has_id_high(context)) {
        uid |= (uint32_t)(le16toh(inode->osd2.os_linux.l_i_uid_high)) << 16;
    }

    return (uid_t)(uid);
}

static __always_inline gid_t ext2_inode_gid(const ext2_vfs_context_t* restrict context, const ext2_inode_t* restrict inode)
{
    uint32_t gid = le16toh(inode->i_gid);

    if(ext2_has_id_high(context)) {
        gid |= (uint32_t)(le16toh(inode->osd2.os_linux.l_i_gid_high)) << 16;
    }

    return (gid_t)(gid);
}

static __always_inline uint64_t ext2_inode_size(const ext2_inode_t* restrict inode)
{
    uint64_t size = le32toh(inode->i_size);

    if(EXT2_S_IFREG == (le16toh(inode->i_mode) & S_IFMT)) {
        size |= (uint64_t)(le32toh(inode->i_size_high)) << 32;
    }

    return size;
}
