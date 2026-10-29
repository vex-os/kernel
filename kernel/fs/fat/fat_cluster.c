#include <vex/block_interface.h>
#include <vex/endian.h>
#include <vex/errno.h>
#include <vex/fcache.h>
#include <vex/page.h>
#include <vex/string.h>

#include "fat_internal.h"

#define FAT12_EOC  UINT16_C(0xFF8)
#define FAT16_EOC  UINT16_C(0xFFF8)
#define FAT32_EOC  UINT32_C(0x0FFFFFF8)
#define FAT32_MASK UINT32_C(0x0FFFFFFF)

#define SCAN_BYTES 1024 // Table bytes read at a time when looking for free clusters

static const uint8_t zeroes[PAGE_SIZE];

static size_t table_bytes(const fat_vfs_context_t* restrict context)
{
    return context->table_size_s * context->sector_size;
}

static uint64_t table_offset(const fat_vfs_context_t* restrict context, unsigned table, size_t offset)
{
    return (context->table_start + table * context->table_size_s) * context->sector_size + offset;
}

static bool table_load(const fat_vfs_context_t* restrict context, size_t offset, void* restrict buffer, size_t size)
{
    if(offset + size > table_bytes(context)) {
        return false;
    }

    uint64_t position = table_offset(context, 0, offset);

    return 0 == fcache_dev_read(context->device_node, position, buffer, size);
}

static void invalidate_fsinfo(fat_vfs_context_t* restrict context)
{
    if(context->filesystem_type != FAT_TYPE_32 || context->fsinfo_lba == 0) {
        return;
    }

    fat_fsinfo_fat32_t info;
    uint64_t offset = context->fsinfo_lba * context->sector_size;

    if(fcache_dev_read(context->device_node, offset, &info, sizeof(info)) < 0) {
        return;
    }

    bool fsinfo_valid = true;
    fsinfo_valid = fsinfo_valid && le32toh(info.lead_signature) == UINT32_C(0x41615252);
    fsinfo_valid = fsinfo_valid && le32toh(info.struct_signature) == UINT32_C(0x61417272);
    fsinfo_valid = fsinfo_valid && le32toh(info.trail_signature) == UINT32_C(0xAA550000);

    if(fsinfo_valid) {
        info.free_count = htole32(UINT32_C(0xFFFFFFFF));
        info.next_free = htole32(UINT32_C(0xFFFFFFFF));

        fcache_dev_write(context->device_node, offset, &info, sizeof(info));
    }
}

static void table_store(fat_vfs_context_t* restrict context, size_t offset, const void* restrict buffer, size_t size)
{
    if(offset + size > table_bytes(context)) {
        return;
    }

    if(!context->fsinfo_invalidated) {
        invalidate_fsinfo(context);

        context->fsinfo_invalidated = true;
    }

    for(unsigned i = 0; i < context->num_tables; ++i) {
        uint64_t position = table_offset(context, i, offset);

        fcache_dev_write(context->device_node, position, buffer, size);
    }
}

static size_t fat12_offset(uintptr_t cluster)
{
    return cluster + (cluster >> 1);
}

static bool fat12_get(const fat_vfs_context_t* restrict context, uintptr_t cluster, uint16_t* restrict out)
{
    uint16_t data;

    size_t position = fat12_offset(cluster);

    if(!table_load(context, position, &data, sizeof(data))) {
        return false;
    }

    data = le16toh(data);

    if(cluster & 1) {
        *out = data >> 4;
    }
    else {
        *out = data & UINT16_C(0x0FFF);
    }

    return true;
}

static void fat12_set(fat_vfs_context_t* restrict context, uintptr_t cluster, uint16_t value)
{
    uint16_t data;
    size_t position = fat12_offset(cluster);

    if(!table_load(context, position, &data, sizeof(data))) {
        return;
    }

    data = le16toh(data);

    if(cluster & 1) {
        data = (data & 0x000F) | ((value & 0x0FFF) << 4);
    }
    else {
        data = (data & 0xF000) | (value & 0x0FFF);
    }

    data = htole16(data);

    table_store(context, position, &data, sizeof(data));
}

static uint32_t entry_get(const fat_vfs_context_t* restrict context, uintptr_t cluster)
{
    if(context->filesystem_type == FAT_TYPE_12) {
        uint16_t data12;

        if(!fat12_get(context, cluster, &data12)) {
            return UINT32_MAX;
        }

        return data12 >= FAT12_EOC ? FAT32_EOC : data12;
    }

    if(context->filesystem_type == FAT_TYPE_16) {
        uint16_t data16;

        if(!table_load(context, cluster * sizeof(uint16_t), &data16, sizeof(data16))) {
            return UINT32_MAX;
        }

        data16 = le16toh(data16);

        return data16 >= FAT16_EOC ? FAT32_EOC : data16;
    }

    uint32_t data32;

    if(!table_load(context, cluster * sizeof(uint32_t), &data32, sizeof(data32))) {
        return UINT32_MAX;
    }

    return le32toh(data32) & FAT32_MASK;
}

static void entry_set(fat_vfs_context_t* restrict context, uintptr_t cluster, uint32_t value)
{
    if(context->filesystem_type == FAT_TYPE_12) {
        fat12_set(context, cluster, value);
    }
    else if(context->filesystem_type == FAT_TYPE_16) {
        uint16_t data16 = htole16(value);
        table_store(context, cluster * sizeof(uint16_t), &data16, sizeof(data16));
    }
    else if(context->filesystem_type == FAT_TYPE_32) {
        uint32_t data32;

        if(!table_load(context, cluster * sizeof(uint32_t), &data32, sizeof(data32))) {
            return;
        }

        data32 = (data32 & htole32(~FAT32_MASK)) | htole32(value & FAT32_MASK);

        table_store(context, cluster * sizeof(uint32_t), &data32, sizeof(data32));
    }
}

static size_t find_free(const fat_vfs_context_t* restrict context, uintptr_t first, size_t count, bool stop, size_t* restrict found)
{
    if(context->filesystem_type == FAT_TYPE_12) {
        size_t num_free = 0;

        for(size_t i = 0; i < count; ++i) {
            if(entry_get(context, first + i) == 0) {
                if(stop) {
                    *found = first + i;
                    return 1;
                }

                num_free += 1;
            }
        }

        return num_free;
    }

    size_t entry_size = context->filesystem_type == FAT_TYPE_16 ? sizeof(uint16_t) : sizeof(uint32_t);
    size_t per_batch = SCAN_BYTES / entry_size;
    uint8_t batch[SCAN_BYTES];
    size_t num_free = 0;

    for(size_t done = 0; done < count; done += per_batch) {
        size_t n = count - done;

        if(n > per_batch) {
            n = per_batch;
        }

        if(!table_load(context, (first + done) * entry_size, batch, n * entry_size)) {
            break;
        }

        for(size_t i = 0; i < n; ++i) {
            uint32_t value;

            if(entry_size == sizeof(uint16_t)) {
                uint16_t data16;
                memcpy(&data16, batch + i * entry_size, sizeof(data16));
                value = le16toh(data16);
            }
            else {
                uint32_t data32;
                memcpy(&data32, batch + i * entry_size, sizeof(data32));
                value = le32toh(data32) & FAT32_MASK;
            }

            if(value) {
                continue;
            }

            if(stop) {
                *found = first + done + i;
                return 1;
            }

            num_free += 1;
        }
    }

    return num_free;
}

uintptr_t fat_cluster_next(const fat_vfs_context_t* restrict context, uintptr_t cluster)
{
    if(!fat_cluster_valid(context, cluster)) {
        return UINTPTR_MAX;
    }

    uint32_t next = entry_get(context, cluster);

    // A link to a reserved or nonexistent cluster is a broken chain; it
    // ends here rather than letting the caller walk off the volume
    if(next >= FAT32_EOC || !fat_cluster_valid(context, next)) {
        return UINTPTR_MAX;
    }

    return next;
}

uintptr_t fat_cluster_alloc(fat_vfs_context_t* restrict context)
{
    // The tables are often larger than the volume needs; entries past
    // the last data cluster don't describe anything that can be allocated
    uintptr_t limit = context->cluster_count + 2;
    uintptr_t hint = context->next_free_hint;

    if(hint < 2 || hint >= limit) {
        hint = 2;
    }

    size_t found;

    if(find_free(context, hint, limit - hint, true, &found) == 0 && find_free(context, 2, hint - 2, true, &found) == 0) {
        return UINTPTR_MAX;
    }

    fat_cluster_end(context, found);
    context->next_free_hint = found + 1;

    return found;
}

void fat_cluster_link(fat_vfs_context_t* restrict context, uintptr_t cluster, uintptr_t next)
{
    if(!fat_cluster_valid(context, cluster)) {
        return;
    }

    entry_set(context, cluster, next);
}

void fat_cluster_end(fat_vfs_context_t* restrict context, uintptr_t cluster)
{
    if(context->filesystem_type == FAT_TYPE_12) {
        fat_cluster_link(context, cluster, 0x0FFF);
    }
    else if(context->filesystem_type == FAT_TYPE_16) {
        fat_cluster_link(context, cluster, 0xFFFF);
    }
    else if(context->filesystem_type == FAT_TYPE_32) {
        fat_cluster_link(context, cluster, FAT32_MASK);
    }
}

void fat_cluster_free(fat_vfs_context_t* restrict context, uintptr_t cluster)
{
    if(!fat_cluster_valid(context, cluster)) {
        return;
    }

    entry_set(context, cluster, 0);

    uint64_t offset = fat_cluster_lba(context, cluster) * context->sector_size;

    fcache_forget(context->device_node, offset, context->cluster_size_b);

    if(cluster < context->next_free_hint) {
        context->next_free_hint = cluster;
    }
}

uintptr_t fat_cluster_alloc_zeroed(fat_vfs_context_t* restrict context)
{
    uintptr_t cluster = fat_cluster_alloc(context);

    if(cluster == UINTPTR_MAX) {
        return UINTPTR_MAX;
    }

    uintptr_t lba = fat_cluster_lba(context, cluster);
    size_t per_write = PAGE_SIZE / context->sector_size;

    for(size_t s = 0; s < context->cluster_size_s; s += per_write) {
        size_t count = context->cluster_size_s - s;

        if(count > per_write) {
            count = per_write;
        }

        if(block_write(context->volume, lba + s, count, zeroes) < 0) {
            fat_cluster_free(context, cluster);
            return UINTPTR_MAX;
        }
    }

    return cluster;
}

uintptr_t fat_cluster_alloc_dir(fat_vfs_context_t* restrict context)
{
    uintptr_t cluster = fat_cluster_alloc(context);

    if(cluster == UINTPTR_MAX) {
        return UINTPTR_MAX;
    }

    uint64_t offset = fat_cluster_lba(context, cluster) * context->sector_size;

    for(size_t done = 0; done < context->cluster_size_b; done += PAGE_SIZE) {
        size_t count = context->cluster_size_b - done;

        if(count > PAGE_SIZE) {
            count = PAGE_SIZE;
        }

        if(fcache_dev_write(context->device_node, offset + done, zeroes, count) < 0) {
            fat_cluster_free(context, cluster);
            return UINTPTR_MAX;
        }
    }

    return cluster;
}

size_t fat_cluster_count_free(const fat_vfs_context_t* restrict context)
{
    size_t unused;

    return find_free(context, 2, context->cluster_count, false, &unused);
}
