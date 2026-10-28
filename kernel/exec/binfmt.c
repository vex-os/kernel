#include <vex/binfmt.h>
#include <vex/errno.h>
#include <vex/file.h>
#include <vex/kprintf.h>
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

int load_binary(vmspace_t* restrict vmspace, file_t* restrict file, const char** restrict argv, const char** restrict envp,
    const credentials_t* restrict credentials, binfmt_entry_t* restrict out)
{
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

            return it->ops->setup(vmspace, file, argv, envp, credentials, out);
        }
    }

    return -ENOEXEC;
}
