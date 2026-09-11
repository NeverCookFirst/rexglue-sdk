#pragma once
/**
 * @file        system/file_fixups.h
 * @brief       Patch bytes as a title reads them, without touching its files
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Some title bugs live in shipped data rather than in code: a
 *              script line the developers commented out, a table with a wrong
 *              entry. Editing the player's own game files to fix those is
 *              intrusive and fights with content verification, so a fixup
 *              rewrites the bytes in flight instead - the title sees the data
 *              it should have shipped with, and nothing on disk changes.
 *
 *              A fixup must verify what it is about to overwrite and do
 *              nothing when the bytes are not what it expects. That keeps it
 *              harmless on a different update, a different region, or data
 *              somebody has already patched by hand.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace rex::system {

/// Called for every successful file read.
/// @param name    file name as the title opened it, e.g. "PATCH.DAT"
/// @param offset  absolute byte offset the read started at
/// @param data    host pointer to the bytes just delivered to the guest
/// @param length  number of bytes delivered
using FileReadFixup =
    std::function<void(const std::string& name, uint64_t offset, uint8_t* data, size_t length)>;

/// Registers a fixup. Not thread-safe: register during startup, before the
/// title runs. Fixups are applied in registration order.
void RegisterFileReadFixup(FileReadFixup fixup);

/// True when at least one fixup is registered. The read path checks this
/// first so a build with no fixups pays nothing.
bool HasFileReadFixups();

/// Runs every registered fixup over a block that has just been read.
void ApplyFileReadFixups(const std::string& name, uint64_t offset, uint8_t* data, size_t length);

}  // namespace rex::system
