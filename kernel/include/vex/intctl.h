#pragma once

#include <vex/compiler.h>
#include <vex/irq.h>
#include <vex/types.h>

typedef struct device_s device_t;
typedef struct hart_local_s hart_local_t;

enum {
    TRIGGER_LEVEL,
    TRIGGER_EDGE,
};

enum {
    ACTIVE_LOW,
    ACTIVE_HIGH,
};

#define INTERRUPT_SHARED 0x01U // The handler may share the GSI with others that agree to

typedef struct interrupt_controller_s interrupt_controller_t;
typedef struct interrupt_controller_ops_s interrupt_controller_ops_t;
typedef struct interrupt_message_ops_s interrupt_message_ops_t;
typedef struct interrupt_controller_private_s interrupt_controller_private_t;
typedef struct interrupt_action_s interrupt_action_t;

struct interrupt_controller_ops_s {
    void (*mask)(interrupt_controller_t* restrict controller, uint32_t gsi);
    void (*unmask)(interrupt_controller_t* restrict controller, uint32_t gsi);
    void (*route)(interrupt_controller_t* restrict controller, uint32_t gsi, uint8_t vector, const hart_local_t* restrict hart,
        unsigned trigger, unsigned polarity);
};

// A source that writes a message to the CPU, as MSI does. It needs no interrupt controller
struct interrupt_message_ops_s {
    /// Sets the message that the source writes
    /// @pre The source is masked
    void (*write)(void* restrict source, uint64_t address, uint32_t data);

    void (*mask)(void* restrict source);
    void (*unmask)(void* restrict source);
};

struct interrupt_controller_private_s {
    interrupt_controller_t* next;
};

struct interrupt_controller_s {
    interrupt_controller_private_t priv;
    const interrupt_controller_ops_t* ops;
    const char* name;
    uint32_t gsi_base;
    uint32_t gsi_count;
    void* ctxdata;
    device_t* device; // The node of the tree that drives it, which drivers with its GSIs then rely on; nullptr until there is one
};

int interrupt_controller_add(interrupt_controller_t* restrict controller);

/// @pre No GSI of the controller is attached
void interrupt_controller_remove(interrupt_controller_t* restrict controller);

interrupt_controller_t* interrupt_controller_find(uint32_t gsi) __nodiscard;

void interrupt_controller_mask(interrupt_controller_t* restrict controller, uint32_t gsi);
void interrupt_controller_unmask(interrupt_controller_t* restrict controller, uint32_t gsi);
void interrupt_controller_route(interrupt_controller_t* restrict controller, uint32_t gsi, uint8_t vector,
    const hart_local_t* restrict hart, unsigned trigger, unsigned polarity);

/// Maps an ISA IRQ to the GSI it raises, through the firmware's overrides
/// @note Without an override the GSI is the IRQ itself, edge-triggered and active-high
void interrupt_isa_route(unsigned irq, uint32_t* restrict gsi, unsigned* restrict trigger, unsigned* restrict polarity);

/// Routes a GSI to a handler; every handler on a shared GSI is called for each interrupt
/// @pre The device framework is the caller. Drivers and firmware code use device_request_irq,
/// so that the node relies on the node of the interrupt controller
/// @retval -ENODEV No interrupt controller has the GSI
/// @retval -EBUSY The GSI is taken without sharing, or with another trigger or polarity
int interrupt_attach(uint32_t gsi, unsigned trigger, unsigned polarity, unsigned flags, irq_handler_t handler, void* restrict ctxdata,
    interrupt_action_t** restrict action);

/// Routes a message-signalled source to its own handler. intctl selects the hart and the vector
/// @param source The argument for each call to ops
/// @retval -ENOMEM Out of memory, or no vector is free on any hart
int interrupt_attach_message(const interrupt_message_ops_t* restrict ops, void* restrict source, irq_handler_t handler,
    void* restrict ctxdata, interrupt_action_t** restrict action);

/// @post The handler isn't running anywhere and won't be called again
void interrupt_detach(interrupt_action_t* restrict action);
