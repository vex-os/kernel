#pragma once

#include <vex/compiler.h>
#include <vex/limits.h>
#include <vex/types.h>

typedef struct credentials_s credentials_t;
typedef struct vnode_s vnode_t;

#define PATH_SEPARATOR 0x2F

#define PATH_NOFOLLOW 0x0001U // A symlink in the last component is returned, not followed

int path_resolve(const char* restrict path, const credentials_t* restrict credentials, vnode_t** restrict out);
int path_resolve_at(vnode_t* restrict base_dir, const char* restrict path, unsigned flags, const credentials_t* restrict credentials,
    vnode_t** restrict out);
int path_resolve_parent_at(vnode_t* restrict base_dir, const char* restrict path, const credentials_t* restrict credentials,
    vnode_t** restrict out_parent, const char** restrict out_name);

/// Writes the absolute path of a directory as a C string at the start of the buffer
/// @pre node is a directory
/// @return The length of the string with its terminating zero
/// @retval -ENOENT The directory is not under the root, for example as its last name was removed
/// @retval -ERANGE The path does not fit in the buffer
/// @note A vnode keeps no name, so this looks for the number of each directory in its parent
ssize_t path_build(vnode_t* restrict node, char* restrict buffer, size_t size) __nodiscard;
