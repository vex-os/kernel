#include <vex/errno.h>
#include <vex/event.h>
#include <vex/file.h>
#include <vex/poll.h>
#include <vex/process.h>
#include <vex/pslab.h>
#include <vex/rlimit.h>
#include <vex/signal.h>
#include <vex/syscall.h>
#include <vex/thread.h>
#include <vex/time.h>
#include <vex/uaccess.h>

#include "syscall_internal.h"

#define POLL_STACK_ENTRIES 8
#define POLL_PAGE_ENTRIES  ((PAGE_SIZE - sizeof(poll_chunk_t)) / sizeof(poll_entry_t))
#define POLL_ALWAYS        (POLLERR | POLLHUP) // Reported even when not asked for

typedef struct poll_entry_s poll_entry_t;
typedef struct poll_chunk_s poll_chunk_t;

struct poll_entry_s {
    pollfd_t request;
    file_t* file; // Holds a reference until the call ends, so the events in the table stay alive
    poll_table_t table;
};

// The first chunk is on the stack. Each other one takes a page from pslab, with the entries after the header
struct poll_chunk_s {
    poll_chunk_t* next;
    size_t count;
    size_t capacity;
    poll_entry_t* entries;
};

static_assert(POLL_PAGE_ENTRIES >= POLL_STACK_ENTRIES);

static poll_chunk_t* chunk_alloc(void)
{
    poll_chunk_t* chunk = pslab_alloc_virt();

    if(chunk == nullptr) {
        return nullptr;
    }

    chunk->next = nullptr;
    chunk->count = 0;
    chunk->capacity = POLL_PAGE_ENTRIES;
    chunk->entries = (poll_entry_t*)(chunk + 1);

    return chunk;
}

static void chunks_free(poll_chunk_t* restrict first)
{
    poll_chunk_t* chunk = first;

    while(chunk) {
        poll_chunk_t* next = chunk->next;

        for(size_t i = 0; i < chunk->count; ++i) {
            file_t* file = chunk->entries[i].file;

            if(file) {
                file_close(file);
            }
        }

        if(chunk != first) {
            pslab_free_virt(chunk);
        }

        chunk = next;
    }
}

static int load_entry(poll_entry_t* restrict entry, process_t* restrict process, user_addr_t source)
{
    entry->file = nullptr;

    ssize_t copied = user_copyin(&entry->request, source, sizeof(pollfd_t));

    if(copied < 0) {
        return copied;
    }

    entry->request.revents = 0;

    if(entry->request.fd >= 0) {
        entry->file = process_fd_get(process, entry->request.fd);
    }

    return 0;
}

static int load_chunks(poll_chunk_t* restrict first, process_t* restrict process, user_addr_t fds, size_t count)
{
    poll_chunk_t* chunk = first;

    for(size_t i = 0; i < count; ++i) {
        if(chunk->count == chunk->capacity) {
            chunk->next = chunk_alloc();

            if(chunk->next == nullptr) {
                return -ENOMEM;
            }

            chunk = chunk->next;
        }

        poll_entry_t* entry = &chunk->entries[chunk->count];
        user_addr_t source = user_addr_add(fds, i * sizeof(pollfd_t));
        int retval = load_entry(entry, process, source);

        // Counted even on a failure, as chunks_free closes the file of each counted entry
        chunk->count += 1;

        if(retval < 0) {
            return retval;
        }
    }

    return 0;
}

static unsigned check_entry(poll_entry_t* restrict entry)
{
    entry->table.count = 0;

    if(entry->request.fd < 0) {
        return 0;
    }

    if(entry->file == nullptr) {
        return POLLNVAL;
    }

    unsigned wanted = (uint16_t)(entry->request.events) | POLL_ALWAYS;
    unsigned ready = file_poll(entry->file, &entry->table);

    return ready & wanted;
}

// Fills revents of every entry
// @return The number of entries with events
static size_t check_all(poll_chunk_t* restrict first, size_t* restrict watched)
{
    size_t ready = 0;

    *watched = 0;

    for(poll_chunk_t* chunk = first; chunk; chunk = chunk->next) {
        for(size_t i = 0; i < chunk->count; ++i) {
            poll_entry_t* entry = &chunk->entries[i];

            entry->request.revents = check_entry(entry);
            *watched += entry->table.count;

            if(entry->request.revents) {
                ready += 1;
            }
        }
    }

    return ready;
}

static ssize_t wait_all(poll_chunk_t* restrict first, size_t watched, uint64_t deadline)
{
    event_watch_t watch;
    int retval = event_watch_begin(&watch, watched);

    if(retval < 0) {
        return retval;
    }

    for(poll_chunk_t* chunk = first; chunk; chunk = chunk->next) {
        for(size_t i = 0; i < chunk->count; ++i) {
            const poll_table_t* table = &chunk->entries[i].table;

            for(unsigned k = 0; k < table->count; ++k) {
                event_watch_add(&watch, table->events[k], table->generations[k]);
            }
        }
    }

    return event_watch_wait(&watch, deadline);
}

static int store_results(const poll_chunk_t* restrict first, user_addr_t fds)
{
    size_t index = 0;

    for(const poll_chunk_t* chunk = first; chunk; chunk = chunk->next) {
        for(size_t i = 0; i < chunk->count; ++i) {
            size_t offset = index * sizeof(pollfd_t) + offsetof(pollfd_t, revents);
            user_addr_t target = user_addr_add(fds, offset);
            const int16_t* revents = &chunk->entries[i].request.revents;

            if(user_copyout(target, revents, sizeof(*revents)) < 0) {
                return -EFAULT;
            }

            index += 1;
        }
    }

    return 0;
}

// A timeout of nullptr waits for as long as it takes, and a zero one does not wait
static int read_deadline(user_addr_t timeout, bool* restrict waits, uint64_t* restrict deadline)
{
    *waits = true;
    *deadline = 0;

    if(timeout.value == 0) {
        return 0;
    }

    timespec_t relative;
    ssize_t copied = user_copyin(&relative, timeout, sizeof(relative));

    if(copied < 0) {
        return copied;
    }

    if(!timespec_valid(&relative)) {
        return -EINVAL;
    }

    uint64_t nsec = timespec_to_nsec(&relative);

    if(nsec == 0) {
        *waits = false;
        return 0;
    }

    *deadline = deadline_after_nsec(nsec);

    return 0;
}

static ssize_t poll_loop(poll_chunk_t* restrict first, bool waits, uint64_t deadline, const sigset_t* restrict mask)
{
    thread_t* thread = this_thread();

    for(;;) {
        size_t watched;
        size_t ready = check_all(first, &watched);

        if(ready || !waits) {
            return ready;
        }

        ssize_t result = wait_all(first, watched, deadline);

        if(result == -ETIMEDOUT) {
            return 0;
        }

        if(result == -ERESTART && job_resume_wait(thread)) {
            // A signal that was ignored by now has put the old mask back
            if(mask) {
                signal_replace_mask(thread, *mask);
            }

            continue;
        }

        if(result == -ERESTART) {
            return -EINTR;
        }

        if(result < 0) {
            return result;
        }
    }
}

static intptr_t sys_ppoll(user_addr_t fds, size_t count, user_addr_t timeout, user_addr_t mask)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -ENOSYS;
    }

    rlimit_t limit = process_get_rlimit(process, RLIMIT_NOFILE);

    if(count > limit.rlim_cur) {
        return -EINVAL;
    }

    bool waits;
    uint64_t deadline;
    int retval = read_deadline(timeout, &waits, &deadline);

    if(retval < 0) {
        return retval;
    }

    sigset_t new_mask;
    sigset_t* wait_mask = nullptr;

    if(mask.value) {
        ssize_t copied = user_copyin(&new_mask, mask, sizeof(new_mask));

        if(copied < 0) {
            return copied;
        }

        wait_mask = &new_mask;
    }

    poll_entry_t stack_entries[POLL_STACK_ENTRIES];

    poll_chunk_t first = {};
    first.capacity = POLL_STACK_ENTRIES;
    first.entries = stack_entries;

    ssize_t result = load_chunks(&first, process, fds, count);

    if(result < 0) {
        chunks_free(&first);
        return result;
    }

    if(wait_mask) {
        signal_replace_mask(thread, new_mask);
    }

    result = poll_loop(&first, waits, deadline, wait_mask);

    // The signal that ends the wait runs its handler on the
    // way out, and the handler's return restores the mask
    if(result != -EINTR) {
        signal_restore_mask(thread);
    }

    if(result >= 0 || result == -EINTR) {
        retval = store_results(&first, fds);

        if(retval < 0) {
            result = retval;
        }
    }

    chunks_free(&first);

    return result;
}

define_syscall(sys_ppoll, SYS_PPOLL, 4);
