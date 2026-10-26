#include "vex/serial.h"
#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/capability.h>
#include <vex/clocksrc.h>
#include <vex/cmdline.h>
#include <vex/cred.h>
#include <vex/ctype.h>
#include <vex/dev_name.h>
#include <vex/errno.h>
#include <vex/event.h>
#include <vex/kprintf.h>
#include <vex/serial_interface.h>
#include <vex/stdlib.h>
#include <vex/string.h>
#include <vex/strings.h>
#include <vex/thread.h>
#include <vex/uaccess.h>

#define NSEC_PER_MSEC UINT64_C(1000000)

#define BREAK_DEFAULT_MSEC 250
#define BREAK_MAX_MSEC     10000
#define EXCHANGE_MAX_MSEC  60000
#define REMOVE_RETRY_MSEC  10

#define CONSOLE_OPTIONS_MAX 16

typedef struct exchange_s exchange_t;

// Under the port's dispatch lock
struct exchange_s {
    const uint8_t* send;
    uint8_t* receive;
    uint32_t send_size;
    uint32_t sent;
    uint32_t receive_size;
    uint32_t received;
    int32_t terminator;
    bool done;
    event_t finished;
};

static spinlock_t consumers_lock = SPINLOCK_INIT;
static serial_consumer_t* consumers_head = nullptr;

static serial_interface_t* serial_of(const char_interface_t* restrict chr)
{
    return chr->ctxdata;
}

static bool is_removed(const serial_interface_t* restrict interface)
{
    return atomic_load(&interface->priv.removed, ATOMIC_ACQUIRE);
}

static bool is_terminated(const char* restrict buffer, size_t size)
{
    return memchr(buffer, 0, size);
}

static void sleep_msec(uint32_t msec)
{
    event_t never;
    event_t* events = &never;

    event_init(&never);
    event_await_timeout(&events, 1, msec * NSEC_PER_MSEC);
    event_deinit(&never);
}

static void set_dispatch(serial_interface_t* restrict interface, const serial_consumer_t* restrict consumer)
{
    bool intr_state = spinlock_acquire_intr(&interface->priv.dispatch_lock);
    interface->priv.consumer = consumer;
    spinlock_release_intr(&interface->priv.dispatch_lock, intr_state);
}

static bool line_is_valid(const serial_line_t* restrict line)
{
    bool result = true;
    result = result && line->speed;
    result = result && line->data_bits >= 5 && line->data_bits <= 8;
    result = result && line->stop_bits >= 1 && line->stop_bits <= 2;
    result = result && line->parity <= SERIAL_PARITY_SPACE;
    result = result && 0 == (line->flow & ~SERIAL_FLOW_RTSCTS);
    return result;
}

static int startup_locked(serial_interface_t* restrict interface)
{
    if(interface->priv.started) {
        return 0;
    }

    if(interface->ops->startup) {
        int retval = interface->ops->startup(interface);

        if(retval < 0) {
            return retval;
        }
    }

    interface->priv.started = true;

    return 0;
}

static void shutdown_locked(serial_interface_t* restrict interface)
{
    if(!interface->priv.started) {
        return;
    }

    if(interface->ops->shutdown) {
        interface->ops->shutdown(interface);
    }

    interface->priv.started = false;
}

static int set_line_locked(serial_interface_t* restrict interface, const serial_line_t* restrict line)
{
    if(!line_is_valid(line) || line->speed > interface->max_speed) {
        return -EINVAL;
    }

    int retval = interface->ops->set_line(interface, line);

    if(retval == 0) {
        interface->line = *line;
    }

    return retval;
}

static void set_modem_locked(serial_interface_t* restrict interface, uint32_t raise, uint32_t lower)
{
    uint32_t outputs = interface->modem_lines & (SERIAL_MODEM_DTR | SERIAL_MODEM_RTS);

    interface->priv.modem = ((interface->priv.modem & ~lower) | raise) & outputs;

    if(interface->ops->set_modem) {
        interface->ops->set_modem(interface, interface->priv.modem);
    }
}

static uint32_t get_modem_locked(serial_interface_t* restrict interface)
{
    if(interface->ops->get_modem) {
        return interface->ops->get_modem(interface);
    }

    return interface->priv.modem;
}

static int send_break_locked(serial_interface_t* restrict interface, uint32_t msec)
{
    if(interface->ops->set_break == nullptr) {
        return -EINVAL;
    }

    if(msec == 0) {
        msec = BREAK_DEFAULT_MSEC;
    }

    interface->ops->set_break(interface, true);
    sleep_msec(msec);
    interface->ops->set_break(interface, false);

    return 0;
}

static const serial_consumer_t* find_consumer(const char* restrict name)
{
    spinlock_acquire(&consumers_lock);

    serial_consumer_t* consumer = consumers_head;

    while(consumer && strcmp(consumer->name, name)) {
        consumer = consumer->next;
    }

    spinlock_release(&consumers_lock);

    return consumer;
}

static int bind_locked(serial_interface_t* restrict interface, serial_binding_t* restrict binding)
{
    if(is_removed(interface)) {
        return -ENODEV;
    }

    if(interface->priv.consumer || interface->priv.unbinding) {
        return -EBUSY;
    }

    const serial_consumer_t* consumer = find_consumer(binding->consumer);

    if(consumer == nullptr) {
        return -ENOENT;
    }

    int retval = consumer->bind(interface, binding->options, &binding->index, binding->object, sizeof(binding->object));

    if(retval < 0) {
        return retval;
    }

    set_dispatch(interface, consumer);
    interface->priv.binding = *binding;

    return 0;
}

// The consumer's unbind runs without the lock
// as the last calls of what it takes away may need it
static int unbind(serial_interface_t* restrict interface, bool force)
{
    const serial_consumer_t* consumer = nullptr;
    int retval = 0;

    mutex_acquire(&interface->priv.lock);

    if(interface->priv.unbinding) {
        retval = -EBUSY;
    }
    else if(interface->priv.binding.consumer[0] == 0) {
        retval = -ENXIO;
    }
    else {
        consumer = interface->priv.consumer;
        interface->priv.unbinding = true;
    }

    mutex_release(&interface->priv.lock);

    if(consumer == nullptr) {
        return retval;
    }

    retval = consumer->unbind(interface, force);

    mutex_acquire(&interface->priv.lock);

    interface->priv.unbinding = false;

    if(retval == 0) {
        set_dispatch(interface, nullptr);
        memset(&interface->priv.binding, 0, sizeof(interface->priv.binding));
        interface->consumer_data = nullptr;
        shutdown_locked(interface);
    }

    mutex_release(&interface->priv.lock);

    return retval;
}

static void bind_from_cmdline_item(serial_interface_t* restrict interface, const char* restrict item)
{
    const char* port = interface->char_interface.name;
    size_t length = strlen(item);
    size_t port_length = strcspn(item, ",");

    bool matches = item[port_length] == ',';
    matches = matches && port_length == strlen(port);
    matches = matches && 0 == strncmp(item, port, port_length);

    if(!matches) {
        return;
    }

    const char* name = &item[port_length + 1];
    size_t name_length = length - port_length - 1;
    size_t letters = 0;

    while(letters < name_length && !isdigit(name[letters])) {
        letters += 1;
    }

    serial_binding_t binding = {};
    binding.index = SERIAL_ANY_INDEX;

    if(letters == 0 || letters >= sizeof(binding.consumer)) {
        pr_warning("serial: %s: bad serial= consumer", interface->char_interface.name);
        return;
    }

    memcpy(binding.consumer, name, letters);

    if(letters < name_length) {
        binding.index = strtol(&name[letters], nullptr, 10);
    }

    mutex_acquire(&interface->priv.lock);
    int retval = bind_locked(interface, &binding);
    mutex_release(&interface->priv.lock);

    if(retval < 0) {
        pr_warning("serial: %s: binding %s: %s", interface->char_interface.name, binding.consumer, strerror(retval));
        return;
    }

    pr_inform("serial: %s bound to %s", interface->char_interface.name, binding.object);
}

// serial=serial0,tty0 serial=serial1,tty binds by the port's
// name; an index after the consumer's name is the one to give
static void bind_from_cmdline(serial_interface_t* restrict interface)
{
    const char* item = cmdline_next_argument(&sys_cmdline, "serial", nullptr);

    while(item) {
        bind_from_cmdline_item(interface, item);
        item = cmdline_next_argument(&sys_cmdline, "serial", item);
    }
}

static void take_answer(exchange_t* restrict exchange, uint8_t byte)
{
    exchange->receive[exchange->received] = byte;
    exchange->received += 1;

    if(exchange->received == exchange->receive_size || (int32_t)(byte) == exchange->terminator) {
        exchange->done = true;
        event_trigger(&exchange->finished, false);
    }
}

static void exchange_receive(serial_interface_t* restrict interface, const uint8_t* restrict bytes, size_t count)
{
    exchange_t* exchange = interface->consumer_data;

    for(size_t i = 0; i < count && !exchange->done; ++i) {
        take_answer(exchange, bytes[i]);
    }
}

static void exchange_receive_error(serial_interface_t* restrict interface, uint8_t byte, unsigned error)
{
    exchange_t* exchange = interface->consumer_data;

    if(error != SERIAL_ERROR_BREAK && !exchange->done) {
        take_answer(exchange, byte);
    }
}

// An exchange that reads nothing back is done once its bytes are taken
static size_t exchange_transmit_take(serial_interface_t* restrict interface, void* restrict buffer, size_t size)
{
    exchange_t* exchange = interface->consumer_data;
    size_t count = exchange->send_size - exchange->sent;

    if(count > size) {
        count = size;
    }

    memcpy(buffer, &exchange->send[exchange->sent], count);
    exchange->sent += count;

    if(exchange->receive_size == 0 && exchange->sent == exchange->send_size && !exchange->done) {
        exchange->done = true;
        event_trigger(&exchange->finished, false);
    }

    return count;
}

// Not registered, so nothing binds it; an exchange
// puts it in place for as long as it lasts
static const serial_consumer_t exchange_consumer = {
    .name = "exchange",
    .receive = &exchange_receive,
    .receive_error = &exchange_receive_error,
    .transmit_take = &exchange_transmit_take,
};

static int wait_exchanged(serial_interface_t* restrict interface, exchange_t* restrict exchange, uint32_t timeout_msec)
{
    uint64_t deadline = clock_source_now_ns() + timeout_msec * NSEC_PER_MSEC;
    int retval = 0;

    bool intr_state = spinlock_acquire_intr(&interface->priv.dispatch_lock);

    while(!exchange->done && !is_removed(interface)) {
        uint64_t now = clock_source_now_ns();

        if(now >= deadline) {
            break;
        }

        event_t* finished = &exchange->finished;
        ssize_t result = event_await_unlock(&finished, 1, &interface->priv.dispatch_lock, intr_state, deadline - now);

        intr_state = spinlock_acquire_intr(&interface->priv.dispatch_lock);

        if(result == -ETIMEDOUT) {
            break;
        }

        // Running the system call again
        // would send the bytes twice
        if(result < 0) {
            if(result == -ERESTART) {
                retval = -EINTR;
                break;
            }
            else {
                retval = (int)(result);
                break;
            }
        }
    }

    interface->priv.consumer = nullptr;

    spinlock_release_intr(&interface->priv.dispatch_lock, intr_state);

    return retval;
}

static int run_exchange_locked(serial_interface_t* restrict interface, exchange_t* restrict exchange, uint32_t timeout_msec)
{
    if(is_removed(interface)) {
        return -ENODEV;
    }

    if(interface->priv.consumer || interface->priv.unbinding) {
        return -EBUSY;
    }

    bool was_started = interface->priv.started;
    int retval = startup_locked(interface);

    if(retval < 0) {
        return retval;
    }

    interface->consumer_data = exchange;
    set_dispatch(interface, &exchange_consumer);

    if(exchange->send_size) {
        interface->ops->start_tx(interface);
    }

    retval = wait_exchanged(interface, exchange, timeout_msec);

    interface->consumer_data = nullptr;

    if(!was_started) {
        shutdown_locked(interface);
    }

    return retval;
}

static int exchange_bytes(serial_interface_t* restrict interface, uintptr_t argument)
{
    serial_exchange_t request;
    uint8_t send[SERIAL_EXCHANGE_MAX];
    uint8_t receive[SERIAL_EXCHANGE_MAX];

    if(user_copyin(&request, argument, sizeof(request)) < 0) {
        return -EFAULT;
    }

    if(request.send_size > SERIAL_EXCHANGE_MAX || request.receive_size > SERIAL_EXCHANGE_MAX) {
        return -EINVAL;
    }

    if(request.timeout_msec > EXCHANGE_MAX_MSEC) {
        return -EINVAL;
    }

    if(request.terminator < SERIAL_NO_TERMINATOR || request.terminator > UINT8_MAX) {
        return -EINVAL;
    }

    if(user_copyin(send, request.send, request.send_size) < 0) {
        return -EFAULT;
    }

    exchange_t exchange = {};
    exchange.send = send;
    exchange.receive = receive;
    exchange.send_size = request.send_size;
    exchange.receive_size = request.receive_size;
    exchange.terminator = request.terminator;
    event_init(&exchange.finished);

    mutex_acquire(&interface->priv.lock);
    int retval = run_exchange_locked(interface, &exchange, request.timeout_msec);
    mutex_release(&interface->priv.lock);

    event_deinit(&exchange.finished);

    if(retval < 0) {
        return retval;
    }

    request.receive_size = exchange.received;

    if(user_copyout(request.receive, receive, exchange.received) < 0) {
        return -EFAULT;
    }

    if(user_copyout(argument, &request, sizeof(request)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static int get_info(serial_interface_t* restrict interface, uintptr_t argument)
{
    serial_info_t info = {};

    // The structure starts zeroed, so a string as long as its field stays terminated
    kstrncpy(info.driver, interface->driver, sizeof(info.driver) - 1);

    if(interface->firmware_id) {
        kstrncpy(info.firmware_id, interface->firmware_id, sizeof(info.firmware_id) - 1);
    }

    info.max_speed = interface->max_speed;
    info.modem_lines = interface->modem_lines;

    if(user_copyout(argument, &info, sizeof(info)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static int get_line(serial_interface_t* restrict interface, uintptr_t argument)
{
    mutex_acquire(&interface->priv.lock);
    serial_line_t line = interface->line;
    mutex_release(&interface->priv.lock);

    if(user_copyout(argument, &line, sizeof(line)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static int set_line(serial_interface_t* restrict interface, uintptr_t argument)
{
    serial_line_t line;

    if(user_copyin(&line, argument, sizeof(line)) < 0) {
        return -EFAULT;
    }

    return serial_interface_set_line(interface, &line);
}

static int get_modem(serial_interface_t* restrict interface, uintptr_t argument)
{
    mutex_acquire(&interface->priv.lock);
    uint32_t lines = get_modem_locked(interface);
    mutex_release(&interface->priv.lock);

    if(user_copyout(argument, &lines, sizeof(lines)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static int set_modem(serial_interface_t* restrict interface, uintptr_t argument)
{
    serial_modem_t modem;

    if(user_copyin(&modem, argument, sizeof(modem)) < 0) {
        return -EFAULT;
    }

    serial_interface_set_modem(interface, modem.raise, modem.lower);

    return 0;
}

static int send_break(serial_interface_t* restrict interface, uintptr_t argument)
{
    int32_t msec;

    if(user_copyin(&msec, argument, sizeof(msec)) < 0) {
        return -EFAULT;
    }

    if(msec < 0 || msec > BREAK_MAX_MSEC) {
        return -EINVAL;
    }

    return serial_interface_send_break(interface, msec);
}

static int bind(serial_interface_t* restrict interface, uintptr_t argument)
{
    serial_binding_t binding;

    if(user_copyin(&binding, argument, sizeof(binding)) < 0) {
        return -EFAULT;
    }

    if(!is_terminated(binding.consumer, sizeof(binding.consumer))) {
        return -EINVAL;
    }

    if(!is_terminated(binding.options, sizeof(binding.options))) {
        return -EINVAL;
    }

    if(binding.index < SERIAL_ANY_INDEX) {
        return -EINVAL;
    }

    memset(binding.object, 0, sizeof(binding.object));

    mutex_acquire(&interface->priv.lock);
    int retval = bind_locked(interface, &binding);
    mutex_release(&interface->priv.lock);

    if(retval == 0) {
        if(user_copyout(argument, &binding, sizeof(binding)) < 0) {
            return -EFAULT;
        }
    }

    return retval;
}

static int get_binding(serial_interface_t* restrict interface, uintptr_t argument)
{
    mutex_acquire(&interface->priv.lock);
    serial_binding_t binding = interface->priv.binding;
    mutex_release(&interface->priv.lock);

    if(user_copyout(argument, &binding, sizeof(binding)) < 0) {
        return -EFAULT;
    }

    return 0;
}

static bool changes_port(uint32_t request)
{
    switch(request) {
        case SERIAL_SET_LINE:
        case SERIAL_SET_MODEM:
        case SERIAL_BREAK:
        case SERIAL_EXCHANGE:
        case SERIAL_BIND:
        case SERIAL_UNBIND:
            return true;

        default:
            return false;
    }
}

// Only the file that keeps everyone else out may change the port, so that a sequence
// of requests, such as dialing a modem and binding it, isn't mixed with another's
static bool may_change(const serial_interface_t* restrict interface)
{
    thread_t* thread = this_thread();

    return char_interface_is_exclusive(&interface->char_interface) && cred_capable(thread->credentials, CAP_SYS_TTY_CONFIG);
}

static int serial_char_ioctl(const char_interface_t* restrict chr, void* restrict file_ctxdata, uint32_t request, uintptr_t argument)
{
    serial_interface_t* interface = serial_of(chr);

    if(changes_port(request) && !may_change(interface)) {
        return -EPERM;
    }

    switch(request) {
        case SERIAL_GET_INFO:
            return get_info(interface, argument);

        case SERIAL_GET_LINE:
            return get_line(interface, argument);

        case SERIAL_SET_LINE:
            return set_line(interface, argument);

        case SERIAL_GET_MODEM:
            return get_modem(interface, argument);

        case SERIAL_SET_MODEM:
            return set_modem(interface, argument);

        case SERIAL_BREAK:
            return send_break(interface, argument);

        case SERIAL_EXCHANGE:
            return exchange_bytes(interface, argument);

        case SERIAL_BIND:
            return bind(interface, argument);

        case SERIAL_UNBIND:
            return unbind(interface, false);

        case SERIAL_GET_BINDING:
            return get_binding(interface, argument);

        default:
            return -ENOTTY;
    }
}

static void serial_char_release(const char_interface_t* restrict chr)
{
    serial_interface_t* interface = serial_of(chr);

    mutex_deinit(&interface->priv.lock);

    if(interface->ops->release) {
        interface->ops->release(interface);
    }
}

static const char_interface_ops_t serial_char_ops = {
    .ioctl = &serial_char_ioctl,
    .release = &serial_char_release,
};

static serial_interface_t* console_port(const console_t* restrict cons)
{
    return cons->ctxdata;
}

static void serial_console_write(console_t* restrict cons, const void* restrict data, size_t size)
{
    serial_interface_t* interface = console_port(cons);
    interface->ops->poll_write(interface, data, size);
}

static void serial_console_release(console_t* restrict cons)
{
    serial_interface_t* interface = console_port(cons);
    interface->ops->poll_drain(interface);
}

static bool serial_console_takeover(console_t* restrict cons)
{
    serial_interface_t* interface = console_port(cons);
    interface->ops->poll_reset(interface, &interface->priv.console_line);

    return true;
}

static const console_ops_t serial_console_ops = {
    .write = &serial_console_write,
    .release = &serial_console_release,
    .takeover = &serial_console_takeover,
};

// As Linux's console= takes them: a speed, then the parity's letter and the data bits, as in 115200n8
static bool parse_console_line(const char* restrict options, serial_line_t* restrict line)
{
    static const char parity_letters[] = "noems"; // In the order of SERIAL_PARITY_*
    serial_line_t parsed = *line;

    parsed.speed = 0;

    while(isdigit(*options)) {
        parsed.speed = parsed.speed * 10 + (*options - '0');
        options += 1;
    }

    const char* parity = nullptr;

    if(*options) {
        parity = strchr(parity_letters, *options);
    }

    if(parity) {
        parsed.parity = parity - parity_letters;
        options += 1;
    }

    if(*options >= '5' && *options <= '8') {
        parsed.data_bits = *options - '0';
        options += 1;
    }

    if(parsed.speed == 0 || *options) {
        return false;
    }

    *line = parsed;

    return true;
}

// The console is added once the port has its name, but the
// port's first open, which may come first, already releases it
static void prepare_console(serial_interface_t* restrict interface, const char* restrict name)
{
    char options[CONSOLE_OPTIONS_MAX];
    serial_line_t line = interface->line;

    if(console_lookup(name, options, sizeof(options)) != CONSOLE_LISTED) {
        return;
    }

    if(interface->ops->poll_write == nullptr) {
        pr_warning("serial: %s can't be a console", name);
        return;
    }

    if(options[0] && !parse_console_line(options, &line)) {
        pr_warning("serial: %s: bad console line %s", name, options);
    }
    else if(options[0]) {
        mutex_acquire(&interface->priv.lock);
        int retval = set_line_locked(interface, &line);
        mutex_release(&interface->priv.lock);

        if(retval < 0) {
            pr_warning("serial: %s: can't run the console at %s", name, options);
        }
    }

    interface->priv.console_line = interface->line;

    console_t* cons = &interface->priv.console;
    cons->ops = &serial_console_ops;
    cons->name = interface->char_interface.name;
    cons->consflags = CONSF_RECALL;
    cons->ctxdata = interface;

    interface->char_interface.console = cons;
}

static int add_with_unit(serial_interface_t* restrict interface, bool numbered, unsigned number)
{
    serial_interface_private_t* priv = &interface->priv;

    memset(priv, 0, sizeof(*priv));
    mutex_init(&priv->lock);
    priv->dispatch_lock = SPINLOCK_INIT;

    if(interface->line.speed == 0) {
        interface->line.speed = SERIAL_DEFAULT_SPEED;
        interface->line.data_bits = 8;
        interface->line.stop_bits = 1;
        interface->line.parity = SERIAL_PARITY_NONE;
        interface->line.flow = 0;
    }

    int retval;

    if(numbered) {
        retval = dev_unit_claim(&dev_prefix_serial, number, &priv->unit);
    }
    else {
        retval = dev_unit_alloc(&dev_prefix_serial, interface->char_interface.device, &priv->unit);
    }

    if(retval < 0) {
        mutex_deinit(&priv->lock);
        return retval;
    }

    interface->consumer_data = nullptr;
    interface->char_interface.ops = &serial_char_ops;
    interface->char_interface.ctxdata = interface;

    char name[sizeof(interface->char_interface.name)];
    dev_name_t dev_name = {};
    dev_name.prefix = priv->unit->prefix;
    dev_name.unit = priv->unit->number;
    dev_name.index = DEV_NAME_NONE;
    dev_name.partition = DEV_NAME_NONE;

    interface->char_interface.console = nullptr;

    if(0 == dev_name_format(&dev_name, name, sizeof(name))) {
        prepare_console(interface, name);
    }

    retval = char_interface_add(&interface->char_interface, priv->unit, DEV_NAME_NONE);

    if(retval < 0) {
        dev_unit_free(priv->unit);
        mutex_deinit(&priv->lock);
        return retval;
    }

    if(interface->char_interface.console) {
        console_add(interface->char_interface.console);
    }

    bind_from_cmdline(interface);

    return 0;
}

int serial_interface_add(serial_interface_t* restrict interface)
{
    return add_with_unit(interface, false, 0);
}

int serial_interface_add_numbered(serial_interface_t* restrict interface, unsigned number)
{
    return add_with_unit(interface, true, number);
}

int serial_interface_remove(serial_interface_t* restrict interface)
{
    if(interface->char_interface.console) {
        console_remove(interface->char_interface.console);
    }

    atomic_store(&interface->priv.removed, true, ATOMIC_SEQ_CST);

    bool intr_state = spinlock_acquire_intr(&interface->priv.dispatch_lock);

    if(interface->priv.consumer == &exchange_consumer) {
        exchange_t* exchange = interface->consumer_data;
        event_trigger(&exchange->finished, false);
    }

    spinlock_release_intr(&interface->priv.dispatch_lock, intr_state);

    // With force, only an unbind already running makes it wait
    while(-EBUSY == unbind(interface, true)) {
        sleep_msec(REMOVE_RETRY_MSEC);
    }

    // Waits for the calls into the driver
    // New calls see the port as removed
    mutex_acquire(&interface->priv.lock);
    shutdown_locked(interface);
    mutex_release(&interface->priv.lock);

    // The registry's reference may be the last
    // one, so release can run during the removal
    dev_unit_t* unit = interface->priv.unit;
    int retval = char_interface_remove(&interface->char_interface);

    if(retval < 0) {
        return retval;
    }

    dev_unit_free(unit);

    return 0;
}

void serial_consumer_register(serial_consumer_t* restrict consumer)
{
    spinlock_acquire(&consumers_lock);
    consumer->next = consumers_head;
    consumers_head = consumer;
    spinlock_release(&consumers_lock);
}

void serial_receive(serial_interface_t* restrict interface, const void* restrict bytes, size_t count)
{
    bool intr_state = spinlock_acquire_intr(&interface->priv.dispatch_lock);
    const serial_consumer_t* consumer = interface->priv.consumer;

    if(consumer) {
        consumer->receive(interface, bytes, count);
    }
    else {
        interface->priv.lost += count;
    }

    spinlock_release_intr(&interface->priv.dispatch_lock, intr_state);
}

void serial_receive_error(serial_interface_t* restrict interface, uint8_t byte, unsigned error)
{
    kassert(error >= SERIAL_ERROR_BREAK && error <= SERIAL_ERROR_OVERRUN);

    bool intr_state = spinlock_acquire_intr(&interface->priv.dispatch_lock);
    const serial_consumer_t* consumer = interface->priv.consumer;

    if(consumer) {
        consumer->receive_error(interface, byte, error);
    }
    else {
        interface->priv.lost += 1;
    }

    spinlock_release_intr(&interface->priv.dispatch_lock, intr_state);
}

size_t serial_transmit_take(serial_interface_t* restrict interface, void* restrict buffer, size_t size)
{
    size_t taken = 0;

    bool intr_state = spinlock_acquire_intr(&interface->priv.dispatch_lock);
    const serial_consumer_t* consumer = interface->priv.consumer;

    if(consumer && size) {
        taken = consumer->transmit_take(interface, buffer, size);
    }

    spinlock_release_intr(&interface->priv.dispatch_lock, intr_state);

    return taken;
}

void serial_interface_detach(serial_interface_t* restrict interface)
{
    set_dispatch(interface, nullptr);
}

// The port is the consumer's from now on, and the console writes to it only in a panic
int serial_interface_startup(serial_interface_t* restrict interface)
{
    int retval = -ENODEV;

    if(interface->char_interface.console) {
        console_release(interface->char_interface.console);
    }

    mutex_acquire(&interface->priv.lock);

    if(!is_removed(interface)) {
        retval = startup_locked(interface);
    }

    mutex_release(&interface->priv.lock);

    return retval;
}

void serial_interface_shutdown(serial_interface_t* restrict interface)
{
    mutex_acquire(&interface->priv.lock);

    if(!is_removed(interface)) {
        shutdown_locked(interface);
    }

    mutex_release(&interface->priv.lock);
}

int serial_interface_set_line(serial_interface_t* restrict interface, const serial_line_t* restrict line)
{
    int retval = -ENODEV;

    mutex_acquire(&interface->priv.lock);

    if(!is_removed(interface)) {
        retval = set_line_locked(interface, line);
    }

    mutex_release(&interface->priv.lock);

    return retval;
}

void serial_interface_set_modem(serial_interface_t* restrict interface, uint32_t raise, uint32_t lower)
{
    mutex_acquire(&interface->priv.lock);

    if(!is_removed(interface)) {
        set_modem_locked(interface, raise, lower);
    }

    mutex_release(&interface->priv.lock);
}

int serial_interface_send_break(serial_interface_t* restrict interface, uint32_t msec)
{
    int retval = -ENODEV;

    mutex_acquire(&interface->priv.lock);

    if(!is_removed(interface)) {
        retval = send_break_locked(interface, msec);
    }

    mutex_release(&interface->priv.lock);

    return retval;
}

void serial_interface_start_tx(serial_interface_t* restrict interface)
{
    if(!is_removed(interface)) {
        interface->ops->start_tx(interface);
    }
}
