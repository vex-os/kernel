#pragma once

#include <vex/cred.h>
#include <vex/event.h>
#include <vex/itimer.h>
#include <vex/limits.h>
#include <vex/mutex.h>
#include <vex/signal.h>
#include <vex/spinlock.h>
#include <vex/usage.h>

typedef struct file_s file_t;
typedef struct pending_signal_s pending_signal_t;
typedef struct process_group_s process_group_t;
typedef struct rlimits_s rlimits_t;
typedef struct thread_s thread_t;
typedef struct vmspace_s vmspace_t;
typedef struct vnode_s vnode_t;

#define INIT_PID 1

#define PROCESS_EXITING       0x01U
#define PROCESS_ZOMBIE        0x02U // Every thread is reaped
#define PROCESS_COLLECTED     0x04U // A waiter claimed the zombie
#define PROCESS_DID_EXEC      0x08U // Its parent can no longer move it to another group
#define PROCESS_EXIT_FINISHED 0x10U // process_finish_exit did its work

#define JOB_RUNNING 0
#define JOB_STOPPED 1

#define JOB_REPORT_STOPPED   0x01U // A stop that wait has not reported yet
#define JOB_REPORT_CONTINUED 0x02U // A continue that wait has not reported yet

typedef struct fd_slot_s fd_slot_t;
typedef struct job_control_s job_control_t;
typedef struct process_s process_t;

struct job_control_s {
    uint8_t state;       // Under the lock of the process
    uint8_t stop_signal; // Under the lock; wait reads it once it holds the report
    uint8_t report;      // Atomic operations only: a waiter takes a bit without the lock
    event_t resume;      // Triggered under the lock of the process
};

struct fd_slot_s {
    file_t* file;
    uint32_t flags;
};

struct process_s {
    pid_t pid;
    pid_t ppid;
    int32_t exit_code;

    uint32_t refcount;

    uint8_t flags; // PROCESS_*, only ever set, with atomic_fetch_or: the bits share a byte
    uint8_t exit_signal;
    int8_t nice; // read by the scheduler without the lock

    spinlock_t lock;

    vmspace_t* vmspace;

    thread_t* threads;
    thread_t* exec_thread;     // Waiting in exec for the other threads to go, under lock
    uint32_t num_threads;      // Until reaped, under lock
    uint32_t num_live_threads; // Until thread_exit, under lock

    process_t* children;
    process_t* sibling_next;
    process_t* sibling_prev;

    process_t* hash_next;

    process_group_t* group; // Changed under process_group_lock, held from creation until the process is destroyed
    process_t* group_next;

    event_t child_exited; // Triggered under lock once a child is a zombie or init adopts one

    credentials_t* credentials;
    mutex_t credentials_lock;

    rlimits_t* rlimits; // Shared with the parent until one of them changes a limit, under lock

    job_control_t job;

    signal_actions_t* signal_actions;
    pending_signal_t* exit_note;
    pending_signal_t* adopt_note;
    sigset_t sig_pending;
    pending_signal_t* sig_queue;

    usage_t usage;          // Of the threads that are reaped, under lock
    usage_t children_usage; // Of the children that wait collected and their own collected children, under lock

    itimer_t itimer; // Not inherited by a fork, and kept over an exec

    vnode_t* cwd;
    vnode_t* root; // Where chroot put it; nullptr stands for the root of the VFS
    mode_t umask;

    spinlock_t fd_lock;
    uint32_t num_fds;
    uint32_t max_fds;
    fd_slot_t* fds;
};

/// Triggered once init's last thread is reaped; init never exits by design
extern event_t init_exited;

/// Threads take their ids from the same counter, so a tid never names another process
pid_t process_alloc_id(void) __nodiscard;

process_t* process_create(process_t* restrict parent) __nodiscard;
process_t* process_locate(pid_t pid) __nodiscard;

/// The process with this pid, or the caller for zero. The caller gets a new reference
/// @return nullptr for a negative pid or a process that does not exist
process_t* process_locate_or_self(process_t* restrict caller, pid_t pid) __nodiscard;

/// Calls the callback for every process, with a reference held, in no particular order
/// @note None is visited twice, and a process that appears meanwhile may be left out. The callback may sleep
void process_for_each(void (*callback)(process_t* restrict process, void* restrict arg), void* restrict arg);
void process_incref(process_t* process);
void process_decref(process_t* process);

/// @note Does nothing for a process that is stopped or exiting
void job_stop(process_t* process, int sig);

/// @pre The caller holds the lock of the process
/// @return Whether the process was stopped; if so, the caller calls job_notify_continued once it has dropped its locks
bool job_continue_locked(process_t* process);
void job_notify_continued(process_t* process);

/// @pre The caller holds the lock of the process and has posted THREAD_WORK_EXIT to the threads that have to leave
void job_wake_locked(process_t* process);

/// @pre The thread is on its way back to user mode, with interrupts on
void job_park(thread_t* thread);

/// For a wait that ends with -ERESTART and does not restart on its own: takes the pending signals that run no handler,
/// and parks while the process is stopped
/// @pre The thread is in a system call
/// @return Whether the wait goes on; if not, a handler is due or the thread has to leave, and the call returns -EINTR
bool job_resume_wait(thread_t* thread) __nodiscard;

/// @param bit JOB_REPORT_STOPPED or JOB_REPORT_CONTINUED
/// @return Whether the caller took the report, and so tells it to the waiter
bool job_claim_report(process_t* process, unsigned bit) __nodiscard;

/// Does the part of the exit that may sleep, once for the process: lets go of the terminal of a session leader, hangs up the groups
/// that the exit orphans and closes the files
/// @pre The caller is a thread of the process on its way out, may sleep and does not hold process_group_lock
/// @note Not in process_exit or process_kill, as a sender of SIGKILL to a group calls them under process_group_lock
void process_finish_exit(process_t* restrict process);

void process_exit(process_t* process, int exit_code);
void process_kill(process_t* process, int sig);
void process_thread_exit(process_t* process, thread_t* thread);

/// Makes every other thread of the process exit and waits for them to be reaped,
/// for exec; the caller is then the only thread, and its tid becomes the pid
/// @retval 0 The caller is the last thread
/// @retval -EINTR The process is exiting, or another
/// thread got here first; the caller is on its way out too
int process_dethread(process_t* process, thread_t* thread);

/// A thread already leaving does not count, signals and lookups treat it as gone
bool process_has_thread(process_t* process, pid_t tid) __nodiscard;

void process_thread_reaped(process_t* process, thread_t* thread);

/// Hands a zombie to exactly one waiter
/// @return Whether the caller got it, and must then call process_destroy
bool process_claim(process_t* process) __nodiscard;

/// Unlinks a claimed zombie, or a child that never ran, from the process table
/// and its parent and drops the reference process_create returned, not the caller's
void process_destroy(process_t* process);

credentials_t* process_get_cred(process_t* process) __nodiscard;
void process_set_cred(process_t* process, credentials_t* credentials);

vnode_t* process_get_cwd(process_t* process) __nodiscard;
void process_set_cwd(process_t* process, vnode_t* node);

/// @return The new reference to the root of the process, or nullptr when it is the root of the VFS
vnode_t* process_get_root(process_t* process) __nodiscard;

/// Takes over the reference of the node
void process_set_root(process_t* process, vnode_t* node);

/// Puts the address space in place, or none with nullptr, and releases the old one
/// @pre No thread of the process has the old address space loaded anymore
/// @note The peak resident size of the old address space stays with the process, for getrusage
void process_set_vmspace(process_t* process, vmspace_t* vmspace);

file_t* process_fd_get(process_t* process, int fd) __nodiscard;
int process_fd_alloc(process_t* process, file_t* file, int min_fd, uint32_t fd_flags) __nodiscard;

/// Makes new_fd another descriptor for the file of old_fd, and closes the file it had
/// @pre new_fd is not negative and differs from old_fd
/// @retval -EBADF old_fd is not open, or new_fd is past the descriptor limit
int process_fd_dup(process_t* process, int old_fd, int new_fd, uint32_t fd_flags) __nodiscard;
int process_fd_release(process_t* process, int fd);
int process_fd_get_flags(process_t* process, int fd) __nodiscard;
int process_fd_set_flags(process_t* process, int fd, uint32_t fd_flags);
void process_fds_close_on_exec(process_t* process);
int process_fds_copy(process_t* restrict process, process_t* restrict source) __nodiscard;
