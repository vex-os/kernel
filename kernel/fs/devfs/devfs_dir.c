#include <vex/dirent.h>
#include <vex/errno.h>
#include <vex/spinlock.h>
#include <vex/stat.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/vnode.h>

#include "devfs_internal.h"

static int devfs_dir_lookup(vnode_t* restrict node, const char* restrict name, ino_t* restrict out)
{
    devfs_state_t* state = node->ctxdata;

    spinlock_acquire(&state->lock);

    for(devfs_node_t* it = state->head; it; it = it->next) {
        if(0 == strcmp(it->name, name)) {
            *out = (ino_t)(uintptr_t)(it->node);
            spinlock_release(&state->lock);
            return 0;
        }
    }

    spinlock_release(&state->lock);

    return -ENOENT;
}

static int devfs_dir_iterate(vnode_t* restrict node, dir_context_t* restrict dir_context)
{
    devfs_state_t* state = node->ctxdata;
    dirent_t record;
    uint64_t index = DIR_DOTS_END;

    // devfs has only its root
    if(!dir_emit_dots(dir_context, node->ino, node->ino)) {
        return 0;
    }

    spinlock_acquire(&state->lock);

    for(devfs_node_t* it = state->head; it; it = it->next, index++) {
        if(index < dir_context->position) {
            continue;
        }

        memset(record.d_name, 0, sizeof(record.d_name));
        kstrncpy(record.d_name, it->name, sizeof(record.d_name));
        record.d_ino = (ino_t)((uintptr_t)(it->node));
        record.d_type = dirent_type_from_vnode(it->node->type);

        if(!dir_emit(dir_context, &record, index + 1)) {
            break;
        }
    }

    spinlock_release(&state->lock);

    return 0;
}

static int devfs_dir_stat(vnode_t* restrict node, stat_t* restrict out)
{
    memset(out, 0, sizeof(stat_t));

    out->st_mode = S_IFDIR | S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;

    return 0;
}

const vnode_ops_t devfs_dir_ops = {
    .lookup = &devfs_dir_lookup,
    .iterate = &devfs_dir_iterate,
    .stat = &devfs_dir_stat,
};
