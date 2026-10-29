#include <arch/hart.h>
#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/capability.h>
#include <vex/clocksrc.h>
#include <vex/cred.h>
#include <vex/errno.h>
#include <vex/fcntl.h>
#include <vex/format.h>
#include <vex/initcall.h>
#include <vex/ioctl.h>
#include <vex/panic.h>
#include <vex/pgrp.h>
#include <vex/process.h>
#include <vex/pslab.h>
#include <vex/sched.h>
#include <vex/signal.h>
#include <vex/string.h>
#include <vex/thread.h>
#include <vex/tty_discipline.h>
#include <vex/uaccess.h>

#define VTIME_UNIT_NSEC UINT64_C(100000000) // VTIME counts tenths of a second

#define CLOSE_DRAIN_NSEC UINT64_C(30000000000) // As Linux's closing_wait; output left after it is dropped

#define DEFAULT_SPEED 38400

#define STATUS_LINE_SIZE   128
#define STATUS_MEMBERS_MAX 4

#define CONTROL(letter) ((letter) & 0x1F)

typedef struct status_line_s status_line_t;
typedef struct ctty_drop_s ctty_drop_t;

struct status_line_s {
    char text[STATUS_LINE_SIZE];
    size_t length;
};

// References a detached controlling terminal leaves
// behind, dropped once process_group_lock is released
struct ctty_drop_s {
    session_t* session;
    process_group_t* foreground;
    tty_interface_t* interface;
};

static spinlock_t worker_lock = SPINLOCK_INIT; // Taken with interrupts disabled
static tty_interface_t* worker_head = nullptr;
static tty_interface_t* worker_tail = nullptr;
static event_t worker_wakeup;

static tty_interface_t* tty_of(const char_interface_t* restrict chr)
{
    return chr->ctxdata;
}

static void leave(tty_interface_t* restrict interface)
{
    atomic_fetch_sub(&interface->priv.in_flight, 1, ATOMIC_RELEASE);
}

static bool enter(tty_interface_t* restrict interface)
{
    atomic_fetch_add(&interface->priv.in_flight, 1, ATOMIC_SEQ_CST);

    if(atomic_load(&interface->priv.removed, ATOMIC_SEQ_CST)) {
        leave(interface);
        return false;
    }

    return true;
}

static bool is_removed(const tty_interface_t* restrict interface)
{
    return atomic_load(&interface->priv.removed, ATOMIC_ACQUIRE);
}

static void set_default_termios(termios_t* restrict termios, uint32_t speed)
{
    memset(termios, 0, sizeof(*termios));

    termios->c_iflag = ICRNL | IXON | IMAXBEL;
    termios->c_oflag = OPOST | ONLCR;
    termios->c_cflag = CS8 | CREAD | HUPCL;
    termios->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;

    termios->c_cc[VINTR] = CONTROL('C');
    termios->c_cc[VQUIT] = CONTROL('\\');
    termios->c_cc[VERASE] = 0x7F;
    termios->c_cc[VKILL] = CONTROL('U');
    termios->c_cc[VEOF] = CONTROL('D');
    termios->c_cc[VSTART] = CONTROL('Q');
    termios->c_cc[VSTOP] = CONTROL('S');
    termios->c_cc[VSUSP] = CONTROL('Z');
    termios->c_cc[VREPRINT] = CONTROL('R');
    termios->c_cc[VWERASE] = CONTROL('W');
    termios->c_cc[VLNEXT] = CONTROL('V');
    termios->c_cc[VDISCARD] = CONTROL('O');
    termios->c_cc[VMIN] = 1;
    termios->c_cc[VTIME] = 0;
    termios->c_cc[VSTATUS] = CONTROL('T');

    if(speed == 0) {
        speed = DEFAULT_SPEED;
    }

    termios->c_ispeed = speed;
    termios->c_ospeed = speed;
}

static void discard_flip_locked(tty_interface_t* restrict interface)
{
    tty_interface_private_t* priv = &interface->priv;

    priv->flip.head = priv->flip.tail;
    memset(priv->flip_errors, 0, sizeof(priv->flip_errors));
}

static void reset_rings_locked(tty_interface_t* restrict interface)
{
    tty_interface_private_t* priv = &interface->priv;

    discard_flip_locked(interface);
    priv->output.head = priv->output.tail;
    priv->xchar = 0;
    priv->output_stopped = false;
    priv->signals = 0;

    tty_discipline_reset(interface);
}

static void notify_input_locked(tty_interface_t* restrict interface)
{
    if(tty_discipline_readable(interface)) {
        event_trigger(&interface->priv.readable, false);
    }
}

// The caller calls start_tx once it dropped the lock when this says so
static bool notify_output_locked(tty_interface_t* restrict interface)
{
    const tty_ring_t* output = &interface->priv.output;
    bool pending = tty_ring_used(output);

    if(tty_ring_room(output) >= TTY_EXPANSION_MAX) {
        event_trigger(&interface->priv.writable, false);
    }

    if(!pending) {
        event_trigger(&interface->priv.drained, false);
    }

    // A flow control character goes out even while the output is stopped
    return interface->priv.xchar || (pending && !interface->priv.output_stopped);
}

static void start_tx(tty_interface_t* restrict interface, bool transmit)
{
    if(transmit && interface->ops->start_tx) {
        interface->ops->start_tx(interface);
    }
}

static void queue_for_worker(tty_interface_t* restrict interface)
{
    bool intr_state = spinlock_acquire_intr(&worker_lock);
    bool was_queued = interface->priv.queued;

    if(!was_queued) {
        char_interface_incref(&interface->char_interface);

        interface->priv.queued = true;
        interface->priv.worker_next = nullptr;

        if(worker_tail) {
            worker_tail->priv.worker_next = interface;
        }
        else {
            worker_head = interface;
        }

        worker_tail = interface;
    }

    spinlock_release_intr(&worker_lock, intr_state);

    if(!was_queued) {
        event_trigger(&worker_wakeup, true);
    }
}

// Hands out the queue's reference to the device
static tty_interface_t* worker_pop(void)
{
    bool intr_state = spinlock_acquire_intr(&worker_lock);
    tty_interface_t* interface = worker_head;

    if(interface) {
        worker_head = interface->priv.worker_next;

        if(worker_head == nullptr) {
            worker_tail = nullptr;
        }

        interface->priv.queued = false;
    }

    spinlock_release_intr(&worker_lock, intr_state);

    return interface;
}

static void status_append(int character, void* restrict user)
{
    status_line_t* line = user;

    if(line->length + 1 < sizeof(line->text)) {
        line->text[line->length] = character;
        line->length += 1;
    }
}

static const char* process_state(process_t* restrict process)
{
    uint8_t flags = atomic_load_n(&process->flags, ATOMIC_ACQUIRE);

    if(flags & PROCESS_ZOMBIE) {
        return "zombie";
    }

    if(flags & PROCESS_EXITING) {
        return "exiting";
    }

    return "running";
}

// Under process_group_lock, which keeps the foreground group and its members still
static void format_status_locked(const tty_interface_t* restrict interface, status_line_t* restrict line)
{
    const process_group_t* foreground = interface->priv.foreground;

    if(foreground == nullptr) {
        format(&status_append, line, "no foreground process group\n");
        return;
    }

    format(&status_append, line, "process group %d:", (int)(foreground->pgid));

    unsigned count = 0;

    for(process_t* it = foreground->members; it; it = it->group_next) {
        if(count == STATUS_MEMBERS_MAX) {
            format(&status_append, line, " ...");
            break;
        }

        format(&status_append, line, " %d (%s)", (int)(it->pid), process_state(it));
        count += 1;
    }

    format(&status_append, line, "\n");
}

static void report_status(tty_interface_t* restrict interface)
{
    status_line_t line = { 0 };

    process_group_lock_acquire();
    format_status_locked(interface, &line);
    process_group_lock_release();

    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);

    for(size_t i = 0; i < line.length; ++i) {
        if(!tty_discipline_output(interface, line.text[i])) {
            break;
        }
    }

    bool transmit = notify_output_locked(interface);

    spinlock_release_intr(&interface->priv.lock, intr_state);

    start_tx(interface, transmit);
}

static void send_signals(tty_interface_t* restrict interface, uint32_t signals)
{
    process_group_lock_acquire();

    process_group_t* foreground = interface->priv.foreground;

    for(int sig = 1; foreground && sig < 32; sig += 1) {
        if(signals & (1U << sig)) {
            process_group_signal_locked(foreground, sig);
        }
    }

    process_group_lock_release();
}

static bool take_error_mark(tty_interface_t* restrict interface)
{
    bitmap_t* flip_errors = interface->priv.flip_errors;
    uint32_t position = interface->priv.flip.head % TTY_RING_SIZE;

    if(!bitmap_isset(flip_errors, position)) {
        return false;
    }

    bitmap_clear(flip_errors, position);

    return true;
}

static void process_input(tty_interface_t* restrict interface)
{
    if(!enter(interface)) {
        return;
    }

    tty_ring_t* flip = &interface->priv.flip;

    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);

    while(tty_ring_used(flip)) {
        unsigned error = TTY_ERROR_NONE;

        if(take_error_mark(interface)) {
            error = tty_ring_take(flip);
        }

        uint8_t byte = tty_ring_take(flip);

        tty_discipline_receive(interface, byte, error);
    }

    uint32_t signals = interface->priv.signals;
    uint32_t lflag = interface->priv.termios.c_lflag;

    interface->priv.signals = 0;

    notify_input_locked(interface);
    bool transmit = notify_output_locked(interface);

    spinlock_release_intr(&interface->priv.lock, intr_state);

    start_tx(interface, transmit);

    if(signals) {
        send_signals(interface, signals);
    }

    if((signals & (1U << SIGINFO)) && 0 == (lflag & NOKERNINFO)) {
        report_status(interface);
    }

    leave(interface);
}

// Runs the line discipline where it may sleep and
// look up process groups, which an interrupt handler can't
static void __noreturn tty_worker(void* restrict arg)
{
    for(;;) {
        event_t* wakeup = &worker_wakeup;
        event_await_blocking(&wakeup, 1);

        for(tty_interface_t* interface = worker_pop(); interface; interface = worker_pop()) {
            process_input(interface);
            char_interface_decref(&interface->char_interface);
        }
    }
}

static process_t* calling_process(void)
{
    thread_t* thread = this_thread();
    return thread->process;
}

// Under process_group_lock
static void detach_locked(tty_interface_t* restrict interface, ctty_drop_t* restrict drop)
{
    session_t* session = interface->priv.session;

    drop->session = session;
    drop->foreground = interface->priv.foreground;
    drop->interface = interface;

    session->tty = nullptr;
    interface->priv.session = nullptr;
    interface->priv.foreground = nullptr;
}

static void drop_detached(const ctty_drop_t* restrict drop)
{
    if(drop->session) {
        session_decref(drop->session);
    }

    if(drop->foreground) {
        process_group_decref(drop->foreground);
    }

    if(drop->interface) {
        char_interface_decref(&drop->interface->char_interface);
    }
}

// Under process_group_lock
static bool is_controlling_locked(const tty_interface_t* restrict interface, process_t* restrict process)
{
    process_group_t* group = process->group;
    session_t* session = group->session;

    return session->tty == interface;
}

// -ERESTART makes the call run again once the group continues. As POSIX has it, a refused SIGTTIN and an orphaned group give -EIO,
// and a refused SIGTTOU lets the access through
static int check_foreground(tty_interface_t* restrict interface, int sig)
{
    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return 0;
    }

    int retval = 0;

    process_group_lock_acquire();

    process_group_t* group = process->group;
    bool background = is_controlling_locked(interface, process) && group != interface->priv.foreground;

    if(background) {
        bool refused = signal_refused(thread, sig);
        bool orphaned = process_group_orphaned_locked(group);

        if(sig == SIGTTOU && refused) {
            retval = 0;
        }
        else if(refused || orphaned) {
            retval = -EIO;
        }
        else {
            process_group_signal_locked(group, sig);
            retval = -ERESTART;
        }
    }

    process_group_lock_release();

    return retval;
}

static int make_controlling(tty_interface_t* restrict interface, uintptr_t argument)
{
    int32_t force;

    if(user_copyin(&force, argument, sizeof(force)) < 0) {
        return -EFAULT;
    }

    thread_t* thread = this_thread();
    process_t* process = thread->process;

    if(process == nullptr) {
        return -EPERM;
    }

    ctty_drop_t drop = { 0 };
    int retval = 0;

    process_group_lock_acquire();

    process_group_t* group = process->group;
    session_t* session = group->session;

    if(session->sid != process->pid) {
        retval = -EPERM;
    }
    else if(session->tty == interface) {
        retval = 0;
    }
    else if(session->tty) {
        retval = -EPERM;
    }
    else if(interface->priv.session && !(force && cred_capable(thread->credentials, CAP_SYS_ADMIN))) {
        retval = -EPERM;
    }
    else {
        if(interface->priv.session) {
            detach_locked(interface, &drop);
        }

        char_interface_incref(&interface->char_interface);
        session_incref(session);
        process_group_incref(group);

        session->tty = interface;
        interface->priv.session = session;
        interface->priv.foreground = group;
    }

    process_group_lock_release();

    drop_detached(&drop);

    return retval;
}

static int give_up_controlling(tty_interface_t* restrict interface)
{
    process_t* process = calling_process();

    if(process == nullptr) {
        return -ENOTTY;
    }

    ctty_drop_t drop = { 0 };
    int retval = 0;

    process_group_lock_acquire();

    process_group_t* group = process->group;
    session_t* session = group->session;

    if(session->tty != interface) {
        retval = -ENOTTY;
    }
    else if(session->sid != process->pid) {
        retval = -EPERM; // UNDONE: only the leader lets go of the session's terminal for now
    }
    else {
        detach_locked(interface, &drop);
    }

    process_group_lock_release();

    drop_detached(&drop);

    return retval;
}

static int get_foreground(tty_interface_t* restrict interface, uintptr_t argument)
{
    process_t* process = calling_process();
    pid_t pgid = 0;
    int retval = 0;

    process_group_lock_acquire();

    if(process == nullptr || !is_controlling_locked(interface, process)) {
        retval = -ENOTTY;
    }
    else {
        process_group_t* foreground = interface->priv.foreground;
        pgid = foreground->pgid;
    }

    process_group_lock_release();

    if(retval == 0 && user_copyout(argument, &pgid, sizeof(pgid)) < 0) {
        retval = -EFAULT;
    }

    return retval;
}

static int set_foreground(tty_interface_t* restrict interface, uintptr_t argument)
{
    pid_t pgid;

    if(user_copyin(&pgid, argument, sizeof(pgid)) < 0) {
        return -EFAULT;
    }

    if(pgid <= 0) {
        return -EINVAL;
    }

    process_t* process = calling_process();
    process_group_t* old_group = nullptr;
    int retval = 0;

    process_group_lock_acquire();

    process_group_t* group = process_group_find_locked(pgid);

    if(process == nullptr || !is_controlling_locked(interface, process)) {
        retval = -ENOTTY;
    }
    else if(group == nullptr || group->session != interface->priv.session) {
        retval = -EPERM;
    }
    else {
        process_group_incref(group);
        old_group = interface->priv.foreground;
        interface->priv.foreground = group;
    }

    process_group_lock_release();

    if(old_group) {
        process_group_decref(old_group);
    }

    return retval;
}

static int get_session(tty_interface_t* restrict interface, uintptr_t argument)
{
    pid_t sid = 0;
    int retval = 0;

    process_group_lock_acquire();

    session_t* session = interface->priv.session;

    if(session == nullptr) {
        retval = -ENOTTY;
    }
    else {
        sid = session->sid;
    }

    process_group_lock_release();

    if(retval == 0 && user_copyout(argument, &sid, sizeof(sid)) < 0) {
        retval = -EFAULT;
    }

    return retval;
}

// A zero timeout waits for as long as it takes
static int wait_drained(tty_interface_t* restrict interface, uint64_t timeout_nsec)
{
    uint64_t deadline = clock_source_now_ns() + timeout_nsec;
    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);

    while(tty_ring_used(&interface->priv.output) && !is_removed(interface)) {
        uint64_t now = clock_source_now_ns();

        if(timeout_nsec && now >= deadline) {
            spinlock_release_intr(&interface->priv.lock, intr_state);
            return -ETIMEDOUT;
        }

        event_t* drained = &interface->priv.drained;
        ssize_t result = event_await_unlock(&drained, 1, &interface->priv.lock, intr_state, timeout_nsec ? deadline - now : 0);

        if(result < 0) {
            return result;
        }

        intr_state = spinlock_acquire_intr(&interface->priv.lock);
    }

    spinlock_release_intr(&interface->priv.lock, intr_state);

    return 0;
}

static int get_termios(tty_interface_t* restrict interface, uintptr_t argument)
{
    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);
    termios_t termios = interface->priv.termios;
    spinlock_release_intr(&interface->priv.lock, intr_state);

    if(user_copyout(argument, &termios, sizeof(termios)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static int set_termios(tty_interface_t* restrict interface, uintptr_t argument, uint32_t request)
{
    termios_t termios;

    if(user_copyin(&termios, argument, sizeof(termios)) < 0) {
        return -EFAULT;
    }

    mutex_acquire(&interface->priv.write_lock);

    int retval = 0;
    bool transmit = false; // Leaving IXOFF while the input is stopped sends VSTART

    if(request != TCSETS) {
        retval = wait_drained(interface, 0);
    }

    if(retval == 0 && interface->ops->set_termios) {
        retval = interface->ops->set_termios(interface, &termios);
    }

    if(retval == 0) {
        bool intr_state = spinlock_acquire_intr(&interface->priv.lock);
        termios_t old_termios = interface->priv.termios;

        interface->priv.termios = termios;
        tty_discipline_set_termios(interface, &old_termios);

        if(request == TCSETSF) {
            discard_flip_locked(interface);
            tty_discipline_flush_input(interface);
        }

        notify_input_locked(interface);
        transmit = notify_output_locked(interface);

        spinlock_release_intr(&interface->priv.lock, intr_state);
    }

    mutex_release(&interface->priv.write_lock);

    start_tx(interface, transmit);

    return retval;
}

static int get_window_size(tty_interface_t* restrict interface, uintptr_t argument)
{
    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);
    winsize_t winsize = interface->priv.winsize;
    spinlock_release_intr(&interface->priv.lock, intr_state);

    if(user_copyout(argument, &winsize, sizeof(winsize)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static int set_window_size(tty_interface_t* restrict interface, uintptr_t argument)
{
    winsize_t winsize;

    if(user_copyin(&winsize, argument, sizeof(winsize)) < 0) {
        return -EFAULT;
    }

    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);
    bool changed = memcmp(&interface->priv.winsize, &winsize, sizeof(winsize));
    interface->priv.winsize = winsize;
    spinlock_release_intr(&interface->priv.lock, intr_state);

    if(changed) {
        send_signals(interface, 1U << SIGWINCH);
    }

    return 0;
}

static int flush_queues(tty_interface_t* restrict interface, uintptr_t argument)
{
    int32_t queue;

    if(user_copyin(&queue, argument, sizeof(queue)) < 0) {
        return -EFAULT;
    }

    if(queue != TCIFLUSH && queue != TCOFLUSH && queue != TCIOFLUSH) {
        return -EINVAL;
    }

    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);

    if(queue != TCOFLUSH) {
        discard_flip_locked(interface);
        tty_discipline_flush_input(interface);
    }

    if(queue != TCIFLUSH) {
        tty_output_flush(interface);
    }

    // Flushed input may let a stopped sender go on
    bool transmit = notify_output_locked(interface);

    spinlock_release_intr(&interface->priv.lock, intr_state);

    start_tx(interface, transmit);

    return 0;
}

static int control_flow(tty_interface_t* restrict interface, uintptr_t argument)
{
    int32_t action;

    if(user_copyin(&action, argument, sizeof(action)) < 0) {
        return -EFAULT;
    }

    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);
    const termios_t* termios = &interface->priv.termios;
    int retval = 0;

    if(action == TCOOFF || action == TCOON) {
        tty_output_set_stopped(interface, action == TCOOFF);
    }
    else if(action == TCIOFF || action == TCION) {
        uint8_t special = termios->c_cc[VSTART];

        if(action == TCIOFF) {
            special = termios->c_cc[VSTOP];
        }

        if(special != _POSIX_VDISABLE) {
            tty_output_put_ahead(interface, special);
        }
    }
    else {
        retval = -EINVAL;
    }

    bool transmit = notify_output_locked(interface);

    spinlock_release_intr(&interface->priv.lock, intr_state);

    start_tx(interface, transmit);

    return retval;
}

static int copyout_count(uintptr_t argument, size_t count)
{
    int32_t value = count;

    if(user_copyout(argument, &value, sizeof(value)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static int set_exclusive(tty_interface_t* restrict interface, bool exclusive)
{
    char_interface_set_exclusive(&interface->char_interface, exclusive);
    return 0;
}

static int count_input(tty_interface_t* restrict interface, uintptr_t argument)
{
    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);
    size_t count = tty_discipline_readable(interface);
    spinlock_release_intr(&interface->priv.lock, intr_state);

    return copyout_count(argument, count);
}

static int count_output(tty_interface_t* restrict interface, uintptr_t argument)
{
    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);
    size_t count = tty_ring_used(&interface->priv.output);
    spinlock_release_intr(&interface->priv.lock, intr_state);

    return copyout_count(argument, count);
}

static int handle_ioctl(tty_interface_t* restrict interface, uint32_t request, uintptr_t argument)
{
    switch(request) {
        case TCGETS:
            return get_termios(interface, argument);

        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            return set_termios(interface, argument, request);

        case TIOCGWINSZ:
            return get_window_size(interface, argument);

        case TIOCSWINSZ:
            return set_window_size(interface, argument);

        case TIOCGPGRP:
            return get_foreground(interface, argument);

        case TIOCSPGRP:
            return set_foreground(interface, argument);

        case TIOCGSID:
            return get_session(interface, argument);

        case TIOCSCTTY:
            return make_controlling(interface, argument);

        case TIOCNOTTY:
            return give_up_controlling(interface);

        case TCFLSH:
            return flush_queues(interface, argument);

        case TCXONC:
            return control_flow(interface, argument);

        case TCDRAIN:
            return wait_drained(interface, 0);

        case TIOCEXCL:
            return set_exclusive(interface, true);

        case TIOCNXCL:
            return set_exclusive(interface, false);

        case TIOCOUTQ:
            return count_output(interface, argument);

        case FIONREAD:
            return count_input(interface, argument);

        default:
            break;
    }

    if(interface->ops->ioctl) {
        return interface->ops->ioctl(interface, request, argument);
    }

    // A line without a break to send has nothing to do
    if(request == TCSENDBREAK) {
        return 0;
    }

    return -ENOTTY;
}

// VMIN and VTIME decide when a non-canonical read is done: VMIN bytes, or VTIME tenths of a
// second, of the whole read when VMIN is zero and between bytes otherwise; both zero just polls
static bool read_done(const termios_t* restrict termios, size_t copied, size_t size)
{
    size_t wanted = termios->c_cc[VMIN];

    if(wanted == 0) {
        wanted = 1;
    }

    if(wanted > size) {
        wanted = size;
    }

    if(copied >= wanted) {
        return true;
    }

    return termios->c_cc[VMIN] == 0 && termios->c_cc[VTIME] == 0;
}

static uint64_t read_timeout(const termios_t* restrict termios, size_t copied)
{
    uint64_t tenths = termios->c_cc[VTIME];

    bool raw = 0 == (termios->c_lflag & ICANON);
    bool waiting = termios->c_cc[VMIN] == 0 || copied > 0;

    if(raw && tenths && waiting) {
        return tenths * VTIME_UNIT_NSEC;
    }

    return 0;
}

static ssize_t read_input(tty_interface_t* restrict interface, uint8_t* restrict buffer, size_t size, unsigned flags)
{
    size_t copied = 0;
    ssize_t error = 0;

    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);

    for(;;) {
        const termios_t* termios = &interface->priv.termios;
        bool end_of_file;

        copied += tty_discipline_read(interface, buffer + copied, size - copied, &end_of_file);

        if(termios->c_lflag & ICANON) {
            if(copied || end_of_file) {
                break;
            }
        }
        else if(read_done(termios, copied, size)) {
            break;
        }

        // A hung up terminal reads as end of file
        if(is_removed(interface)) {
            break;
        }

        if(flags & O_NONBLOCK) {
            if(copied == 0) {
                error = -EAGAIN;
            }

            break;
        }

        event_t* readable = &interface->priv.readable;
        ssize_t result = event_await_unlock(&readable, 1, &interface->priv.lock, intr_state, read_timeout(termios, copied));

        intr_state = spinlock_acquire_intr(&interface->priv.lock);

        if(result == -ETIMEDOUT) {
            break;
        }

        if(result < 0) {
            if(copied == 0) {
                error = result;
            }

            break;
        }
    }

    // The room the read made may send VSTART
    bool transmit = notify_output_locked(interface);

    spinlock_release_intr(&interface->priv.lock, intr_state);

    start_tx(interface, transmit);

    if(error) {
        return error;
    }

    return copied;
}

static ssize_t write_output(tty_interface_t* restrict interface, const uint8_t* restrict buffer, size_t size, unsigned flags)
{
    size_t written = 0;
    ssize_t error = 0;

    mutex_acquire(&interface->priv.write_lock);

    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);

    for(;;) {
        while(written < size && tty_discipline_output(interface, buffer[written])) {
            written += 1;
        }

        bool transmit = notify_output_locked(interface);
        bool done = written == size || is_removed(interface) || (flags & O_NONBLOCK);

        spinlock_release_intr(&interface->priv.lock, intr_state);

        start_tx(interface, transmit);

        if(done) {
            break;
        }

        intr_state = spinlock_acquire_intr(&interface->priv.lock);

        // The driver may have made room while the lock was dropped
        if(tty_ring_room(&interface->priv.output) >= TTY_EXPANSION_MAX || is_removed(interface)) {
            continue;
        }

        event_t* writable = &interface->priv.writable;
        ssize_t result = event_await_unlock(&writable, 1, &interface->priv.lock, intr_state, 0);

        if(result < 0) {
            error = result;
            break;
        }

        intr_state = spinlock_acquire_intr(&interface->priv.lock);
    }

    mutex_release(&interface->priv.write_lock);

    if(written) {
        return written;
    }

    if(is_removed(interface)) {
        return -EIO;
    }

    if(error) {
        return error;
    }

    if(flags & O_NONBLOCK) {
        return -EAGAIN;
    }

    return 0;
}

static int tty_char_open(const char_interface_t* restrict chr, unsigned flags)
{
    tty_interface_t* interface = tty_of(chr);

    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);
    reset_rings_locked(interface);
    termios_t termios = interface->priv.termios;
    spinlock_release_intr(&interface->priv.lock, intr_state);

    if(interface->ops->open) {
        int retval = interface->ops->open(interface);

        if(retval < 0) {
            return retval;
        }
    }

    // No file is open yet to write or set termios meanwhile
    if(interface->ops->set_termios) {
        int retval = interface->ops->set_termios(interface, &termios);

        if(retval < 0 && interface->ops->close) {
            interface->ops->close(interface);
        }

        if(retval < 0) {
            return retval;
        }
    }

    return 0;
}

// A program's last words would be lost otherwise; a signal cuts the wait short
static void tty_char_close(const char_interface_t* restrict chr)
{
    tty_interface_t* interface = tty_of(chr);

    wait_drained(interface, CLOSE_DRAIN_NSEC);

    if(interface->ops->close) {
        interface->ops->close(interface);
    }
}

static bool output_stops_background(tty_interface_t* restrict interface)
{
    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);
    bool tostop = interface->priv.termios.c_lflag & TOSTOP;
    spinlock_release_intr(&interface->priv.lock, intr_state);

    return tostop;
}

// The requests that a process in the
// background of its terminal may not make
static bool changes_terminal(uint32_t request)
{
    switch(request) {
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
        case TIOCSPGRP:
            return true;

        default:
            return false;
    }
}

static ssize_t tty_char_read(const char_interface_t* restrict chr, void* restrict file_ctxdata, off_t offset, void* restrict buffer,
    size_t size, unsigned flags)
{
    tty_interface_t* interface = tty_of(chr);

    if(size == 0) {
        return 0;
    }

    if(!enter(interface)) {
        return 0;
    }

    int allowed = check_foreground(interface, SIGTTIN);

    if(allowed < 0) {
        leave(interface);
        return allowed;
    }

    ssize_t retval = read_input(interface, buffer, size, flags);

    leave(interface);

    return retval;
}

static ssize_t tty_char_write(const char_interface_t* restrict chr, void* restrict file_ctxdata, off_t offset, const void* restrict buffer,
    size_t size, unsigned flags)
{
    tty_interface_t* interface = tty_of(chr);

    if(!enter(interface)) {
        return -EIO;
    }

    if(output_stops_background(interface)) {
        int allowed = check_foreground(interface, SIGTTOU);

        if(allowed < 0) {
            leave(interface);
            return allowed;
        }
    }

    ssize_t retval = write_output(interface, buffer, size, flags);

    leave(interface);

    return retval;
}

static int tty_char_ioctl(const char_interface_t* restrict chr, void* restrict file_ctxdata, uint32_t request, uintptr_t argument)
{
    tty_interface_t* interface = tty_of(chr);

    if(!enter(interface)) {
        return -EIO;
    }

    if(changes_terminal(request)) {
        int allowed = check_foreground(interface, SIGTTOU);

        if(allowed < 0) {
            leave(interface);
            return allowed;
        }
    }

    int retval = handle_ioctl(interface, request, argument);

    leave(interface);

    return retval;
}

static void tty_char_release(const char_interface_t* restrict chr)
{
    tty_interface_t* interface = tty_of(chr);

    pslab_free_virt(interface->priv.flip.data);
    pslab_free_virt(interface->priv.output.data);
    tty_discipline_deinit(interface);

    event_deinit(&interface->priv.readable);
    event_deinit(&interface->priv.writable);
    event_deinit(&interface->priv.drained);
    mutex_deinit(&interface->priv.write_lock);

    if(interface->ops->release) {
        interface->ops->release(interface);
    }
}

static const char_interface_ops_t tty_char_ops = {
    .open = &tty_char_open,
    .close = &tty_char_close,
    .read = &tty_char_read,
    .write = &tty_char_write,
    .ioctl = &tty_char_ioctl,
    .release = &tty_char_release,
};

int tty_interface_add(tty_interface_t* restrict interface, dev_unit_t* restrict unit, unsigned index)
{
    tty_interface_private_t* priv = &interface->priv;

    memset(priv, 0, sizeof(*priv));

    priv->flip.data = pslab_alloc_virt();
    priv->output.data = pslab_alloc_virt();

    if(priv->flip.data == nullptr || priv->output.data == nullptr) {
        pslab_free_virt(priv->flip.data);
        pslab_free_virt(priv->output.data);
        return -ENOMEM;
    }

    int retval = tty_discipline_init(interface);

    if(retval < 0) {
        pslab_free_virt(priv->flip.data);
        pslab_free_virt(priv->output.data);
        return retval;
    }

    priv->lock = SPINLOCK_INIT;
    mutex_init(&priv->write_lock);
    event_init(&priv->readable);
    event_init(&priv->writable);
    event_init(&priv->drained);
    set_default_termios(&priv->termios, interface->initial_speed);

    interface->char_interface.ops = &tty_char_ops;
    interface->char_interface.device = nullptr;
    interface->char_interface.ctxdata = interface;

    retval = char_interface_add(&interface->char_interface, unit, index);

    if(retval < 0) {
        pslab_free_virt(priv->flip.data);
        pslab_free_virt(priv->output.data);
        tty_discipline_deinit(interface);
        event_deinit(&priv->readable);
        event_deinit(&priv->writable);
        event_deinit(&priv->drained);
        mutex_deinit(&priv->write_lock);
        return retval;
    }

    return 0;
}

int tty_interface_remove(tty_interface_t* restrict interface)
{
    atomic_store(&interface->priv.removed, true, ATOMIC_SEQ_CST);

    // The registry's reference may be the
    // last one, and the interface is still needed below
    char_interface_incref(&interface->char_interface);

    int retval = char_interface_remove(&interface->char_interface);

    if(retval < 0) {
        char_interface_decref(&interface->char_interface);
        return retval;
    }

    // Under the lock, so a reader or writer about
    // to sleep either sees removed or gets woken
    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);
    event_trigger(&interface->priv.readable, false);
    event_trigger(&interface->priv.writable, false);
    event_trigger(&interface->priv.drained, false);
    spinlock_release_intr(&interface->priv.lock, intr_state);

    while(atomic_load(&interface->priv.in_flight, ATOMIC_ACQUIRE)) {
        hart_relax();
    }

    ctty_drop_t drop = { 0 };

    process_group_lock_acquire();

    if(interface->priv.session) {
        detach_locked(interface, &drop);
    }

    process_group_lock_release();

    drop_detached(&drop);
    char_interface_decref(&interface->char_interface);

    return 0;
}

void tty_receive(tty_interface_t* restrict interface, const void* restrict bytes, size_t count)
{
    const uint8_t* data = bytes;
    tty_ring_t* flip = &interface->priv.flip;

    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);

    for(size_t i = 0; i < count; ++i) {
        if(tty_ring_room(flip) == 0) {
            interface->priv.lost += count - i;
            break;
        }

        tty_ring_put(flip, data[i]);
    }

    spinlock_release_intr(&interface->priv.lock, intr_state);

    if(!is_removed(interface)) {
        queue_for_worker(interface);
    }
}

void tty_receive_error(tty_interface_t* restrict interface, uint8_t byte, unsigned error)
{
    kassert(error >= TTY_ERROR_BREAK && error <= TTY_ERROR_OVERRUN);

    tty_interface_private_t* priv = &interface->priv;
    tty_ring_t* flip = &priv->flip;

    bool intr_state = spinlock_acquire_intr(&priv->lock);

    if(error == TTY_ERROR_OVERRUN) {
        priv->lost += 1; // The hardware doesn't say how many it overran

        if(tty_ring_room(flip)) {
            tty_ring_put(flip, byte);
        }
        else {
            priv->lost += 1;
        }
    }
    else if(tty_ring_room(flip) >= 2) {
        bitmap_set(priv->flip_errors, flip->tail % TTY_RING_SIZE);
        tty_ring_put(flip, error);
        tty_ring_put(flip, byte);
    }
    else {
        priv->lost += 1;
    }

    spinlock_release_intr(&priv->lock, intr_state);

    if(!is_removed(interface)) {
        queue_for_worker(interface);
    }
}

size_t tty_transmit_take(tty_interface_t* restrict interface, void* restrict buffer, size_t size)
{
    uint8_t* out = buffer;
    tty_ring_t* output = &interface->priv.output;
    size_t taken = 0;

    bool intr_state = spinlock_acquire_intr(&interface->priv.lock);

    if(interface->priv.xchar && size) {
        out[0] = interface->priv.xchar;
        interface->priv.xchar = 0;
        taken = 1;
    }

    if(!interface->priv.output_stopped) {
        while(taken < size && tty_ring_used(output)) {
            out[taken] = tty_ring_take(output);
            taken += 1;
        }
    }

    if(taken) {
        notify_output_locked(interface);
    }

    spinlock_release_intr(&interface->priv.lock, intr_state);

    return taken;
}

bool tty_output_put(tty_interface_t* restrict interface, const uint8_t* restrict bytes, size_t count)
{
    tty_ring_t* output = &interface->priv.output;

    if(tty_ring_room(output) < count) {
        return false;
    }

    for(size_t i = 0; i < count; ++i) {
        tty_ring_put(output, bytes[i]);
    }

    return true;
}

void tty_output_put_ahead(tty_interface_t* restrict interface, uint8_t byte)
{
    interface->priv.xchar = byte;
}

void tty_output_flush(tty_interface_t* restrict interface)
{
    tty_ring_t* output = &interface->priv.output;
    output->head = output->tail;
}

void tty_output_set_stopped(tty_interface_t* restrict interface, bool stopped)
{
    interface->priv.output_stopped = stopped;
}

bool tty_output_is_stopped(const tty_interface_t* restrict interface)
{
    return interface->priv.output_stopped;
}

void tty_queue_signal(tty_interface_t* restrict interface, int sig)
{
    interface->priv.signals |= 1U << sig;
}

static void init_tty(void)
{
    event_init(&worker_wakeup);

    if(!thread_spawn_kernel(&tty_worker, nullptr)) {
        panic("tty: out of memory");
    }
}

initcall_bsp_dev(init_tty, INIT_FIRST);
