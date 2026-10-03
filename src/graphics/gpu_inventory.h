#pragma once
#include <cstdint>
#include <string>

namespace rex::graphics { class RegisterFile; }
namespace rex::graphics::gpu_inventory {
bool IsActive();
void Record(std::string key);
void EndFrame();
// Opt-in ordered reference capture, independent of the aggregated inventory.
// GPU-thread only; starts at the first nonzero window offset on a 1280-wide
// target and ends after that partial frame plus two complete frames.
void TraceTilingPacket(const RegisterFile& regs, uint32_t packet,
                       uint64_t bin_mask, uint64_t bin_select);
bool IsTilingTraceActive();
void TraceTilingDetail(std::string line);
}  // namespace rex::graphics::gpu_inventory
