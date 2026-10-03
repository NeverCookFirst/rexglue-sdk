#include "gpu_inventory.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string_view>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/graphics/register_file.h>
#include <xxhash.h>

namespace rex::graphics::gpu_inventory {
namespace {
std::atomic<bool> active{false};
std::mutex mutex;
std::map<std::string, uint64_t> entries;
uint32_t pending = 0, remaining = 0, captured = 0;
uint64_t overflow = 0;
constexpr size_t kMaxKeys = 32768;

struct TilingTrace {
  std::filesystem::path path;
  std::ofstream file;
  bool active = false;
  bool finished = false;
  uint32_t frame = 0;
  uint32_t events = 0;
  TilingTrace() {
    const char* value = std::getenv("LEGO_XENOS_TILING_TRACE");
    if (value && *value) path = value;
  }
};
// All callers run on the command-processor thread. No render state is changed.
TilingTrace& TileTrace() { static TilingTrace trace; return trace; }
void FinishTilingTrace(bool overflow) {
  auto& trace = TileTrace();
  trace.file << "END frames=" << trace.frame << " events=" << trace.events
             << " overflow=" << overflow << '\n';
  trace.file.close();
  trace.active = false;
  trace.finished = true;
  REXLOG_INFO("Xenos tiling trace complete: {} events, overflow={}", trace.events, overflow);
}
}

bool IsTilingTraceActive() { return TileTrace().active; }
void TraceTilingDetail(std::string line) {
  auto& trace = TileTrace();
  if (!trace.active) return;
  if (trace.events >= 20000) { FinishTilingTrace(true); return; }
  trace.file << "f=" << trace.frame << " event=" << trace.events++ << ' ' << line << '\n';
}

void TraceTilingPacket(const RegisterFile& regs, uint32_t packet,
                       uint64_t bin_mask, uint64_t bin_select) {
  auto& trace = TileTrace();
  if (trace.path.empty() || trace.finished) return;
  const auto window = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  if (!trace.active) {
    if (regs.Get<reg::RB_SURFACE_INFO>().surface_pitch != 1280 ||
        (!window.window_x_offset && !window.window_y_offset)) return;
    trace.file.open(trace.path, std::ios::out | std::ios::trunc);
    if (!trace.file) {
      trace.finished = true;
      REXLOG_WARN("Xenos tiling trace could not open {}", trace.path.string());
      return;
    }
    trace.active = true;
    trace.file << "BEGIN partial frame0 then two complete frames; raw register words in hex\n";
    REXLOG_INFO("Xenos tiling trace started: {}", trace.path.string());
  }
  const auto hash_bank = [&](uint32_t first, uint32_t dwords) {
    return XXH3_64bits(&regs.values[first], size_t(dwords) * sizeof(uint32_t));
  };
  TraceTilingDetail(fmt::format(
      "packet={:08X} execute={} mask={:016X} select={:016X} window={},{} "
      "screen={:08X},{:08X} scissor={:08X},{:08X} mode={:08X} "
      "viewport={:08X},{:08X},{:08X},{:08X},{:08X},{:08X} "
      "vsconst={:016X} psconst={:016X} fetch={:016X} "
      "surface={:08X} rt={:08X},{:08X},{:08X},{:08X} depth={:08X} "
      "copy={:08X} dest={:08X},{:08X},{:08X}",
      packet, !(packet & 1) || ((bin_mask & bin_select) != 0), bin_mask, bin_select,
      int32_t(window.window_x_offset), int32_t(window.window_y_offset),
      regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR],
      regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
      regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
      regs[XE_GPU_REG_PA_CL_VPORT_XSCALE], regs[XE_GPU_REG_PA_CL_VPORT_XOFFSET],
      regs[XE_GPU_REG_PA_CL_VPORT_YSCALE], regs[XE_GPU_REG_PA_CL_VPORT_YOFFSET],
      regs[XE_GPU_REG_PA_CL_VPORT_ZSCALE], regs[XE_GPU_REG_PA_CL_VPORT_ZOFFSET],
      hash_bank(XE_GPU_REG_SHADER_CONSTANT_000_X, 256 * 4),
      hash_bank(XE_GPU_REG_SHADER_CONSTANT_000_X + 256 * 4, 256 * 4),
      hash_bank(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 32 * 6),
      regs[XE_GPU_REG_RB_SURFACE_INFO],
      regs[reg::RB_COLOR_INFO::rt_register_indices[0]], regs[reg::RB_COLOR_INFO::rt_register_indices[1]],
      regs[reg::RB_COLOR_INFO::rt_register_indices[2]], regs[reg::RB_COLOR_INFO::rt_register_indices[3]],
      regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_RB_COPY_CONTROL],
      regs[XE_GPU_REG_RB_COPY_DEST_BASE], regs[XE_GPU_REG_RB_COPY_DEST_PITCH],
      regs[XE_GPU_REG_RB_COPY_DEST_INFO]));
}
REXCVAR_DEFINE_COMMAND_ARGS(
    gpu_inventory,
    [](std::string_view args) {
      std::string text(args);
      char* end = nullptr;
      unsigned long frames = std::strtoul(text.c_str(), &end, 10);
      if (end == text.c_str()) frames = 60;
      std::lock_guard lock(mutex);
      active.store(false, std::memory_order_relaxed);
      entries.clear();
      remaining = captured = 0;
      overflow = 0;
      pending = uint32_t(std::clamp(frames, 1ul, 600ul));
      REXLOG_INFO("GPU inventory armed: {} complete frames, starts at next swap", pending);
    }, "GPU", "Inventory D3D12 Xenos workload: gpu_inventory [frames, default 60]");

bool IsActive() { return active.load(std::memory_order_relaxed); }
void Record(std::string key) {
  if (!IsActive()) return;
  std::lock_guard lock(mutex);
  if (!IsActive()) return;
  auto found = entries.find(key);
  if (found != entries.end()) ++found->second;
  else if (entries.size() < kMaxKeys) entries.emplace(std::move(key), 1);
  else ++overflow;
}
void EndFrame() {
  auto& trace = TileTrace();
  if (trace.active) {
    trace.file << "SWAP frame=" << trace.frame << '\n';
    if (++trace.frame == 3) FinishTilingTrace(false);
  }
  std::lock_guard lock(mutex);
  if (pending) {
    remaining = pending;
    pending = 0;
    active.store(true, std::memory_order_relaxed);
    return;
  }
  if (!IsActive()) return;
  ++captured;
  if (--remaining) return;
  active.store(false, std::memory_order_relaxed);
  REXLOG_INFO("GPU inventory BEGIN frames={} unique={} overflow_events={}",
              captured, entries.size(), overflow);
  for (const auto& [key, count] : entries)
    REXLOG_INFO("GPU inventory {} count={}", key, count);
  REXLOG_INFO("GPU inventory END (observed workload only; draw attempts are not proof of completed GPU work)");
  entries.clear();
}
}  // namespace rex::graphics::gpu_inventory
