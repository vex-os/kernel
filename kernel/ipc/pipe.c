#include <vex/atomic.h>
#include <vex/commit.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/fcntl.h>
#include <vex/file.h>
#include <vex/initcall.h>
#include <vex/ioctl.h>
#include <vex/kprintf.h>
#include <vex/limits.h>
#include <vex/mount.h>
#include <vex/pipe.h>
#include <vex/process.h>
#include <vex/pslab.h>
#include <vex/signal.h>
#include <vex/slab.h>
#include <vex/spinlock.h>
#include <vex/stat.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/uaccess.h>
#include <vex/vnode.h>

#define PIPEFS_TYPE     "pipefs"
#define PIPEFS_ROOT_INO 1
#define PIPE_PAGES      16
#define PIPE_MODE       (S_IFIFO | S_IRUSR | S_IWUSR)

// A write of up to this many bytes goes in all or nothing. The system call
// cuts a write into pieces of FILE_IO_CHUNK, so each piece has to qualify
#define PIPE_ATOMIC_SIZE PAGE_SIZE

static_assert(PIPE_ATOMIC_SIZE <= FILE_IO_CHUNK);

typedef struct pipe_s pipe_t;

// The data is a queue of pages, so that an empty pipe holds none and a page goes back to the
// system as soon as its last byte is read. Only the first page has a read offset and only the last
// has a write offset
struct pipe_s {
    spinlock_t lock; // Guards all the fields below
    event_t readable;
    event_t writable;
    unsigned readers; // Open files that can read
    unsigned writers; // Open files that can write
    uid_t uid;
    gid_t gid;
    void* pages[PIPE_PAGES]; // A ring: the queue starts at first
    unsigned first;
    unsigned count;
    size_t read_offset;
    size_t write_offset; // PAGE_SIZE when the last page is full, and when there is no page
    size_t used;
};

static_assert(sizeof(pipe_t) <= SLAB_MAX_SIZE);

static const vnode_ops_t pipefs_root_ops = {};

static vfs_t* pipefs = nullptr;
static uint64_t next_ino = PIPEFS_ROOT_INO + 1;

static void* page_take(void)
{
    if(commit_charge(1) < 0) {
        return nullptr;
    }

    void* page = pslab_alloc_virt();

    if(page == nullptr) {
        commit_uncharge(1);
    }

    return page;
}

static void page_give(void* restrict page)
{
    pslab_free_virt(page);
    commit_uncharge(1);
}

static size_t pipe_room(const pipe_t* restrict pipe)
{
    size_t room = (PIPE_PAGES - pipe->count) * PAGE_SIZE;

    if(pipe->write_offset < PAGE_SIZE) {
        room += PAGE_SIZE - pipe->write_offset;
    }

    return room;
}

// Where the data ends in the first page
static size_t first_limit(const pipe_t* restrict pipe)
{
    if(pipe->count == 1) {
        return pipe->write_offset;
    }

    return PAGE_SIZE;
}

static void drop_first(pipe_t* restrict pipe)
{
    page_give(pipe->pages[pipe->first]);

    pipe->pages[pipe->first] = nullptr;
    pipe->first = (pipe->first + 1) % PIPE_PAGES;
    pipe->count -= 1;
    pipe->read_offset = 0;

    if(pipe->count == 0) {
        pipe->write_offset = PAGE_SIZE;
    }
}

// Copies as much as the pipe has room and memory for
static size_t pipe_put(pipe_t* restrict pipe, const char* restrict source, size_t size)
{
    size_t done = 0;

    while(done < size) {
        if(pipe->write_offset == PAGE_SIZE) {
            if(pipe->count == PIPE_PAGES) {
                break;
            }

            void* page = page_take();

            if(page == nullptr) {
                break;
            }

            pipe->pages[(pipe->first + pipe->count) % PIPE_PAGES] = page;
            pipe->count += 1;
            pipe->write_offset = 0;
        }

        char* page = pipe->pages[(pipe->first + pipe->count - 1) % PIPE_PAGES];
        size_t chunk = size - done;

        if(chunk > PAGE_SIZE - pipe->write_offset) {
            chunk = PAGE_SIZE - pipe->write_offset;
        }

        memcpy(page + pipe->write_offset, source + done, chunk);

        pipe->write_offset += chunk;
        pipe->used += chunk;
        done += chunk;
    }

    return done;
}

static size_t pipe_get(pipe_t* restrict pipe, char* restrict target, size_t size)
{
    size_t done = 0;

    while(done < size && pipe->used) {
        const char* page = pipe->pages[pipe->first];
        size_t limit = first_limit(pipe);
        size_t chunk = limit - pipe->read_offset;

        if(chunk > size - done) {
            chunk = size - done;
        }

        memcpy(target + done, page + pipe->read_offset, chunk);

        pipe->read_offset += chunk;
        pipe->used -= chunk;
        done += chunk;

        if(pipe->read_offset == limit) {
            drop_first(pipe);
        }
    }

    return done;
}

static void send_sigpipe(void)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return;
    }

    siginfo_t info = {};
    info.si_signo = SIGPIPE;
    info.si_code = SI_KERNEL;

    signal_send(process, thread->tid, &info);
}

static ssize_t pipe_vnode_read(vnode_t* restrict node, file_t* restrict file, void* restrict buffer, size_t size, off_t offset)
{
    pipe_t* pipe = node->ctxdata;

    if(size == 0) {
        return 0;
    }

    bool intr_state = spinlock_acquire_intr(&pipe->lock);

    // No writer left and nothing in the pipe is the end of the file
    while(pipe->used == 0 && pipe->writers) {
        if(file_flags(file) & O_NONBLOCK) {
            spinlock_release_intr(&pipe->lock, intr_state);
            return -EAGAIN;
        }

        event_t* readable = &pipe->readable;
        ssize_t result = event_await_unlock(&readable, 1, &pipe->lock, intr_state, 0);

        intr_state = spinlock_acquire_intr(&pipe->lock);

        if(result < 0) {
            spinlock_release_intr(&pipe->lock, intr_state);
            return result;
        }
    }

    size_t count = pipe_get(pipe, buffer, size);

    if(count) {
        event_trigger(&pipe->writable, false);
    }

    spinlock_release_intr(&pipe->lock, intr_state);

    return count;
}

static ssize_t pipe_vnode_write(vnode_t* restrict node, file_t* restrict file, const void* restrict buffer, size_t size, off_t offset)
{
    pipe_t* pipe = node->ctxdata;
    size_t written = 0;
    ssize_t error = 0;

    bool intr_state = spinlock_acquire_intr(&pipe->lock);

    while(written < size) {
        if(pipe->readers == 0) {
            error = -EPIPE;
            break;
        }

        // Past the atomic size a write goes in as it finds room
        size_t wanted = size - written;

        if(wanted > PIPE_ATOMIC_SIZE) {
            wanted = 1;
        }

        if(pipe_room(pipe) >= wanted) {
            size_t count = pipe_put(pipe, (const char*)(buffer) + written, size - written);

            if(count == 0) {
                error = -ENOMEM;
                break;
            }

            written += count;
            event_trigger(&pipe->readable, false);
            continue;
        }

        if(file_flags(file) & O_NONBLOCK) {
            error = -EAGAIN;
            break;
        }

        event_t* writable = &pipe->writable;
        ssize_t result = event_await_unlock(&writable, 1, &pipe->lock, intr_state, 0);

        intr_state = spinlock_acquire_intr(&pipe->lock);

        if(result < 0) {
            error = result;
            break;
        }
    }

    spinlock_release_intr(&pipe->lock, intr_state);

    if(error == -EPIPE) {
        send_sigpipe();
    }

    if(written) {
        return written;
    }

    return error;
}

static off_t pipe_vnode_seek(vnode_t* restrict node, file_t* restrict file, off_t offset, int origin)
{
    return -ESPIPE;
}

static int pipe_vnode_open(vnode_t* restrict node, file_t* restrict file)
{
    pipe_t* pipe = node->ctxdata;
    unsigned flags = file_flags(file);

    bool intr_state = spinlock_acquire_intr(&pipe->lock);

    if(file_flags_allow_read(flags)) {
        pipe->readers += 1;
    }

    if(file_flags_allow_write(flags)) {
        pipe->writers += 1;
    }

    spinlock_release_intr(&pipe->lock, intr_state);

    return 0;
}

// The last reader of a pipe leaves its writers with EPIPE, and the last writer leaves its readers with the end of the file
static int pipe_vnode_close(vnode_t* restrict node, file_t* restrict file)
{
    pipe_t* pipe = node->ctxdata;
    unsigned flags = file_flags(file);

    bool intr_state = spinlock_acquire_intr(&pipe->lock);

    if(file_flags_allow_read(flags)) {
        pipe->readers -= 1;

        if(pipe->readers == 0) {
            event_trigger(&pipe->writable, false);
        }
    }

    if(file_flags_allow_write(flags)) {
        pipe->writers -= 1;

        if(pipe->writers == 0) {
            event_trigger(&pipe->readable, false);
        }
    }

    spinlock_release_intr(&pipe->lock, intr_state);

    return 0;
}

static int pipe_vnode_ioctl(vnode_t* restrict node, file_t* restrict file, uint32_t request, uintptr_t argument)
{
    pipe_t* pipe = node->ctxdata;

    if(request != FIONREAD) {
        return -ENOTTY;
    }

    bool intr_state = spinlock_acquire_intr(&pipe->lock);
    size_t used = pipe->used;
    spinlock_release_intr(&pipe->lock, intr_state);

    int32_t value = used;

    if(user_copyout(argument, &value, sizeof(value)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static int pipe_vnode_stat(vnode_t* restrict node, stat_t* restrict out)
{
    const pipe_t* pipe = node->ctxdata;

    memset(out, 0, sizeof(stat_t));

    out->st_dev = node->fs->dev;
    out->st_ino = node->ino;
    out->st_mode = PIPE_MODE;
    out->st_nlink = 1;
    out->st_uid = pipe->uid;
    out->st_gid = pipe->gid;
    out->st_blksize = PAGE_SIZE;

    return 0;
}

static void pipe_vnode_release(vnode_t* restrict node)
{
    pipe_t* pipe = node->ctxdata;

    // The readers closed with data in the pipe
    while(pipe->count) {
        drop_first(pipe);
    }

    event_deinit(&pipe->readable);
    event_deinit(&pipe->writable);

    slab_free(pipe);
}

static const vnode_ops_t pipe_vnode_ops = {
    .release = &pipe_vnode_release,
    .open = &pipe_vnode_open,
    .close = &pipe_vnode_close,
    .read = &pipe_vnode_read,
    .write = &pipe_vnode_write,
    .seek = &pipe_vnode_seek,
    .ioctl = &pipe_vnode_ioctl,
    .stat = &pipe_vnode_stat,
};

static int pipefs_mount(vfs_t* restrict fs, const block_interface_t* restrict volume, const cmdline_t* restrict params)
{
    vnode_t* root = vnode_alloc(VNODE_DIRECTORY, &pipefs_root_ops, nullptr);

    if(root == nullptr) {
        return -ENOMEM;
    }

    root->fs = fs;
    root->ino = PIPEFS_ROOT_INO;

    fs->root = root;

    return 0;
}

static int pipefs_unmount(vfs_t* restrict fs)
{
    vnode_decref(fs->root);

    return 0;
}

static const vfs_ops_t pipefs_ops = {
    .mount = &pipefs_mount,
    .unmount = &pipefs_unmount,
};

static vfs_driver_t pipefs_driver = {
    .name = PIPEFS_TYPE,
    .ops = &pipefs_ops,
    .internal = true,
};

int pipe_create(unsigned flags, const credentials_t* restrict credentials, file_t** restrict read_end, file_t** restrict write_end)
{
    if(pipefs == nullptr) {
        return -ENOSYS;
    }

    pipe_t* pipe = slab_alloc(sizeof(pipe_t));

    if(pipe == nullptr) {
        return -ENOMEM;
    }

    pipe->lock = SPINLOCK_INIT;
    pipe->write_offset = PAGE_SIZE;
    pipe->uid = credentials->euid;
    pipe->gid = credentials->egid;

    event_init(&pipe->readable);
    event_init(&pipe->writable);

    vnode_t* node = vnode_alloc(VNODE_FIFO, &pipe_vnode_ops, pipe);

    if(node == nullptr) {
        event_deinit(&pipe->readable);
        event_deinit(&pipe->writable);
        slab_free(pipe);
        return -ENOMEM;
    }

    node->fs = pipefs;
    node->ino = atomic_fetch_add(&next_ino, 1, ATOMIC_RELAXED);

    unsigned nonblock = flags & O_NONBLOCK;

    // Each end takes a reference; the pipe lives in the node
    vnode_incref(node);

    file_t* reader;
    int retval = file_open_node(node, O_RDONLY | nonblock, credentials, &reader);

    if(retval < 0) {
        vnode_decref(node);
        return retval;
    }

    file_t* writer;
    retval = file_open_node(node, O_WRONLY | nonblock, credentials, &writer);

    if(retval < 0) {
        file_close(reader);
        return retval;
    }

    *read_end = reader;
    *write_end = writer;

    return 0;
}

static void init_pipefs_driver(void)
{
    vfs_driver_add(&pipefs_driver);
}

initcall_bsp_vfs(init_pipefs_driver, INIT_EARLY);

static void init_pipefs(void)
{
    int retval = vfs_create(PIPEFS_TYPE, nullptr, &pipefs);

    if(retval < 0) {
        pr_warning("pipefs: no %s: %s", PIPEFS_TYPE, strerror(retval));
    }
}

initcall_bsp_fs(init_pipefs, INIT_EARLY);
