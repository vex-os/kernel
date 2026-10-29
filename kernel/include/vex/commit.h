#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

extern size_t commit_limit_pages;
extern size_t commit_charged_pages; // Atomic; read it with atomic_load

/// Takes pages from the commit limit
/// @retval -ENOMEM The total would pass the limit; nothing is taken
/// @note Takes no lock, so any context may call it
int commit_charge(size_t pages);

/// Takes pages from the commit limit, as a mapping that replaces others does
/// @param replaced Pages that the caller gives back with commit_uncharge once it holds the new ones
/// @retval -ENOMEM The total would pass the limit by more than replaced; nothing is taken
/// @note Until the caller gives the pages back, the total may pass the limit, so every other charge fails
int commit_charge_replacing(size_t pages, size_t replaced);

/// Gives back pages that commit_charge took
/// @pre The caller holds at least that many pages
void commit_uncharge(size_t pages);
