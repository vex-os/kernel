#pragma once

#include <arch/hart.h>
#include <arch/thread.h>
#include <vex/compiler.h>
#include <vex/cputime.h>
#include <vex/signal.h>
#include <vex/spinlock.h>
#include <vex/types.h>

typedef struct credentials_s credentials_t;
typedef struct event_s event_t;
typedef struct pending_signal_s pending_signal_t;
typedef struct process_s process_t;
typedef struct hart_local_s hart_local_t;
typedef struct runqueue_s runqueue_t;

#define THREAD_WORK_EXIT   UINT32_C(0x00000001)
#define THREAD_WORK_SIGNAL UINT32_C(0x00000002)
#define THREAD_WORK_STOP   UINT32_C(0x00000004)

enum {
    THREAD_RUNNABLE,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD,
};

// The saved context lives at the very top of the kernel stack, where nothing
// running on that stack reaches, so the thread itself stays small
#define THREAD_CTX_RESERVE align_ceil(sizeof(hart_context_t), 16)

typedef struct thread_s thread_t;

struct thread_s {
    thread_t* self;
    void* kernel_rsp;
    void* entry_scratch;

    pid_t tid;
    uint16_t state; // Under sched_lock
    bool need_resched;
    bool wait_interrupt;
    uint16_t preempt_count; // Spinlocks held, the thread is not preempted while nonzero
    bool syscall_restart;   // The syscall being returned from was rewound to run again

    spinlock_t sched_lock;
    spinlock_t hart_lock; // Held while the thread is on a hart
    spinlock_t yield_await;

    hart_context_t* syscall_ctx;

    hart_local_t* last_hart;  // The hart this thread runs on or last to be running on
    hart_local_t* bound_hart; // The hart this thread MUST run on, nullptr otherwise

    runqueue_t* runqueue;
    thread_t* runqueue_next;
    thread_t* runqueue_prev;

    event_t** events;
    uint32_t wakeup_index;
    uint32_t num_events;
    uint32_t max_events;
    uint32_t pending_work; // loaded every return to ring-3

    uint64_t wake_deadline;
    thread_t* wake_next;

    process_t* process;
    credentials_t* credentials;
    thread_t* process_next;
    thread_t* process_prev;

    sigset_t sig_blocked;
    sigset_t sig_saved; // The mask that sigsuspend replaced, valid while sig_saved_valid
    sigset_t sig_pending;
    pending_signal_t* sig_queue;
    uintptr_t sig_altstack;
    uint32_t sig_altstack_size;
    bool sig_saved_valid;

    cputime_t cputime; // Changed only on the hart that runs the thread

    uintptr_t clear_tid; // Zeroed and woken as a futex when the thread exits

    void* kstack_top;
    void* io_page; // The bounce buffer of read and write system calls, which never nest; allocated on first use

    arch_thread_t arch;
};

static_assert((0 * sizeof(uintptr_t)) == offsetof(thread_t, self));
static_assert((1 * sizeof(uintptr_t)) == offsetof(thread_t, kernel_rsp));
static_assert((2 * sizeof(uintptr_t)) == offsetof(thread_t, entry_scratch));

/// Handles the thread's pending work, exit or signals, before it returns to user mode
/// @pre Interrupts are disabled
/// @post Interrupts are disabled
/// @note This way, work that is posted after the last check reaches the thread before it returns to ring 3
void exit_to_user(hart_context_t* restrict ctx);

static __always_inline __nodiscard hart_context_t* thread_context(const thread_t* restrict thread)
{
    return (hart_context_t*)((uintptr_t)(thread->kstack_top) - THREAD_CTX_RESERVE);
}
