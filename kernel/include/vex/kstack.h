#pragma once

#include <vex/address.h>
#include <vex/compiler.h>
#include <vex/limits.h>
#include <vex/types.h>

#define KSTACK_PAGES ((unsigned)(CONFIG_KSTACK_PAGES))
#define KSTACK_SIZE  (KSTACK_PAGES * PAGE_SIZE)

void* kstack_alloc(void) __nodiscard;
void kstack_free(void* restrict top);
bool kstack_is_guard(virt_addr_t address) __nodiscard;
