#include <vex/binfmt.h>
#include <vex/errno.h>
#include <vex/file.h>
#include <vex/kprintf.h>
#include <vex/page.h>
#include <vex/rlimit.h>
#include <vex/spinlock.h>

static spinlock_t binfmt_lock = SPINLOCK_INIT;
static binfmt_t* binfmt_head = nullptr;

static binfmt_t* next_binfmt(const binfmt_t* restrict binfmt)
{
    binfmt_t* next;

    spinlock_acquire(&binfmt_lock);

    if(binfmt) {
        next = binfmt->priv.next;
    }
    else {
        next = binfmt_head;
    }

    spinlock_release(&binfmt_lock);

    return next;
}

int binfmt_add(binfmt_t* restrict binfmt)
{
    spinlock_acquire(&binfmt_lock);

    for(binfmt_t* it = binfmt_head; it; it = it->priv.next) {
        if(it == binfmt) {
            spinlock_release(&binfmt_lock);
            return -EEXIST;
        }
    }

    binfmt->priv.next = binfmt_head;
    binfmt_head = binfmt;

    spinlock_release(&binfmt_lock);

    pr_verbose("binfmt: adding %s", binfmt->name);

    return 0;
}

int binfmt_remove(binfmt_t* restrict binfmt)
{
    spinlock_acquire(&binfmt_lock);

    for(binfmt_t** prev = &binfmt_head; *prev; prev = &(*prev)->priv.next) {
        if(*prev == binfmt) {
            *prev = binfmt->priv.next;
            spinlock_release(&binfmt_lock);
            return 0;
        }
    }

    spinlock_release(&binfmt_lock);

    return -ENOENT;
}

#define STACK_SIZE_MIN (UINT64_C(64) * 1024)
#define STACK_SIZE_MAX (UINT64_C(1024) * 1024 * 1024)

// The program has all of its stack from the start, and the arguments take the top of it
static size_t stack_size_of(uint64_t limit)
{
    if(limit == RLIM_INFINITY) {
        limit = RLIMIT_STACK_DEFAULT;
    }

    if(limit < STACK_SIZE_MIN) {
        limit = STACK_SIZE_MIN;
    }

    if(limit > STACK_SIZE_MAX) {
        limit = STACK_SIZE_MAX;
    }

    return page_align_up_number(limit);
}

int load_binary(vmspace_t* restrict vmspace, file_t* restrict file, const char** restrict argv, const char** restrict envp,
    const credentials_t* restrict credentials, uint64_t stack_limit, binfmt_entry_t* restrict out)
{
    size_t stack_size = stack_size_of(stack_limit);

    uint8_t header[256];
    ssize_t read = file_read(file, header, sizeof(header), &file->offset);

    file_seek(file, 0, SEEK_SET);

    if(read < 0) {
        return read;
    }

    for(binfmt_t* it = next_binfmt(nullptr); it; it = next_binfmt(it)) {
        if(it->ops->probe && it->ops->setup) {
            int retval = it->ops->probe(file, header, (size_t)(read));

            if(retval == -ENOEXEC) {
                continue;
            }

            if(retval < 0) {
                return retval;
            }

            return it->ops->setup(vmspace, file, argv, envp, credentials, stack_size, out);
        }
    }

    return -ENOEXEC;
}
