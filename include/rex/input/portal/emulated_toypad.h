/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Ported from the Xenia-Seamless-Toypad-Build fork, 2026 -
 *              adapted for the ReXGlue runtime.
 */

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <rex/input/portal/portal.h>

namespace rex::input {

constexpr size_t kToypadFigureCount = 7;
constexpr size_t kToypadTagSize = 0x2D * 0x04;  // 180 bytes, NTAG213 dump
// Upper bound for queued toypad replies; oldest frames are dropped beyond it.
constexpr size_t kMaxQueuedResponses = 256;

// GET_LED reply, companion-app wire format v2:
//   'L', serial, version, region count, then 3 regions x 12 bytes
//   (pad, mode, r, g, b, fromR, fromG, fromB, onTicks, offTicks, count,
//    speedTicks).
// Durations are toypad ticks exactly as the game sends them, never converted.
constexpr uint8_t kLedMagic = 0x4C;
constexpr uint8_t kLedProtocolVersion = 2;
constexpr uint8_t kLedRegionCount = 3;
constexpr size_t kLedRegionStride = 12;
constexpr size_t kLedSnapshotSize = 4 + kLedRegionCount * kLedRegionStride;

// Mode values are the companion app's LedMode enum.
enum class ToypadLedMode : uint8_t { Off = 0, Solid = 1, Flash = 2, Fade = 3 };

// One pad's last light command, held verbatim.
struct PadLed {
  uint8_t mode = static_cast<uint8_t>(ToypadLedMode::Off);
  uint8_t r = 0, g = 0, b = 0;
  // What the pad was showing when this command arrived. Real hardware fades
  // from it, so the app needs it to reproduce a cross-fade.
  uint8_t from_r = 0, from_g = 0, from_b = 0;
  uint8_t on_ticks = 0, off_ticks = 0, count = 0, speed_ticks = 0;
};

struct ToypadFigure {
  std::string file_path;  // optional .bin backing file, empty = in-memory only
  std::array<uint8_t, kToypadTagSize> data{};
  uint8_t index = 255;  // 1-based slot index as reported to the game, 255=free
  uint8_t pad = 255;    // 1=centre, 2=left, 3=right
  uint32_t id = 0;
  void Save();
};

// Emulated LEGO Dimensions ToyPad. Protocol logic is a port of the
// dimensions_toypad implementation from RPCS3 (rpcs3/Emu/Io/Dimensions.cpp,
// GPLv2) as used by the RPCS3-Seamless-Toypad-Build companion project.
// A loopback TCP listener accepts LOAD/REMOVE/MOVE commands from the
// LegoToypad companion app (same wire contract as the Cemu/RPCS3 forks).
class EmulatedToypad final : public Portal {
 public:
  EmulatedToypad();
  ~EmulatedToypad() override;

  bool IsConnected() override { return true; }
  void OnDeviceArrival() override {}
  void OnDeviceRemoval() override {}

  // True while the companion app's picker overlay is open; gamepad input
  // should not reach the game.
  static bool IsPickerInputActive();

  // Listener-driven state changes (called from the listener thread).
  uint32_t LoadFigure(const std::array<uint8_t, kToypadTagSize>& buf, std::string file_path,
                      uint8_t pad, uint8_t index, bool lock);
  bool RemoveFigure(uint8_t pad, uint8_t index, bool full_remove, bool lock);
  bool TempRemove(uint8_t index);
  bool CancelRemove(uint8_t index);
  bool MoveFigure(uint8_t pad, uint8_t index, uint8_t old_pad, uint8_t old_index);

 private:
  void OpenDevice() override {}
  void CloseDevice() override {}
  X_STATUS ReadInternal(std::span<uint8_t> data, int32_t& read_count) override;
  X_STATUS WriteInternal(std::span<uint8_t> data) override;

  void HandleCommand(const uint8_t* frame, size_t frame_size);

  // Protocol helpers.
  static uint8_t GenerateChecksum(const std::array<uint8_t, 32>& data, uint32_t num_of_bytes);
  static void GetBlankResponse(uint8_t type, uint8_t sequence, std::array<uint8_t, 32>& reply_buf);
  void GenerateRandomNumber(const uint8_t* buf, uint8_t sequence,
                            std::array<uint8_t, 32>& reply_buf);
  void InitializeRNG(uint32_t seed);
  uint32_t GetNext();
  void GetChallengeResponse(const uint8_t* buf, uint8_t sequence,
                            std::array<uint8_t, 32>& reply_buf);
  void QueryBlock(uint8_t index, uint8_t page, std::array<uint8_t, 32>& reply_buf,
                  uint8_t sequence);
  void WriteBlock(uint8_t index, uint8_t page, const uint8_t* to_write_buf,
                  std::array<uint8_t, 32>& reply_buf, uint8_t sequence);
  void GetModel(const uint8_t* buf, uint8_t sequence, std::array<uint8_t, 32>& reply_buf);

  static std::array<uint8_t, 8> Decrypt(const uint8_t* buf,
                                        std::optional<std::array<uint8_t, 16>> key);
  static std::array<uint8_t, 8> Encrypt(const uint8_t* buf,
                                        std::optional<std::array<uint8_t, 16>> key);
  static std::array<uint8_t, 16> GenerateFigureKey(const std::array<uint8_t, kToypadTagSize>& buf);
  static uint32_t Scramble(const std::array<uint8_t, 7>& uid, uint8_t count);
  static std::array<uint8_t, 4> Randomize(const std::vector<uint8_t>& key, uint8_t count);
  static uint32_t GetFigureId(const std::array<uint8_t, kToypadTagSize>& buf);
  ToypadFigure& GetFigureByIndex(uint8_t index);

  void PushResponse(std::array<uint8_t, 32> frame);

  // The light command the game last gave a pad. Kept so the companion app can
  // show the pads the way the game lit them; the game never reads it back.
  //
  // Storing the whole command rather than just its colour is what lets the app
  // reproduce a flash or a cross-fade: the emulated toypad records commands,
  // not animation progress, and the app runs the clock at its end.
  void RecordPadLed(uint8_t pad, uint8_t mode, const uint8_t* rgb, uint8_t on_ticks,
                    uint8_t off_ticks, uint8_t count, uint8_t speed_ticks);
  // One GET_LED reply in companion-app wire format v2.
  std::array<uint8_t, kLedSnapshotSize> LedSnapshot();

  // Listener.
  void ListenerRun(uint16_t port);
  // Serves one companion-app connection until the peer closes it. Runs on its
  // own thread so a slow command cannot stall the accept loop.
  void HandleClient(uintptr_t client_socket);
  // Reads and runs a single command. False means the connection is finished.
  bool HandleCommand(uintptr_t client_socket);
  void ForgetClient(uintptr_t client_socket);
  void PickerWatchRun();

  std::mutex state_lock_;
  std::array<ToypadFigure, kToypadFigureCount> figures_{};
  std::queue<std::array<uint8_t, 32>> responses_;
  // Per pad, in pad order (1 centre, 2 left, 3 right).
  std::array<PadLed, 3> pad_leds_{};
  // Bumped whenever a pad's command actually changes, so the companion app can
  // skip a snapshot it has already drawn.
  uint8_t led_serial_ = 0;

  uint32_t random_a_ = 0;
  uint32_t random_b_ = 0;
  uint32_t random_c_ = 0;
  uint32_t random_d_ = 0;

  // Frame wrapping detected from the game's writes. -1 = not yet known,
  // 0 = raw 0x55/0x56 frames, >0 = number of prefix bytes before 0x55.
  int frame_offset_ = -1;
  uint8_t frame_prefix_byte_ = 0;

  std::thread listener_thread_;
  std::thread picker_thread_;
  std::atomic<bool> listener_running_{false};
  std::atomic<uintptr_t> listen_socket_{~uintptr_t(0)};

  // One thread per open companion-app connection. Commands still run one at a
  // time under command_lock_, so placement order is unchanged; what this buys
  // is that a client may hold its connection open - the LED poll no longer has
  // to reconnect for every read, and a 500 ms MOVE no longer overflows the
  // accept backlog and looks to the app like the game dropped the socket.
  std::mutex command_lock_;
  std::mutex clients_lock_;
  std::vector<uintptr_t> client_sockets_;
  std::atomic<int> client_count_{0};
};

}  // namespace rex::input
