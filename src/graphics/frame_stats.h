/**
 * @file        graphics/frame_stats.h
 *
 * @brief       Per-frame accounting of where the GPU command thread spends a
 *              frame, logged for frames slower than slow_frame_log_ms.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace rex::graphics::frame_stats {

// What a slow frame can be made of. Times are wall-clock microseconds spent on
// the thread that recorded them; counts are events.
enum Id : uint32_t {
  kCpIdle,             // GPU command thread waiting for the game to write commands
  kFenceWait,          // waiting for the host GPU to finish a submission
  kShaderTranslate,    // translating a shader on the command thread (not async)
  kPipelineCreate,     // creating a pipeline on the command thread (not async)
  kPipelineMiss,       // new pipelines seen (count only; async ones included)
  kTextureLoad,        // uploading/untiling texture data
  kReadbackResolve,    // resolve copied back to guest memory (count, bytes in extra)
  kMemoryInvalidate,   // CPU writes that tripped a GPU watch (count, bytes in extra)
  kMemexportFlush,     // host-GPU syncs to read memexport results back (bytes in extra)
  kCount
};

struct Counter {
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> us{0};
  std::atomic<uint64_t> extra{0};
};

inline std::array<Counter, kCount>& Counters() {
  static std::array<Counter, kCount> counters;
  return counters;
}

inline void Add(Id id, uint64_t us, uint64_t extra = 0) {
  Counter& c = Counters()[id];
  c.count.fetch_add(1, std::memory_order_relaxed);
  c.us.fetch_add(us, std::memory_order_relaxed);
  if (extra) c.extra.fetch_add(extra, std::memory_order_relaxed);
}

// Times a scope into `id`.
class Scope {
 public:
  explicit Scope(Id id) : id_(id), start_(std::chrono::steady_clock::now()) {}
  ~Scope() {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - start_)
                  .count();
    Add(id_, static_cast<uint64_t>(us));
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Id id_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace rex::graphics::frame_stats
