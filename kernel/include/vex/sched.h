#pragma once

#include <arch/sched.h>
#include <usr/vex/sched.h>
#include <vex/hart.h>
#include <vex/types.h>

typedef struct hart_context_s hart_context_t;
typedef struct thread_s thread_t;
typedef struct process_s process_t;

typedef struct __k_sched_param sched_param_t;
typedef struct __k_sched_range sched_range_t;

#define PRIO_PROCESS __k_PRIO_PROCESS
#define PRIO_PGRP    __k_PRIO_PGRP
#define PRIO_USER    __k_PRIO_USER

#define NICE_MIN  __k_NICE_MIN
#define NICE_MAX  __k_NICE_MAX
#define NICE_BIAS __k_NICE_BIAS

#define SCHED_OTHER __k_SCHED_OTHER
#define SCHED_FIFO  __k_SCHED_FIFO
#define SCHED_RR    __k_SCHED_RR
#define SCHED_BATCH __k_SCHED_BATCH
#define SCHED_IDLE  __k_SCHED_IDLE

/// The words of the affinity mask of a thread:
/// a bit for each hart that the kernel can have
#define SCHED_AFFINITY_WORDS ((MAX_HARTS + 63U) / 64U)

typedef void (*kernel_thread_entry_t)(void* restrict arg);

/// How long a thread of a process with this nice value runs before the scheduler lets another thread on its hart
/// @param nice Outside NICE_MIN to NICE_MAX it counts as the nearest end
uint64_t sched_timeslice_for_nice(int nice) __nodiscard;

/// The new thread stays blocked until sched_enqueue
thread_t* thread_create_kernel(kernel_thread_entry_t entry, void* restrict arg);

/// thread_create_kernel, then sched_enqueue
thread_t* thread_spawn_kernel(kernel_thread_entry_t entry, void* restrict arg);

/// The new thread stays blocked until sched_enqueue
thread_t* thread_create_user(process_t* restrict process, uintptr_t entry, uintptr_t stack_top);

/// thread_create_user, then sched_enqueue
thread_t* thread_spawn_user(process_t* restrict process, uintptr_t entry, uintptr_t stack_top);

/// The new thread stays blocked until sched_enqueue, and starts with the affinity of the creator
/// @param ctx The parent's system call frame, which the child resumes from
thread_t* thread_create_fork(process_t* restrict process, const thread_t* restrict creator, const hart_context_t* restrict ctx);

/// The new thread stays blocked until sched_enqueue, and starts with the signal mask and the affinity of the creator
thread_t* thread_create_sibling(const thread_t* restrict creator, uintptr_t entry, uintptr_t stack_top, uintptr_t arg, uintptr_t tcb);

/// The first thread of a spawned process: like a sibling, but of a process that has no threads yet
thread_t* thread_create_spawn(process_t* restrict process, const thread_t* restrict creator, uintptr_t entry, uintptr_t stack_top);

/// Gives the thread a mask of its own for every hart if it has none yet, so that thread_set_affinity cannot fail
/// @pre The caller keeps the thread from being reaped, as the process lock held does
/// @retval -ENOMEM There is no memory for the mask
int thread_reserve_affinity(thread_t* restrict thread);

/// Lets the thread run only on the harts of the mask, and moves it off a hart that the mask leaves out
/// @param mask SCHED_AFFINITY_WORDS words, with a bit set for at least one hart that is online
/// @pre thread_reserve_affinity succeeded for the thread, and the caller keeps the thread from being reaped
void thread_set_affinity(thread_t* restrict thread, const uint64_t* restrict mask);

/// Fills SCHED_AFFINITY_WORDS words; a thread with no mask of its own gets every bit set
/// @pre As for thread_set_affinity
void thread_get_affinity(const thread_t* restrict thread, uint64_t* restrict mask);

/// Wakes a blocked thread, whatever it waits for. Does nothing to a thread that is not blocked
/// @note Any context, with interrupts disabled or spinlocks held
void sched_enqueue(thread_t* restrict thread);

/// Marks the calling thread blocked, but does not switch away
/// The protocol: register as a waiter under the lock of the waker, call this before the lock is released,
/// call sched_yield, then check the condition again
/// @param interruptible sched_kick can end the wait; if work is already pending, the thread is not blocked
/// @note A wakeup can be spurious. Calling this before the release of the lock stops a wakeup from getting lost
void sched_dequeue(thread_t* restrict thread, bool interruptible);

/// Makes the thread see work that was just posted to its pending_work
/// @pre The work is already posted
/// @note Wakes the thread from an interruptible wait, or interrupts its hart so that it passes through exit_to_user
void sched_kick(thread_t* restrict thread);

/// Wakes the thread with sched_enqueue once the deadline passes
/// @post The thread stays in the list until the deadline, also when something else wakes it first
/// @note Every call needs a sched_wake_cancel after the wait
/// @param deadline Absolute time on the clock_source_now_ns scale
void sched_wake_at(thread_t* restrict thread, uint64_t deadline);

/// Takes the thread off the list sched_wake_at put it on, if it is still there
void sched_wake_cancel(thread_t* restrict thread);

/// Ends the calling thread; its hart frees it on the next scheduler tick
/// @pre A user thread has already left its process, through process_thread_exit or THREAD_WORK_EXIT
void sched_terminate(void) __noreturn;

/// Gives up the hart until the scheduler picks the thread again; after sched_dequeue, until a wakeup
/// @pre Interrupts are enabled and no spinlock is held
/// @note The switch is a self-IPI
void sched_yield(void);
