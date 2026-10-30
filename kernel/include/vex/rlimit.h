#pragma once

#include <vex/compiler.h>
#include <vex/limits.h>
#include <vex/resource.h>
#include <vex/types.h>

typedef struct process_s process_t;
typedef struct rlimits_s rlimits_t;

/// The soft limit of the stack of a program that nothing set: the stack that the program gets from exec
#define RLIMIT_STACK_DEFAULT (UINT64_C(1024) * CONFIG_USER_STACK_SIZE)

/// The number of signals that may wait in the queues of a process, as in FreeBSD (kern.sigqueue.max_pending_per_proc)
#define RLIMIT_SIGPENDING_DEFAULT UINT64_C(128)
#define RLIMIT_SIGPENDING_CEILING UINT64_C(1024)

/// Makes the limits that init starts with, with one reference
rlimits_t* rlimits_create(void) __nodiscard;

void rlimits_incref(rlimits_t* restrict rlimits);
void rlimits_decref(rlimits_t* restrict rlimits);

/// The limits of the process, with a new reference; a fork shares them with its parent
rlimits_t* process_get_rlimits(process_t* restrict process) __nodiscard;

/// @pre resource is below RLIMIT_NLIMITS
rlimit_t process_get_rlimit(process_t* restrict process, unsigned resource) __nodiscard;

/// Changes one limit; the other limits of the process stay as they are
/// @param privileged Whether the caller may raise a hard limit
/// @pre resource is below RLIMIT_NLIMITS
/// @retval -EINVAL The soft limit is above the hard one
/// @retval -EPERM The hard limit goes up and the caller is not privileged, or it is above what the kernel can give
/// @retval -ENOMEM No memory for the new limits
int process_set_rlimit(process_t* restrict process, unsigned resource, const rlimit_t* restrict limit, bool privileged);
