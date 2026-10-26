#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/cred.h>
#include <vex/endian.h>
#include <vex/errno.h>
#include <vex/kmalloc.h>
#include <vex/slab.h>
#include <vex/stdlib.h>
#include <vex/string.h>

#define ID_UNCHANGED (-1)

static_assert(sizeof(uid_t) == sizeof(gid_t));

credentials_t kernel_credentials = {
    .refcount = 1,
    .cap_effective = CAP_FULL_SET,
    .cap_permitted = CAP_FULL_SET,
    .cap_bounding = CAP_FULL_SET,
};

size_t cred_max_groups = CONFIG_NGROUPS_MAX;

static __always_inline __nodiscard size_t cred_size(size_t num_groups)
{
    return sizeof(credentials_t) + num_groups * sizeof(gid_t);
}

static credentials_t* cred_alloc(size_t num_groups)
{
    size_t size = cred_size(num_groups);

    if(size <= SLAB_MAX_SIZE) {
        return slab_alloc(size);
    }

    return kmalloc(size);
}

static int compare_gids(const void* a, const void* b)
{
    gid_t gid_a = *(const gid_t*)(a);
    gid_t gid_b = *(const gid_t*)(b);

    if(gid_a < gid_b) {
        return -1;
    }

    if(gid_a > gid_b) {
        return 1;
    }

    return 0;
}

static int check_id(int32_t wanted, int32_t r, int32_t e, int32_t s, bool capable)
{
    if(wanted < ID_UNCHANGED) {
        return -EINVAL;
    }

    if(capable || wanted == ID_UNCHANGED || wanted == r || wanted == e || wanted == s) {
        return 0;
    }

    return -EPERM;
}

static int check_res(int32_t new_r, int32_t new_e, int32_t new_s, int32_t r, int32_t e, int32_t s, bool capable)
{
    int retval = check_id(new_r, r, e, s, capable);

    if(retval == 0) {
        retval = check_id(new_e, r, e, s, capable);
    }

    if(retval == 0) {
        retval = check_id(new_s, r, e, s, capable);
    }

    return retval;
}

static void fixup_caps(credentials_t* restrict credentials, uid_t old_r, uid_t old_e, uid_t old_s)
{
    bool was_root = old_r == 0 || old_e == 0 || old_s == 0;
    bool is_root = credentials->uid == 0 || credentials->euid == 0 || credentials->suid == 0;

    if(was_root && !is_root && 0 == (credentials->cap_flags & CAPS_KEEP)) {
        credentials->cap_permitted = 0;
        credentials->cap_ambient = 0;
    }

    if(old_e == 0 && credentials->euid) {
        credentials->cap_effective = 0;
    }

    if(old_e && credentials->euid == 0) {
        credentials->cap_effective = credentials->cap_permitted;
    }
}

static void apply_id(int32_t* restrict id, int32_t wanted)
{
    if(wanted != ID_UNCHANGED) {
        *id = wanted;
    }
}

credentials_t* cred_copy(const credentials_t* restrict credentials)
{
    kassert(credentials);

    credentials_t* copy = cred_copy_ids(credentials, credentials->num_groups);

    if(copy) {
        memcpy(copy->groups, credentials->groups, credentials->num_groups * sizeof(gid_t));
    }

    return copy;
}

credentials_t* cred_copy_real(const credentials_t* restrict credentials)
{
    kassert(credentials);

    credentials_t* copy = cred_copy(credentials);

    if(copy == nullptr) {
        return nullptr;
    }

    copy->euid = copy->uid;
    copy->egid = copy->gid;

    if(copy->uid == 0) {
        copy->cap_effective = copy->cap_permitted;
    }
    else {
        copy->cap_effective = 0;
    }

    return copy;
}

credentials_t* cred_copy_ids(const credentials_t* restrict credentials, size_t num_groups)
{
    kassert(credentials);

    credentials_t* copy = cred_alloc(num_groups);

    if(copy == nullptr) {
        return nullptr;
    }

    memcpy(copy, credentials, sizeof(credentials_t));
    copy->refcount = 1;
    copy->num_groups = num_groups;

    return copy;
}

void cred_incref(credentials_t* restrict credentials)
{
    kassert(credentials);

    atomic_fetch_add(&credentials->refcount, 1, ATOMIC_RELAXED);
}

void cred_decref(credentials_t* restrict credentials)
{
    kassert(credentials);

    if(1 == atomic_fetch_sub(&credentials->refcount, 1, ATOMIC_ACQ_REL)) {
        kassert(credentials != &kernel_credentials);

        if(cred_size(credentials->num_groups) <= SLAB_MAX_SIZE) {
            slab_free(credentials);
        }
        else {
            kfree(credentials);
        }
    }
}

bool cred_capable(const credentials_t* restrict credentials, unsigned cap)
{
    kassert(credentials);
    kassert(cap < 64);

    return credentials->cap_effective & CAP_TO_MASK(cap);
}

int cred_setcaps(credentials_t* restrict credentials, const caps_t* restrict caps)
{
    kassert(credentials);
    kassert(caps);

    if((caps->flags & ~CAPS_KEEP) || caps->reserved) {
        return -EINVAL;
    }

    if((caps->permitted & ~credentials->cap_permitted) || (caps->bounding & ~credentials->cap_bounding)) {
        return -EPERM;
    }

    if((caps->effective & ~caps->permitted) || (caps->ambient & ~caps->permitted)) {
        return -EPERM;
    }

    credentials->cap_effective = caps->effective;
    credentials->cap_permitted = caps->permitted;
    credentials->cap_ambient = caps->ambient;
    credentials->cap_bounding = caps->bounding;
    credentials->cap_flags = caps->flags;

    return 0;
}

int cred_exec_caps(credentials_t* restrict credentials, bool privileged, const file_caps_t* restrict file)
{
    kassert(credentials);

    if(privileged || file) {
        credentials->cap_ambient = 0;
    }

    credentials->cap_flags = 0;

    if(credentials->euid == 0) {
        credentials->cap_permitted = credentials->cap_bounding;
        credentials->cap_ambient &= credentials->cap_permitted;
        credentials->cap_effective = credentials->cap_permitted;
        return 0;
    }

    credentials->cap_permitted = credentials->cap_ambient & credentials->cap_bounding;
    credentials->cap_ambient &= credentials->cap_permitted;
    credentials->cap_effective = credentials->cap_ambient;

    if(file == nullptr) {
        return 0;
    }

    // A program that counts on all its capabilities being in effect
    // without checking must not start with only some of them
    if(file->effective && (file->permitted & ~credentials->cap_bounding)) {
        return -EPERM;
    }

    credentials->cap_permitted |= file->permitted & credentials->cap_bounding;

    if(file->effective) {
        credentials->cap_effective = credentials->cap_permitted;
    }

    return 0;
}

bool cred_exec_secure(const credentials_t* restrict credentials)
{
    kassert(credentials);

    if(credentials->uid != credentials->euid || credentials->gid != credentials->egid) {
        return true;
    }

    return credentials->uid && (credentials->cap_permitted & ~credentials->cap_ambient);
}

int cred_parse_file_caps(const void* restrict value, size_t size, file_caps_t* restrict out)
{
    kassert(value);
    kassert(out);

    vfs_cap_data_t data = {};

    if(size < sizeof(data.magic_etc) || size > sizeof(data)) {
        return -EINVAL;
    }

    memcpy(&data, value, size);

    uint32_t magic = le32toh(data.magic_etc);
    uint32_t revision = magic & VFS_CAP_REVISION_MASK;
    size_t expected;

    switch(revision) {
        case VFS_CAP_REVISION_1:
            expected = VFS_CAP_SIZE_1;
            break;

        case VFS_CAP_REVISION_2:
            expected = VFS_CAP_SIZE_2;
            break;

        case VFS_CAP_REVISION_3:
            expected = VFS_CAP_SIZE_3;
            break;

        default:
            return -EINVAL;
    }

    if(size != expected) {
        return -EINVAL;
    }

    if(revision == VFS_CAP_REVISION_3 && data.rootid) {
        return -ENODATA;
    }

    out->permitted = le32toh(data.data[0].permitted);
    out->inheritable = le32toh(data.data[0].inheritable);
    out->effective = magic & VFS_CAP_FLAGS_EFFECTIVE;

    if(revision != VFS_CAP_REVISION_1) {
        out->permitted |= (uint64_t)(le32toh(data.data[1].permitted)) << 32;
        out->inheritable |= (uint64_t)(le32toh(data.data[1].inheritable)) << 32;
    }

    return 0;
}

bool cred_in_group(const credentials_t* restrict credentials, gid_t gid)
{
    kassert(credentials);

    if(credentials->egid == gid) {
        return true;
    }

    size_t low = 0;
    size_t high = credentials->num_groups;

    while(low < high) {
        size_t mid = low + (high - low) / 2;

        if(credentials->groups[mid] == gid) {
            return true;
        }

        if(credentials->groups[mid] < gid) {
            low = mid + 1;
        }
        else {
            high = mid;
        }
    }

    return false;
}

int cred_setresuid(credentials_t* restrict credentials, uid_t ruid, uid_t euid, uid_t suid)
{
    kassert(credentials);

    bool capable = cred_capable(credentials, CAP_SETUID);
    int retval = check_res(ruid, euid, suid, credentials->uid, credentials->euid, credentials->suid, capable);

    if(retval < 0) {
        return retval;
    }

    uid_t old_r = credentials->uid;
    uid_t old_e = credentials->euid;
    uid_t old_s = credentials->suid;

    apply_id(&credentials->uid, ruid);
    apply_id(&credentials->euid, euid);
    apply_id(&credentials->suid, suid);

    fixup_caps(credentials, old_r, old_e, old_s);

    return 0;
}

int cred_setresgid(credentials_t* restrict credentials, gid_t rgid, gid_t egid, gid_t sgid)
{
    kassert(credentials);

    bool capable = cred_capable(credentials, CAP_SETGID);
    int retval = check_res(rgid, egid, sgid, credentials->gid, credentials->egid, credentials->sgid, capable);

    if(retval < 0) {
        return retval;
    }

    apply_id(&credentials->gid, rgid);
    apply_id(&credentials->egid, egid);
    apply_id(&credentials->sgid, sgid);

    return 0;
}

int cred_sort_groups(credentials_t* restrict credentials)
{
    kassert(credentials);

    for(size_t i = 0; i < credentials->num_groups; ++i) {
        if(credentials->groups[i] < 0) {
            return -EINVAL;
        }
    }

    sort(credentials->groups, credentials->num_groups, sizeof(gid_t), &compare_gids);

    return 0;
}
