#include <vex/block_interface.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/kprintf.h>
#include <vex/string.h>
#include <vex/vnode.h>

#include "ext2_internal.h"

#define MIN_REC_LEN (size_t)(4 + sizeof(ext2_dir_entry_t))
#define MAX_REC_LEN (size_t)((EXT2_NAME_LEN + 3 + sizeof(ext2_dir_entry_t)) & ~(size_t)(3))

static bool has_filetype(const ext2_vfs_context_t* restrict fs_ctx)
{
    return le32toh(fs_ctx->super.s_feature_incompat) & EXT2_FEATURE_INCOMPAT_FILETYPE;
}

static size_t rec_len_from_disk(const ext2_vfs_context_t* restrict fs_ctx, uint16_t raw)
{
    size_t rec_len = le16toh(raw);

    if(fs_ctx->block_size == 65536 && (rec_len == 0 || rec_len == 65535)) {
        return 65536;
    }

    return rec_len;
}

static uint16_t rec_len_to_disk(size_t rec_len)
{
    if(rec_len == 65536) {
        return htole16(65535);
    }

    return htole16((uint16_t)(rec_len));
}

static int read_exact(vnode_t* restrict dir, void* restrict buffer, size_t size, uint64_t offset)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;
    ssize_t done = fcache_read(dir, &ext2_bmap, buffer, size, offset, ext2_inode_size(&ctx->inode));

    if(done < 0) {
        return done;
    }

    if((size_t)(done) != size) {
        return -EIO;
    }

    return 0;
}

static int write_exact(vnode_t* restrict dir, const void* restrict buffer, size_t size, uint64_t offset)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;

    ssize_t done = fcache_write(dir, &ext2_bmap, buffer, size, offset, ext2_inode_size(&ctx->inode));

    if(done < 0) {
        return done;
    }

    if((size_t)(done) != size) {
        return -EIO;
    }

    return 0;
}

static int read_record(vnode_t* restrict dir, uint64_t offset, ext2_dir_entry_t* restrict head, size_t* restrict rec_len)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    int retval = read_exact(dir, head, sizeof(ext2_dir_entry_t), offset);

    if(retval < 0) {
        return retval;
    }

    *rec_len = rec_len_from_disk(fs_ctx, head->rec_len);

    size_t in_block = offset % fs_ctx->block_size;

    bool valid = true;
    valid = valid && *rec_len >= ext2_dirent_reclen(head->name_len);
    valid = valid && 0 == *rec_len % 4;
    valid = valid && *rec_len <= fs_ctx->block_size - in_block;

    if(!valid) {
        pr_warning("ext2: %s: inode %u has a broken directory record at %zu", fs_ctx->volume->name, (uint32_t)(dir->ino), (size_t)(offset));
        return -EIO;
    }

    return 0;
}

static void fill_record(const ext2_vfs_context_t* restrict fs_ctx, uint8_t* restrict at, size_t rec_len, uint32_t ino,
    const char* restrict name, uint8_t file_type)
{
    ext2_dir_entry_t* head = (ext2_dir_entry_t*)(at);
    size_t name_len = strlen(name);

    head->inode = htole32(ino);
    head->rec_len = rec_len_to_disk(rec_len);
    head->name_len = (uint8_t)(name_len);
    head->file_type = 0;

    if(has_filetype(fs_ctx)) {
        head->file_type = file_type;
    }

    memcpy(head->name, name, name_len);
}

static int write_record(vnode_t* restrict dir, uint64_t offset, size_t rec_len, uint32_t ino, const char* restrict name, uint8_t file_type)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;
    uint8_t buffer[MAX_REC_LEN] = {};

    fill_record(ctx->fs_ctx, buffer, rec_len, ino, name, file_type);

    size_t exact_size = ext2_dirent_reclen(strlen(name));

    return write_exact(dir, buffer, exact_size, offset);
}

static int directory_changed(vnode_t* restrict dir)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;
    uint32_t flags = le32toh(ctx->inode.i_flags);

    if(0 == (flags & EXT2_INDEX_FL)) {
        return 0;
    }

    ctx->inode.i_flags = htole32(flags & ~EXT2_INDEX_FL);

    return ext2_inode_write(ctx->fs_ctx, (uint32_t)(dir->ino), &ctx->inode);
}

static int append_block(vnode_t* restrict dir, const uint8_t* restrict records, size_t length)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    uint64_t size = ext2_inode_size(&ctx->inode);

    int retval = write_exact(dir, records, length, size);

    if(retval == 0) {
        retval = ext2_inode_set_size(fs_ctx, &ctx->inode, size + fs_ctx->block_size);
    }

    if(retval == 0) {
        retval = ext2_inode_write(fs_ctx, (uint32_t)(dir->ino), &ctx->inode);
    }

    return retval;
}

int ext2_dirent_read(vnode_t* restrict dir, uint64_t* restrict offset, ext2_dirent_t* restrict out)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;
    uint64_t size = ext2_inode_size(&ctx->inode);

    while(*offset < size) {
        ext2_dir_entry_t head;
        size_t rec_len;
        int retval = read_record(dir, *offset, &head, &rec_len);

        if(retval < 0) {
            return retval;
        }

        uint64_t record = *offset;
        uint32_t ino = le32toh(head.inode);

        *offset += rec_len;

        if(ino == 0) {
            continue;
        }

        retval = read_exact(dir, out->name, head.name_len, record + sizeof(head));

        if(retval < 0) {
            return retval;
        }

        out->name[head.name_len] = 0;
        out->offset = record;
        out->ino = ino;

        if(has_filetype(ctx->fs_ctx)) {
            out->file_type = head.file_type;
        }
        else {
            out->file_type = EXT2_FT_UNKNOWN;
        }

        return 0;
    }

    return -ENOENT;
}

int ext2_dirent_align(vnode_t* restrict dir, uint64_t* restrict offset)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;
    uint64_t size = ext2_inode_size(&ctx->inode);
    uint64_t record = *offset - *offset % ctx->fs_ctx->block_size;

    while(record < *offset && record < size) {
        ext2_dir_entry_t head;
        size_t rec_len;
        int retval = read_record(dir, record, &head, &rec_len);

        if(retval < 0) {
            return retval;
        }

        record += rec_len;
    }

    *offset = record;

    return 0;
}

int ext2_dirent_find(vnode_t* restrict dir, const char* restrict name, ext2_dirent_t* restrict out)
{
    uint64_t offset = 0;

    for(;;) {
        int retval = ext2_dirent_read(dir, &offset, out);

        if(retval < 0) {
            return retval;
        }

        if(0 == strcmp(out->name, name)) {
            return 0;
        }
    }
}

int ext2_dirent_add(vnode_t* restrict dir, const char* restrict name, uint32_t ino, uint8_t file_type)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;
    uint64_t size = ext2_inode_size(&ctx->inode);
    size_t needed = ext2_dirent_reclen(strlen(name));
    uint64_t offset = 0;

    int retval = directory_changed(dir);

    if(retval < 0) {
        return retval;
    }

    while(offset < size) {
        ext2_dir_entry_t head;
        size_t rec_len;
        retval = read_record(dir, offset, &head, &rec_len);

        if(retval < 0) {
            return retval;
        }

        size_t used = 0;

        if(head.inode) {
            used = ext2_dirent_reclen(head.name_len);
        }

        if(rec_len - used < needed) {
            offset += rec_len;
            continue;
        }

        if(used) {
            uint16_t shrunk = rec_len_to_disk(used);
            retval = write_exact(dir, &shrunk, sizeof(shrunk), offset + offsetof(ext2_dir_entry_t, rec_len));

            if(retval < 0) {
                return retval;
            }
        }

        return write_record(dir, offset + used, rec_len - used, ino, name, file_type);
    }

    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    uint8_t record[MAX_REC_LEN] = {};

    fill_record(fs_ctx, record, fs_ctx->block_size, ino, name, file_type);

    size_t exact_size = ext2_dirent_reclen(strlen(name));

    return append_block(dir, record, exact_size);
}

int ext2_dirent_remove(vnode_t* restrict dir, uint64_t offset)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;
    uint64_t block_start = offset - offset % ctx->fs_ctx->block_size;

    int retval = directory_changed(dir);

    if(retval < 0) {
        return retval;
    }

    ext2_dir_entry_t head;
    size_t rec_len;
    retval = read_record(dir, offset, &head, &rec_len);

    if(retval < 0) {
        return retval;
    }

    if(offset == block_start) {
        uint32_t unused = 0;
        return write_exact(dir, &unused, sizeof(unused), offset + offsetof(ext2_dir_entry_t, inode));
    }

    uint64_t previous = block_start;

    for(;;) {
        ext2_dir_entry_t previous_head;
        size_t previous_len;
        retval = read_record(dir, previous, &previous_head, &previous_len);

        if(retval < 0) {
            return retval;
        }

        if(previous + previous_len == offset) {
            uint16_t merged = rec_len_to_disk(previous_len + rec_len);
            return write_exact(dir, &merged, sizeof(merged), previous + offsetof(ext2_dir_entry_t, rec_len));
        }

        if(previous + previous_len > offset) {
            return -EIO;
        }

        previous += previous_len;
    }
}

int ext2_dirent_retarget(vnode_t* restrict dir, uint64_t offset, uint32_t ino, uint8_t file_type)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;
    uint32_t raw = htole32(ino);

    int retval = directory_changed(dir);

    if(retval == 0) {
        retval = write_exact(dir, &raw, sizeof(raw), offset + offsetof(ext2_dir_entry_t, inode));
    }

    if(retval == 0 && has_filetype(ctx->fs_ctx)) {
        retval = write_exact(dir, &file_type, sizeof(file_type), offset + offsetof(ext2_dir_entry_t, file_type));
    }

    return retval;
}

int ext2_dir_init(vnode_t* restrict dir, uint32_t parent_ino)
{
    ext2_vnode_context_t* ctx = dir->ctxdata;
    ext2_vfs_context_t* fs_ctx = ctx->fs_ctx;
    size_t dot_len = ext2_dirent_reclen(1);
    size_t dotdot_len = ext2_dirent_reclen(2);
    uint8_t records[2 * MIN_REC_LEN] = {};

    fill_record(fs_ctx, records, dot_len, (uint32_t)(dir->ino), ".", EXT2_FT_DIR);
    fill_record(fs_ctx, records + dot_len, fs_ctx->block_size - dot_len, parent_ino, "..", EXT2_FT_DIR);

    return append_block(dir, records, dot_len + dotdot_len);
}

int ext2_dir_check_empty(vnode_t* restrict dir)
{
    ext2_dirent_t entry;
    uint64_t offset = 0;

    for(;;) {
        int retval = ext2_dirent_read(dir, &offset, &entry);

        if(retval == -ENOENT) {
            return 0;
        }

        if(retval < 0) {
            return retval;
        }

        if(strcmp(entry.name, ".") && strcmp(entry.name, "..")) {
            return -ENOTEMPTY;
        }
    }
}
