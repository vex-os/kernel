#pragma once

#include <limine.h>
#include <vex/address.h>
#include <vex/compiler.h>
#include <vex/types.h>

extern const struct limine_memmap_response* memmap;

bool memmap_is_ram_type(uint64_t type) __nodiscard;
bool memmap_is_ram(phys_addr_t phys, size_t size) __nodiscard;
