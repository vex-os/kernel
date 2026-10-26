#pragma once

#include <vex/capability.h>
#include <vex/compiler.h>
#include <vex/types.h>

typedef struct credentials_s credentials_t;
typedef struct file_caps_s file_caps_t;

struct credentials_s {
    unsigned refcount;

    uid_t uid;
    uid_t euid;
    uid_t suid;

    gid_t gid;
    gid_t egid;
    gid_t sgid;

    uint64_t cap_effective;
    uint64_t cap_permitted;
    uint64_t cap_ambient;
    uint64_t cap_bounding;
    unsigned cap_flags;

    size_t num_groups;
    gid_t groups[];
};

struct file_caps_s {
    uint64_t permitted;
    uint64_t inheritable;
    bool effective;
};

extern credentials_t kernel_credentials;
extern size_t cred_max_groups;

credentials_t* cred_copy(const credentials_t* restrict credentials) __nodiscard;

/// Copies the credentials as they are for a check that goes by the real ids, as access does
/// @return The copy with one reference, or nullptr when out of memory
/// @note The copy has the real ids as the effective ones. As on Linux, it has all the permitted capabilities
/// if the real uid is 0, and none otherwise
credentials_t* cred_copy_real(const credentials_t* restrict credentials) __nodiscard;
credentials_t* cred_copy_ids(const credentials_t* restrict credentials, size_t num_groups) __nodiscard;
void cred_incref(credentials_t* restrict credentials);
void cred_decref(credentials_t* restrict credentials);

bool cred_capable(const credentials_t* restrict credentials, unsigned cap) __nodiscard;
int cred_setcaps(credentials_t* restrict credentials, const caps_t* restrict caps) __nodiscard;

/// Works out the capabilities a program starts with. There is no inheritable
/// set, so the file's inheritable set never grants anything
/// @pre The ids the program will run with are already in place
/// @param privileged Whether a setuid or setgid bit of the program took effect
/// @param file The program's file capabilities, nullptr when it has none
int cred_exec_caps(credentials_t* restrict credentials, bool privileged, const file_caps_t* restrict file) __nodiscard;

/// Checks if the new program must not trust what the caller passed to it
/// @note That is when the program runs with other ids, or has capabilities that the caller cannot hand down
bool cred_exec_secure(const credentials_t* restrict credentials) __nodiscard;

/// Revision 3 names the uid that counts as root for the capabilities
/// @note Without user namespaces only 0 is valid. Any other uid leaves the file with no capabilities, as on Linux
int cred_parse_file_caps(const void* restrict value, size_t size, file_caps_t* restrict out) __nodiscard;

bool cred_in_group(const credentials_t* restrict credentials, gid_t gid) __nodiscard;

int cred_setresuid(credentials_t* restrict credentials, uid_t ruid, uid_t euid, uid_t suid) __nodiscard;
int cred_setresgid(credentials_t* restrict credentials, gid_t rgid, gid_t egid, gid_t sgid) __nodiscard;
int cred_sort_groups(credentials_t* restrict credentials) __nodiscard;
