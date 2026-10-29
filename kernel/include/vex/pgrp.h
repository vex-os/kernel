#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

typedef struct process_s process_t;
typedef struct tty_interface_s tty_interface_t;

typedef struct session_s session_t;
typedef struct process_group_s process_group_t;

struct session_s {
    pid_t sid;
    uint32_t refcount;
    tty_interface_t* tty; // The controlling terminal, under process_group_lock; holds a reference to its chr device
};

struct process_group_s {
    pid_t pgid;
    uint32_t refcount;
    uint32_t num_members; // Under process_group_lock; a group with none is out of the table and takes no one in
    session_t* session;
    process_t* members; // Under process_group_lock, linked through process_t.group_next
    process_group_t* hash_next;
};

/// Holds the lock that guards every process's group, the groups' members and the sessions' terminals;
/// a mutex, so it can be held while signals go out
void process_group_lock_acquire(void);
void process_group_lock_release(void);

/// @pre The caller holds process_group_lock, which keeps the group alive without a reference
process_group_t* process_group_find_locked(pid_t pgid) __nodiscard;

/// Whether no member of the group has a parent outside it in the same session. Exited processes and children of init do not count
/// @pre The caller holds process_group_lock
bool process_group_orphaned_locked(const process_group_t* restrict group) __nodiscard;

/// Sends SIGHUP and SIGCONT to the groups, its own and those of its children, that its exit orphans while they hold a stopped member
/// @pre The process is exiting, and the caller may sleep and does not hold process_group_lock
void process_group_check_orphans(process_t* restrict process);

/// Sends a signal from the kernel to every member of the group
/// @pre The caller holds process_group_lock
void process_group_signal_locked(process_group_t* restrict group, int sig);

void session_incref(session_t* session);
void session_decref(session_t* session);

/// Puts a new process in its parent's group, or init, which has no parent, in a session of its own
/// @pre The process is not in a group yet
/// @retval 0 In the group
/// @retval -ENOMEM No memory for init's session
int process_group_attach(process_t* restrict process, process_t* restrict parent) __nodiscard;

/// Takes a destroyed process out of its group; a process that was never attached is left alone
void process_group_detach(process_t* process);

process_group_t* process_group_find(pid_t pgid) __nodiscard;
process_group_t* process_get_group(process_t* process) __nodiscard;
void process_group_incref(process_group_t* group);
void process_group_decref(process_group_t* group);

/// @return The id of the process's group
/// @retval -ESRCH The process was destroyed since it was located
pid_t process_get_pgid(process_t* process) __nodiscard;

/// @return The id of the process's session
/// @retval -ESRCH The process was destroyed since it was located
pid_t process_get_sid(process_t* process) __nodiscard;

/// setsid: makes the process the leader of a new session and of a new group in it
/// @return The new session's id
/// @retval -EPERM A group with the process's id exists, so the process would not lead its group
/// @retval -ENOMEM No memory for the session or the group
/// @note Groups that the process tied to its old session are hung up like on an exit, if a member is stopped
pid_t process_group_new_session(process_t* process) __nodiscard;

/// setpgid: moves the caller or a child of its into another group of the caller's session, or a new one
/// @param pgid The group's id; the process's own id makes a new group if there is none
/// @retval 0 Moved
/// @retval -ESRCH The process is neither the caller nor a child of the caller's, or was destroyed
/// @retval -EPERM The process is in another session or leads its own, or the
/// group is in another session or is not there for a pgid other than the process's id
/// @retval -EACCES The child has called exec since fork
/// @retval -ENOMEM No memory for a new group
/// @note Groups that the process tied to its old group are hung up like on an exit, if a member is stopped
int process_group_move(process_t* restrict caller, process_t* restrict process, pid_t pgid) __nodiscard;

/// Calls the callback on every member of the group, the zombies among them
/// @pre The callback must not take process_group_lock, which it runs under; it may sleep
/// @retval 0 The callback ran for every member
/// @retval -ESRCH No group has the id
int process_group_for_each(pid_t pgid, void (*callback)(process_t* restrict process, void* restrict arg), void* restrict arg);
