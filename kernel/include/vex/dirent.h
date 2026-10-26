#pragma once

#include <usr/vex/dirent.h>
#include <vex/path.h>
#include <vex/types.h>

#define DT_UNKNOWN __k_DT_UNKNOWN
#define DT_FIFO    __k_DT_FIFO
#define DT_CHR     __k_DT_CHR
#define DT_DIR     __k_DT_DIR
#define DT_BLK     __k_DT_BLK
#define DT_REG     __k_DT_REG
#define DT_LNK     __k_DT_LNK
#define DT_SOCK    __k_DT_SOCK

#define DIR_DOTS_END 2 // The position after "." and ".."

typedef struct __k_dirent dirent_t;
typedef struct dir_context_s dir_context_t;

struct dir_context_s {
    uint64_t position;
    bool (*emit)(dir_context_t* restrict dir_context, const dirent_t* restrict entry);
    void* state;
};

/// Hands one entry to the reader
/// @pre Called from vnode_ops_t.iterate, with the entries in the order of their positions
/// @param next_position The position of the entry after this one
/// @retval true The reader took the entry, and dir_context->position is next_position
/// @retval false The reader is full; the caller stops and leaves dir_context->position as it is
bool dir_emit(dir_context_t* restrict dir_context, dirent_t* restrict entry, uint64_t next_position);

/// Hands "." and ".." to the reader at the positions 0 and 1
/// @pre Called from vnode_ops_t.iterate, before any other entry
/// @param self The node number of the directory
/// @param parent The node number of its parent, or self for the root of a filesystem
/// @retval true Both entries are with the reader, and dir_context->position is at least DIR_DOTS_END
/// @retval false The reader is full; the caller stops
/// @note A filesystem that calls this starts its own positions at DIR_DOTS_END
bool dir_emit_dots(dir_context_t* restrict dir_context, ino_t self, ino_t parent);

/// Gives the DT_ value that a directory entry of this node type has
/// @param vnode_type A VNODE_ value
/// @retval DT_UNKNOWN The type has no DT_ value
uint8_t dirent_type_from_vnode(unsigned vnode_type);
