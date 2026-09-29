/**
 ******************************************************************************
 * @file        graphics/memexport_trace.h
 * @brief       On-demand diagnostics for guest shader memory exports
 *
 * @copyright   Copyright (c) 2026 NeverCookFirst
 * @license     BSD 3-Clause License
 ******************************************************************************
 */

#pragma once

#include <cstdint>

namespace rex::graphics::memexport_trace {

enum class HostReadSource : uint32_t {
  kCpuVertexFetch,
  kWaitRegMem,
  kDataProvider,
};

// The console command `memexport_trace <frames>` arms this collector. The
// current partial frame is discarded, then exactly the requested number of
// complete frames are reported at swap. All calls are cheap no-ops otherwise.
bool IsActive();
void RecordDraw(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash,
                bool vertex_shader_exports, bool pixel_shader_exports,
                uint32_t vertex_count, uint32_t range_count, uint32_t total_size);
void RecordRange(uint32_t address, uint32_t size);
void RecordHostRead(HostReadSource source, uint32_t address, uint32_t size);
void RecordCpuWrite(uint32_t address_first, uint32_t address_last);
void RecordPublish(const char* reason);
void EndFrame();

}  // namespace rex::graphics::memexport_trace
