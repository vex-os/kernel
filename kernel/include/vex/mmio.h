#pragma once

#include <vex/address.h>
#include <vex/compiler.h>
#include <vex/types.h>
#include <vex/vprot.h>

void* mmio_map(phys_addr_t phys, size_t size, unsigned vprot) __nodiscard;
void mmio_unmap(volatile void* virt);
bool mmio_owns(const volatile void* virt) __nodiscard;
