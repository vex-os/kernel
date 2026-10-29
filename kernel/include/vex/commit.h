#pragma once

#include <vex/compiler.h>
#include <vex/types.h>

/// Takes pages from the commit limit
/// @retval -ENOMEM The total would pass the limit; nothing is taken
/// @note Takes no lock, so any context may call it
int commit_charge(size_t pages);

/// Gives back pages that commit_charge took
/// @pre The caller holds at least that many pages
void commit_uncharge(size_t pages);
