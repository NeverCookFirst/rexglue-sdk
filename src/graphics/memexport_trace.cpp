/**
 ******************************************************************************
 * @file        graphics/memexport_trace.cpp
 * @brief       On-demand diagnostics for guest shader memory exports
 *
 * @copyright   Copyright (c) 2026 NeverCookFirst
 * @license     BSD 3-Clause License
 ******************************************************************************
 */

#include "memexport_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace rex::graphics::memexport_trace {
namespace {

constexpr uint32_t kPhysicalMemorySize = 512u << 20;
constexpr uint32_t kPageSizeLog2 = 12;
constexpr uint32_t kPageCount = kPhysicalMemorySize >> kPageSizeLog2;
constexpr uint32_t kPageWordCount = kPageCount / 64;

struct DrawKey {
  uint64_t vertex_shader_hash;
  uint64_t pixel_shader_hash;
  bool vertex_shader_exports;
  bool pixel_shader_exports;

  auto AsTuple() const {
    return std::tie(vertex_shader_hash, pixel_shader_hash, vertex_shader_exports,
                    pixel_shader_exports);
  }
  bool operator<(const DrawKey& other) const { return AsTuple() < other.AsTuple(); }
};

struct DrawStats {
  uint64_t draws = 0;
  uint64_t vertices = 0;
  uint64_t ranges = 0;
  uint64_t bytes = 0;
};

struct ReadStats {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> bytes{0};
};

std::atomic<bool> active{false};
std::atomic<bool> waiting_for_frame_boundary{false};
std::atomic<uint32_t> frames_remaining{0};
std::array<std::atomic<uint64_t>, kPageWordCount> exported_pages{};
std::array<std::atomic<uint64_t>, kPageWordCount> previous_exported_pages{};
std::mutex detail_mutex;
std::map<DrawKey, DrawStats> draws;
std::map<std::string, uint64_t> publish_reasons;
ReadStats cpu_vertex_reads;
ReadStats wait_reg_mem_reads;
ReadStats data_provider_reads;
ReadStats cpu_writes;

void ClearStatistics() {
  {
    std::lock_guard<std::mutex> lock(detail_mutex);
    draws.clear();
    publish_reasons.clear();
  }
  for (ReadStats* stats : {&cpu_vertex_reads, &wait_reg_mem_reads, &data_provider_reads,
                           &cpu_writes}) {
    stats->calls.store(0, std::memory_order_relaxed);
    stats->bytes.store(0, std::memory_order_relaxed);
  }
}

void ClearPages() {
  for (uint32_t i = 0; i < kPageWordCount; ++i) {
    exported_pages[i].store(0, std::memory_order_relaxed);
    previous_exported_pages[i].store(0, std::memory_order_relaxed);
  }
}

void AdvancePageHistory() {
  for (uint32_t i = 0; i < kPageWordCount; ++i) {
    previous_exported_pages[i].store(
        exported_pages[i].exchange(0, std::memory_order_relaxed),
        std::memory_order_relaxed);
  }
}

bool OverlapsExportedPage(uint32_t address, uint32_t size) {
  if (!size || address >= kPhysicalMemorySize) {
    return false;
  }
  size = std::min(size, kPhysicalMemorySize - address);
  uint32_t first = address >> kPageSizeLog2;
  uint32_t last = (address + size - 1) >> kPageSizeLog2;
  for (uint32_t page = first; page <= last; ++page) {
    const uint64_t mask = uint64_t(1) << (page & 63);
    if ((exported_pages[page >> 6].load(std::memory_order_relaxed) |
         previous_exported_pages[page >> 6].load(std::memory_order_relaxed)) &
        mask) {
      return true;
    }
  }
  return false;
}

void AddRead(ReadStats& stats, uint32_t address, uint32_t size) {
  if (!active.load(std::memory_order_relaxed) || !OverlapsExportedPage(address, size)) {
    return;
  }
  stats.calls.fetch_add(1, std::memory_order_relaxed);
  stats.bytes.fetch_add(size, std::memory_order_relaxed);
}

void Arm(uint32_t frame_count) {
  frame_count = std::clamp(frame_count, 1u, 120u);
  active.store(false, std::memory_order_release);
  frames_remaining.store(frame_count, std::memory_order_relaxed);
  waiting_for_frame_boundary.store(true, std::memory_order_release);
  REXGPU_INFO("memexport_trace: armed for {} complete frame(s); capture starts after the next swap",
              frame_count);
}

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(
    memexport_trace,
    [](std::string_view args) {
      std::string text(args);
      char* end = nullptr;
      unsigned long count = std::strtoul(text.c_str(), &end, 10);
      if (!end || end == text.c_str()) {
        count = 1;
      }
      Arm(uint32_t(std::min<unsigned long>(count, 120)));
    },
    "GPU", "Trace shader memory exports for N complete frames: memexport_trace [N]");

bool IsActive() { return active.load(std::memory_order_relaxed); }

void RecordDraw(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash,
                bool vertex_shader_exports, bool pixel_shader_exports,
                uint32_t vertex_count, uint32_t range_count, uint32_t total_size) {
  if (!IsActive()) {
    return;
  }
  std::lock_guard<std::mutex> lock(detail_mutex);
  DrawStats& stats = draws[{vertex_shader_hash, pixel_shader_hash, vertex_shader_exports,
                            pixel_shader_exports}];
  ++stats.draws;
  stats.vertices += vertex_count;
  stats.ranges += range_count;
  stats.bytes += total_size;
}

void RecordRange(uint32_t address, uint32_t size) {
  if (!IsActive() || !size || address >= kPhysicalMemorySize) {
    return;
  }
  size = std::min(size, kPhysicalMemorySize - address);
  uint32_t first = address >> kPageSizeLog2;
  uint32_t last = (address + size - 1) >> kPageSizeLog2;
  for (uint32_t page = first; page <= last; ++page) {
    exported_pages[page >> 6].fetch_or(uint64_t(1) << (page & 63),
                                       std::memory_order_relaxed);
  }
}

void RecordHostRead(HostReadSource source, uint32_t address, uint32_t size) {
  switch (source) {
    case HostReadSource::kCpuVertexFetch:
      AddRead(cpu_vertex_reads, address, size);
      break;
    case HostReadSource::kWaitRegMem:
      AddRead(wait_reg_mem_reads, address, size);
      break;
    case HostReadSource::kDataProvider:
      AddRead(data_provider_reads, address, size);
      break;
  }
}

void RecordCpuWrite(uint32_t address_first, uint32_t address_last) {
  if (address_last < address_first) {
    return;
  }
  AddRead(cpu_writes, address_first, address_last - address_first + 1);
}

void RecordPublish(const char* reason) {
  if (!IsActive()) {
    return;
  }
  std::lock_guard<std::mutex> lock(detail_mutex);
  ++publish_reasons[reason ? reason : "unknown"];
}

void EndFrame() {
  if (waiting_for_frame_boundary.exchange(false, std::memory_order_acq_rel)) {
    ClearStatistics();
    ClearPages();
    active.store(true, std::memory_order_release);
    REXGPU_INFO("memexport_trace: capture started");
    return;
  }
  if (!active.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  std::vector<std::pair<DrawKey, DrawStats>> draw_snapshot;
  std::map<std::string, uint64_t> publish_snapshot;
  {
    std::lock_guard<std::mutex> lock(detail_mutex);
    draw_snapshot.assign(draws.begin(), draws.end());
    publish_snapshot = publish_reasons;
  }
  std::sort(draw_snapshot.begin(), draw_snapshot.end(), [](const auto& a, const auto& b) {
    return a.second.draws > b.second.draws;
  });

  uint64_t exported_page_count = 0;
  for (auto& word : exported_pages) {
    exported_page_count += std::popcount(word.load(std::memory_order_relaxed));
  }
  auto count = [](const ReadStats& stats) { return stats.calls.load(std::memory_order_relaxed); };
  auto bytes = [](const ReadStats& stats) { return stats.bytes.load(std::memory_order_relaxed); };
  REXGPU_INFO(
      "memexport_trace: frame: {} shader pair(s), {} exported page(s) | CPU-VS reads {} ({} KB) | "
      "WAIT_REG_MEM {} ({} KB) | provider {} ({} KB) | CPU writes {} ({} KB)",
      draw_snapshot.size(), exported_page_count, count(cpu_vertex_reads),
      bytes(cpu_vertex_reads) >> 10, count(wait_reg_mem_reads), bytes(wait_reg_mem_reads) >> 10,
      count(data_provider_reads), bytes(data_provider_reads) >> 10, count(cpu_writes),
      bytes(cpu_writes) >> 10);
  for (const auto& [key, stats] : draw_snapshot) {
    REXGPU_INFO(
        "memexport_trace:   VS={:016X}{} PS={:016X}{} draws={} vertices={} ranges={} bytes={} KB",
        key.vertex_shader_hash, key.vertex_shader_exports ? "*" : " ", key.pixel_shader_hash,
        key.pixel_shader_exports ? "*" : " ", stats.draws, stats.vertices, stats.ranges,
        stats.bytes >> 10);
  }
  if (!publish_snapshot.empty()) {
    std::string line;
    for (const auto& [reason, reason_count] : publish_snapshot) {
      line += fmt::format(" {}={}", reason, reason_count);
    }
    REXGPU_INFO("memexport_trace: publish/observe reasons:{}", line);
  }

  uint32_t remaining = frames_remaining.load(std::memory_order_relaxed);
  remaining = remaining ? remaining - 1 : 0;
  frames_remaining.store(remaining, std::memory_order_relaxed);
  ClearStatistics();
  AdvancePageHistory();
  if (remaining) {
    active.store(true, std::memory_order_release);
  } else {
    REXGPU_INFO("memexport_trace: capture complete");
  }
}

}  // namespace rex::graphics::memexport_trace
