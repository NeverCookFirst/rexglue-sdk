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

// winsock2.h must be included before anything that could pull in windows.h.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <rex/input/portal/emulated_toypad.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

#include <rex/logging.h>

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rex::input {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
void CloseSock(socket_t s) { ::closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
void CloseSock(socket_t s) { ::close(s); }
#endif

constexpr auto kMovePickupDelay = std::chrono::milliseconds(500);

// How long a connection may sit silent before the listener drops it. The
// companion app polls the pad colours several times a second and reconnects
// on its own, so this only ever fires on a client that has gone away without
// closing - and it is what stops such a client leaking a thread and a socket.
constexpr int kClientIdleTimeoutMs = 60000;

// A blocking recv with no deadline is how one silent client used to wedge the
// whole listener; every accepted socket gets one.
void SetRecvTimeout(socket_t s, int milliseconds) {
#ifdef _WIN32
  const DWORD timeout = static_cast<DWORD>(milliseconds);
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
#else
  timeval timeout{};
  timeout.tv_sec = milliseconds / 1000;
  timeout.tv_usec = (milliseconds % 1000) * 1000;
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

// Companion-app input handoff (same cross-repo contract as the Cemu and
// RPCS3 forks): LegoToypad keeps this manual-reset event signaled while its
// picker overlay is visible.
#ifdef _WIN32
constexpr wchar_t kPickerEventName[] = L"Local\\CemuToypadPickerInputActive";
#endif

std::atomic<bool> s_picker_active{false};

constexpr std::array<uint8_t, 16> kCommandKey = {0x55, 0xFE, 0xF6, 0xB0, 0x62, 0xBF, 0x0B, 0x41,
                                                 0xC9, 0xB3, 0x7C, 0xB4, 0x97, 0x3E, 0x29, 0x7B};

constexpr std::array<uint8_t, 17> kCharConstant = {0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA,
                                                   0x3C, 0xA8, 0xD8, 0x75, 0x47, 0x68,
                                                   0xCF, 0x23, 0xE9, 0xFE, 0xAA};

uint32_t ReadLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint32_t ReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
void WriteLE32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}
void WriteBE32(uint8_t* p, uint32_t v) {
  p[0] = (v >> 24) & 0xFF;
  p[1] = (v >> 16) & 0xFF;
  p[2] = (v >> 8) & 0xFF;
  p[3] = v & 0xFF;
}

std::string ToHex(const uint8_t* data, size_t size) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(size * 3);
  for (size_t i = 0; i < size; i++) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0xF]);
    out.push_back(' ');
  }
  return out;
}

bool SendAll(socket_t s, const uint8_t* data, size_t len) {
  while (len != 0) {
    const auto sent = ::send(s, reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
    if (sent <= 0) {
      return false;
    }
    data += sent;
    len -= static_cast<size_t>(sent);
  }
  return true;
}

bool RecvAll(socket_t s, uint8_t* data, size_t len) {
  while (len != 0) {
    const auto received = ::recv(s, reinterpret_cast<char*>(data), static_cast<int>(len), 0);
    if (received <= 0) {
      return false;
    }
    data += received;
    len -= static_cast<size_t>(received);
  }
  return true;
}

uint16_t ListenerPort() {
  // REXGLUE_TOYPAD_PORT is the native name; XENIA_TOYPAD_PORT stays supported
  // so an existing companion-app setup keeps working unchanged.
  for (const char* name : {"REXGLUE_TOYPAD_PORT", "XENIA_TOYPAD_PORT"}) {
    if (const char* env = std::getenv(name)) {
      const int port = std::atoi(env);
      if (port >= 1 && port <= 65535) {
        return static_cast<uint16_t>(port);
      }
    }
  }
  return 9191;
}

}  // namespace

void ToypadFigure::Save() {
  if (file_path.empty()) {
    return;
  }
  std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    REXLOG_WARN("Toypad: could not save figure to {}", file_path);
    return;
  }
  out.write(reinterpret_cast<const char*>(data.data()), data.size());
}

EmulatedToypad::EmulatedToypad() : Portal() {
  std::srand(static_cast<unsigned>(std::time(nullptr)));

#ifdef _WIN32
  WSADATA wsa_data{};
  WSAStartup(MAKEWORD(2, 2), &wsa_data);  // ref-counted, safe if already done
#endif

  listener_running_.store(true);
  listener_thread_ = std::thread(&EmulatedToypad::ListenerRun, this, ListenerPort());
#ifdef _WIN32
  picker_thread_ = std::thread(&EmulatedToypad::PickerWatchRun, this);
#endif
  REXLOG_INFO("Toypad: emulated LEGO Dimensions ToyPad active");
}

EmulatedToypad::~EmulatedToypad() {
  listener_running_.store(false);

  const socket_t sock = static_cast<socket_t>(listen_socket_.exchange(~uintptr_t(0)));
  if (sock != kInvalidSocket) {
    CloseSock(sock);  // unblocks accept()
  }
  if (listener_thread_.joinable()) {
    listener_thread_.join();
  }
  if (picker_thread_.joinable()) {
    picker_thread_.join();
  }
#ifdef _WIN32
  WSACleanup();
#endif
}

bool EmulatedToypad::IsPickerInputActive() { return s_picker_active.load(); }

// ============================================================================
// Guest-facing read/write (XamInputNonControllerGetRawEx/SetRawEx)
// ============================================================================

X_STATUS EmulatedToypad::ReadInternal(std::span<uint8_t> data, int32_t& read_count) {
  std::array<uint8_t, 32> frame;
  {
    std::lock_guard<std::mutex> guard(state_lock_);
    if (responses_.empty()) {
      read_count = 0;
      return X_ERROR_SUCCESS;
    }
    frame = responses_.front();
    responses_.pop();
  }

  std::array<uint8_t, 32> out{};
  // Total frame length: header (type + len) + payload + checksum.
  const size_t frame_len = std::min<size_t>(size_t(frame[1]) + 3, out.size());
  if (frame_offset_ > 0) {
    // Mirror the wrapping style the game uses for its own writes.
    out[0] = frame_prefix_byte_;
    out[1] = static_cast<uint8_t>(frame_len);
    std::memcpy(&out[2], frame.data(), std::min(frame_len, out.size() - 2));
  } else {
    std::memcpy(out.data(), frame.data(), frame_len);
  }

  const size_t count = std::min(out.size(), data.size());
  std::memcpy(data.data(), out.data(), count);
  read_count = static_cast<int32_t>(count);

  static int logged_reads = 0;
  if (logged_reads < 64) {
    logged_reads++;
    REXLOG_INFO("Toypad[Read ] {}", ToHex(out.data(), count));
  }
  return X_ERROR_SUCCESS;
}

X_STATUS EmulatedToypad::WriteInternal(std::span<uint8_t> data) {
  static int logged_writes = 0;
  if (logged_writes < 256) {
    logged_writes++;
    REXLOG_INFO("Toypad[Write] {}", ToHex(data.data(), data.size()));
  }

  // Locate the 0x55 command frame; Xbox 360 transport may prepend a short
  // wrapper (as seen with X360 Skylanders portals: 0x0B <len> <payload>).
  size_t offset = 0;
  bool found = false;
  const size_t scan_limit = std::min<size_t>(4, data.size());
  for (; offset < scan_limit; offset++) {
    if (data[offset] == 0x55) {
      found = true;
      break;
    }
  }
  if (!found) {
    REXLOG_WARN("Toypad[Write] no 0x55 frame found: {}", ToHex(data.data(), data.size()));
    return X_ERROR_SUCCESS;
  }

  if (frame_offset_ != static_cast<int>(offset)) {
    frame_offset_ = static_cast<int>(offset);
    frame_prefix_byte_ = offset > 0 ? data[0] : 0;
    REXLOG_INFO("Toypad: detected frame offset {} (prefix {:02X})", offset, frame_prefix_byte_);
  }

  HandleCommand(data.data() + offset, data.size() - offset);
  return X_ERROR_SUCCESS;
}

void EmulatedToypad::HandleCommand(const uint8_t* buf, size_t buf_size) {
  if (buf_size < 6) {
    return;
  }

  const uint8_t command = buf[2];
  const uint8_t sequence = buf[3];

  std::array<uint8_t, 32> result{};

  switch (command) {
    case 0xB0: {  // Wake
      result = {0x55, 0x0e, 0x01, 0x28, 0x63, 0x29, 0x20, 0x4c, 0x45,
                0x47, 0x4f, 0x20, 0x32, 0x30, 0x31, 0x34, 0x46};
      break;
    }
    case 0xB1: {  // Seed
      GenerateRandomNumber(&buf[4], sequence, result);
      break;
    }
    case 0xB3: {  // Challenge
      GetChallengeResponse(&buf[4], sequence, result);
      break;
    }
    // The pad lights. The game never reads these back, but the companion app
    // asks for them so it can show the pads the way the game lit them, so the
    // colour each command ends on is recorded. The payload layouts below are
    // the ones the game actually sends, read off captured frames; each per-pad
    // group ends with its RGB triplet, which is the only part worth keeping.
    // Pad 0 addresses all three at once, which is how the game lights them on
    // connect and, far more often, how it turns them all off again.
    case 0xC0: {  // Color: pad, R, G, B
      RecordPadLed(buf[4], static_cast<uint8_t>(ToypadLedMode::Solid), &buf[5], 0, 0, 0, 0);
      GetBlankResponse(0x01, sequence, result);
      break;
    }
    case 0xC2: {  // Fade: pad, time, count, R, G, B
      RecordPadLed(buf[4], static_cast<uint8_t>(ToypadLedMode::Fade), &buf[7], 0, 0, buf[6],
                   buf[5]);
      GetBlankResponse(0x01, sequence, result);
      break;
    }
    case 0xC3: {  // Flash: pad, on, off, count, R, G, B
      RecordPadLed(buf[4], static_cast<uint8_t>(ToypadLedMode::Flash), &buf[8], buf[5], buf[6],
                   buf[7], 0);
      GetBlankResponse(0x01, sequence, result);
      break;
    }
    case 0xC8: {  // Color All: 3 x (enabled, R, G, B)
      for (uint8_t pad = 1; pad <= kLedRegionCount; ++pad) {
        const uint8_t* group = &buf[4] + (pad - 1) * 4;
        if (group[0]) {
          RecordPadLed(pad, static_cast<uint8_t>(ToypadLedMode::Solid), &group[1], 0, 0, 0, 0);
        }
      }
      GetBlankResponse(0x01, sequence, result);
      break;
    }
    case 0xC6: {  // Fade All: 3 x (enabled, time, count, R, G, B)
      for (uint8_t pad = 1; pad <= kLedRegionCount; ++pad) {
        const uint8_t* group = &buf[4] + (pad - 1) * 6;
        if (group[0]) {
          RecordPadLed(pad, static_cast<uint8_t>(ToypadLedMode::Fade), &group[3], 0, 0, group[2],
                       group[1]);
        }
      }
      GetBlankResponse(0x01, sequence, result);
      break;
    }
    case 0xC7: {  // Flash All: 3 x (enabled, on, off, count, R, G, B)
      for (uint8_t pad = 1; pad <= kLedRegionCount; ++pad) {
        const uint8_t* group = &buf[4] + (pad - 1) * 7;
        if (group[0]) {
          RecordPadLed(pad, static_cast<uint8_t>(ToypadLedMode::Flash), &group[4], group[1],
                       group[2], group[3], 0);
        }
      }
      GetBlankResponse(0x01, sequence, result);
      break;
    }
    case 0xC1:    // Get Pad Color
    case 0xC4: {  // Fade Random
      GetBlankResponse(0x01, sequence, result);
      break;
    }
    case 0xD2: {  // Read
      QueryBlock(buf[4], buf[5], result, sequence);
      break;
    }
    case 0xD3: {  // Write
      WriteBlock(buf[4], buf[5], &buf[6], result, sequence);
      break;
    }
    case 0xD4: {  // Model
      GetModel(&buf[4], sequence, result);
      break;
    }
    default: {
      REXLOG_WARN("Toypad: unimplemented LD command {:02X}: {}", command, ToHex(buf, buf_size));
      break;
    }
  }
  PushResponse(result);
}

void EmulatedToypad::RecordPadLed(uint8_t pad, uint8_t mode, const uint8_t* rgb, uint8_t on_ticks,
                                  uint8_t off_ticks, uint8_t count, uint8_t speed_ticks) {
  // Pad 0 addresses all three at once, and the game leans on it: it is how the
  // pads are lit white on connect and, more often, how they are all turned off
  // again. Dropping it as an invalid pad meant every such command was ignored
  // and the companion app kept showing whatever colour happened to be there
  // last - most visibly, pads that never went dark.
  if (pad > kLedRegionCount) {
    return;
  }

  // A solid colour of pure black is the pad being switched off, and saying so
  // in the mode is what lets the app stop drawing a glow rather than draw a
  // black one.
  if (mode == static_cast<uint8_t>(ToypadLedMode::Solid) && rgb[0] == 0 && rgb[1] == 0 &&
      rgb[2] == 0) {
    mode = static_cast<uint8_t>(ToypadLedMode::Off);
  }

  bool changed = false;
  {
    std::lock_guard<std::mutex> guard(state_lock_);
    const size_t first = pad == 0 ? 0 : pad - 1u;
    const size_t last = pad == 0 ? pad_leds_.size() : first + 1;
    for (size_t i = first; i < last; ++i) {
      PadLed next;
      next.mode = mode;
      next.r = rgb[0];
      next.g = rgb[1];
      next.b = rgb[2];
      // The colour standing on the pad when this command landed.
      next.from_r = pad_leds_[i].r;
      next.from_g = pad_leds_[i].g;
      next.from_b = pad_leds_[i].b;
      next.on_ticks = on_ticks;
      next.off_ticks = off_ticks;
      next.count = count;
      next.speed_ticks = speed_ticks;

      const PadLed& current = pad_leds_[i];
      if (current.mode == next.mode && current.r == next.r && current.g == next.g &&
          current.b == next.b && current.on_ticks == next.on_ticks &&
          current.off_ticks == next.off_ticks && current.count == next.count &&
          current.speed_ticks == next.speed_ticks) {
        continue;  // the game repeats a standing command constantly
      }
      pad_leds_[i] = next;
      changed = true;
    }
    if (changed) {
      ++led_serial_;
    }
  }

  // Only on a change, so a game that repaints the same colour every frame does
  // not drown the log. This is the line to look for when the companion app
  // shows the wrong colour: it says what the game actually asked for.
  if (changed) {
    REXLOG_INFO("Toypad: pad {} mode {} lit {:02X} {:02X} {:02X} (on {} off {} count {} speed {})",
                pad, mode, rgb[0], rgb[1], rgb[2], on_ticks, off_ticks, count, speed_ticks);
  }
}

std::array<uint8_t, kLedSnapshotSize> EmulatedToypad::LedSnapshot() {
  std::array<uint8_t, kLedSnapshotSize> out{};
  out[0] = kLedMagic;
  out[2] = kLedProtocolVersion;
  out[3] = kLedRegionCount;

  std::lock_guard<std::mutex> guard(state_lock_);
  out[1] = led_serial_;
  for (size_t i = 0; i < pad_leds_.size(); ++i) {
    const PadLed& led = pad_leds_[i];
    uint8_t* region = out.data() + 4 + i * kLedRegionStride;
    region[0] = static_cast<uint8_t>(i + 1);  // 1 centre, 2 left, 3 right
    region[1] = led.mode;
    region[2] = led.r;
    region[3] = led.g;
    region[4] = led.b;
    region[5] = led.from_r;
    region[6] = led.from_g;
    region[7] = led.from_b;
    region[8] = led.on_ticks;
    region[9] = led.off_ticks;
    region[10] = led.count;
    region[11] = led.speed_ticks;
  }
  return out;
}

void EmulatedToypad::PushResponse(std::array<uint8_t, 32> frame) {
  std::lock_guard<std::mutex> guard(state_lock_);
  // The game can keep issuing LED commands while not draining replies (e.g.
  // while the window is unfocused); never let the queue grow without bound.
  while (responses_.size() >= kMaxQueuedResponses) {
    responses_.pop();
  }
  responses_.push(frame);
}

// ============================================================================
// LEGO Dimensions toypad protocol (ported from RPCS3's dimensions_toypad)
// ============================================================================

uint8_t EmulatedToypad::GenerateChecksum(const std::array<uint8_t, 32>& data,
                                         uint32_t num_of_bytes) {
  int checksum = 0;
  num_of_bytes = std::min<uint32_t>(num_of_bytes, uint32_t(data.size()));
  for (uint32_t i = 0; i < num_of_bytes; i++) {
    checksum += data[i];
  }
  return checksum & 0xFF;
}

void EmulatedToypad::GetBlankResponse(uint8_t type, uint8_t sequence,
                                      std::array<uint8_t, 32>& reply_buf) {
  reply_buf[0] = 0x55;
  reply_buf[1] = type;
  reply_buf[2] = sequence;
  reply_buf[3] = GenerateChecksum(reply_buf, 3);
}

void EmulatedToypad::GenerateRandomNumber(const uint8_t* buf, uint8_t sequence,
                                          std::array<uint8_t, 32>& reply_buf) {
  const std::array<uint8_t, 8> value = Decrypt(buf, std::nullopt);
  const uint32_t seed = ReadLE32(value.data());
  const uint32_t conf = ReadBE32(value.data() + 4);
  InitializeRNG(seed);
  std::array<uint8_t, 8> value_to_encrypt{};
  WriteBE32(value_to_encrypt.data(), conf);
  const std::array<uint8_t, 8> encrypted = Encrypt(value_to_encrypt.data(), std::nullopt);
  reply_buf[0] = 0x55;
  reply_buf[1] = 0x09;
  reply_buf[2] = sequence;
  std::memcpy(&reply_buf[3], encrypted.data(), encrypted.size());
  reply_buf[11] = GenerateChecksum(reply_buf, 11);
}

void EmulatedToypad::InitializeRNG(uint32_t seed) {
  random_a_ = 0xF1EA5EED;
  random_b_ = seed;
  random_c_ = seed;
  random_d_ = seed;
  for (int i = 0; i < 42; i++) {
    GetNext();
  }
}

uint32_t EmulatedToypad::GetNext() {
  const uint32_t e = random_a_ - std::rotl(random_b_, 21);
  random_a_ = random_b_ ^ std::rotl(random_c_, 19);
  random_b_ = random_c_ + std::rotl(random_d_, 6);
  random_c_ = random_d_ + e;
  random_d_ = e + random_a_;
  return random_d_;
}

std::array<uint8_t, 8> EmulatedToypad::Decrypt(const uint8_t* buf,
                                               std::optional<std::array<uint8_t, 16>> key) {
  uint32_t data_one = ReadLE32(buf);
  uint32_t data_two = ReadLE32(buf + 4);

  const uint8_t* k = key ? key->data() : kCommandKey.data();
  const uint32_t key_one = ReadLE32(k);
  const uint32_t key_two = ReadLE32(k + 4);
  const uint32_t key_three = ReadLE32(k + 8);
  const uint32_t key_four = ReadLE32(k + 12);

  uint32_t sum = 0xC6EF3720;
  constexpr uint32_t delta = 0x9E3779B9;
  for (int i = 0; i < 32; i++) {
    data_two -= (((data_one << 4) + key_three) ^ (data_one + sum) ^ ((data_one >> 5) + key_four));
    data_one -= (((data_two << 4) + key_one) ^ (data_two + sum) ^ ((data_two >> 5) + key_two));
    sum -= delta;
  }

  std::array<uint8_t, 8> decrypted;
  WriteLE32(decrypted.data(), data_one);
  WriteLE32(decrypted.data() + 4, data_two);
  return decrypted;
}

std::array<uint8_t, 8> EmulatedToypad::Encrypt(const uint8_t* buf,
                                               std::optional<std::array<uint8_t, 16>> key) {
  uint32_t data_one = ReadLE32(buf);
  uint32_t data_two = ReadLE32(buf + 4);

  const uint8_t* k = key ? key->data() : kCommandKey.data();
  const uint32_t key_one = ReadLE32(k);
  const uint32_t key_two = ReadLE32(k + 4);
  const uint32_t key_three = ReadLE32(k + 8);
  const uint32_t key_four = ReadLE32(k + 12);

  uint32_t sum = 0;
  constexpr uint32_t delta = 0x9E3779B9;
  for (int i = 0; i < 32; i++) {
    sum += delta;
    data_one += (((data_two << 4) + key_one) ^ (data_two + sum) ^ ((data_two >> 5) + key_two));
    data_two += (((data_one << 4) + key_three) ^ (data_one + sum) ^ ((data_one >> 5) + key_four));
  }

  std::array<uint8_t, 8> encrypted;
  WriteLE32(encrypted.data(), data_one);
  WriteLE32(encrypted.data() + 4, data_two);
  return encrypted;
}

std::array<uint8_t, 16> EmulatedToypad::GenerateFigureKey(
    const std::array<uint8_t, kToypadTagSize>& buf) {
  const std::array<uint8_t, 7> uid = {buf[0], buf[1], buf[2], buf[4], buf[5], buf[6], buf[7]};
  std::array<uint8_t, 16> figure_key{};
  WriteBE32(figure_key.data(), Scramble(uid, 3));
  WriteBE32(figure_key.data() + 4, Scramble(uid, 4));
  WriteBE32(figure_key.data() + 8, Scramble(uid, 5));
  WriteBE32(figure_key.data() + 12, Scramble(uid, 6));
  return figure_key;
}

uint32_t EmulatedToypad::Scramble(const std::array<uint8_t, 7>& uid, uint8_t count) {
  std::vector<uint8_t> to_scramble;
  to_scramble.reserve(uid.size() + kCharConstant.size());
  to_scramble.insert(to_scramble.end(), uid.begin(), uid.end());
  to_scramble.insert(to_scramble.end(), kCharConstant.begin(), kCharConstant.end());
  to_scramble[count * 4 - 1] = 0xAA;
  return ReadBE32(Randomize(to_scramble, count).data());
}

std::array<uint8_t, 4> EmulatedToypad::Randomize(const std::vector<uint8_t>& key, uint8_t count) {
  uint32_t scrambled = 0;
  for (uint8_t i = 0; i < count; i++) {
    const uint32_t v4 = std::rotr(scrambled, 25);
    const uint32_t v5 = std::rotr(scrambled, 10);
    const uint32_t b = ReadLE32(key.data() + i * 4);
    scrambled = b + v4 + v5 - scrambled;
  }
  return {uint8_t(scrambled & 0xFF), uint8_t((scrambled >> 8) & 0xFF),
          uint8_t((scrambled >> 16) & 0xFF), uint8_t((scrambled >> 24) & 0xFF)};
}

uint32_t EmulatedToypad::GetFigureId(const std::array<uint8_t, kToypadTagSize>& buf) {
  const std::array<uint8_t, 16> figure_key = GenerateFigureKey(buf);
  const std::array<uint8_t, 8> decrypted = Decrypt(&buf[36 * 4], figure_key);
  const uint32_t fig_num = ReadLE32(decrypted.data());
  // Characters have their model number encrypted in page 36.
  if (fig_num < 1000) {
    return fig_num;
  }
  // Vehicles/gadgets have their model number as plain little endian.
  return ReadLE32(&buf[36 * 4]);
}

ToypadFigure& EmulatedToypad::GetFigureByIndex(uint8_t index) { return figures_.at(index); }

void EmulatedToypad::GetChallengeResponse(const uint8_t* buf, uint8_t sequence,
                                          std::array<uint8_t, 32>& reply_buf) {
  const std::array<uint8_t, 8> value = Decrypt(buf, std::nullopt);
  const uint32_t conf = ReadBE32(value.data());
  const uint32_t next_random = GetNext();
  std::array<uint8_t, 8> value_to_encrypt{};
  WriteLE32(value_to_encrypt.data(), next_random);
  WriteBE32(value_to_encrypt.data() + 4, conf);
  const std::array<uint8_t, 8> encrypted = Encrypt(value_to_encrypt.data(), std::nullopt);
  reply_buf[0] = 0x55;
  reply_buf[1] = 0x09;
  reply_buf[2] = sequence;
  std::memcpy(&reply_buf[3], encrypted.data(), encrypted.size());
  reply_buf[11] = GenerateChecksum(reply_buf, 11);
}

void EmulatedToypad::QueryBlock(uint8_t index, uint8_t page, std::array<uint8_t, 32>& reply_buf,
                                uint8_t sequence) {
  std::lock_guard<std::mutex> guard(state_lock_);

  reply_buf[0] = 0x55;
  reply_buf[1] = 0x12;
  reply_buf[2] = sequence;
  reply_buf[3] = 0x00;

  // Index from game begins at 1 rather than 0.
  if (const uint8_t figure_index = index - 1; figure_index < kToypadFigureCount) {
    const ToypadFigure& figure = figures_[figure_index];
    if (figure.index != 255 && (4 * page) < (kToypadTagSize - 16)) {
      std::memcpy(&reply_buf[4], figure.data.data() + (4 * page), 16);
    }
  }
  reply_buf[20] = GenerateChecksum(reply_buf, 20);
}

void EmulatedToypad::WriteBlock(uint8_t index, uint8_t page, const uint8_t* to_write_buf,
                                std::array<uint8_t, 32>& reply_buf, uint8_t sequence) {
  std::lock_guard<std::mutex> guard(state_lock_);

  reply_buf[0] = 0x55;
  reply_buf[1] = 0x02;
  reply_buf[2] = sequence;
  reply_buf[3] = 0x00;

  if (const uint8_t figure_index = index - 1; figure_index < kToypadFigureCount) {
    ToypadFigure& figure = figures_[figure_index];
    if (figure.index != 255 && page < 0x2D) {
      // Id is written to page 36.
      if (page == 36) {
        figure.id = ReadLE32(to_write_buf);
      }
      std::memcpy(figure.data.data() + (page * 4), to_write_buf, 4);
      figure.Save();
    }
  }
  reply_buf[4] = GenerateChecksum(reply_buf, 4);
}

void EmulatedToypad::GetModel(const uint8_t* buf, uint8_t sequence,
                              std::array<uint8_t, 32>& reply_buf) {
  const std::array<uint8_t, 8> value = Decrypt(buf, std::nullopt);
  const uint8_t index = value[0];
  const uint32_t conf = ReadBE32(value.data() + 4);
  std::array<uint8_t, 8> value_to_encrypt{};
  if (const uint8_t figure_index = index - 1; figure_index < kToypadFigureCount) {
    std::lock_guard<std::mutex> guard(state_lock_);
    const ToypadFigure& figure = figures_[figure_index];
    WriteLE32(value_to_encrypt.data(), figure.id);
  }
  WriteBE32(value_to_encrypt.data() + 4, conf);
  const std::array<uint8_t, 8> encrypted = Encrypt(value_to_encrypt.data(), std::nullopt);
  reply_buf[0] = 0x55;
  reply_buf[1] = 0x0a;
  reply_buf[2] = sequence;
  reply_buf[3] = 0x00;
  std::memcpy(&reply_buf[4], encrypted.data(), encrypted.size());
  reply_buf[12] = GenerateChecksum(reply_buf, 12);
}

// ============================================================================
// Figure management (driven by the companion-app listener)
// ============================================================================

uint32_t EmulatedToypad::LoadFigure(const std::array<uint8_t, kToypadTagSize>& buf,
                                    std::string file_path, uint8_t pad, uint8_t index, bool lock) {
  if (lock) {
    state_lock_.lock();
  }

  const uint32_t id = GetFigureId(buf);

  ToypadFigure& figure = GetFigureByIndex(index);
  figure.file_path = std::move(file_path);
  figure.id = id;
  figure.pad = pad;
  figure.index = index + 1;
  std::memcpy(figure.data.data(), buf.data(), buf.size());

  // Notify the game: pad, index, direction (0x00 = added) and UID.
  std::array<uint8_t, 32> figure_change_response = {0x56,         0x0b,   figure.pad, 0x00,
                                                    figure.index, 0x00,   buf[0],     buf[1],
                                                    buf[2],       buf[4], buf[5],     buf[6],
                                                    buf[7]};
  figure_change_response[13] = GenerateChecksum(figure_change_response, 13);
  responses_.push(figure_change_response);

  if (lock) {
    state_lock_.unlock();
  }
  return id;
}

bool EmulatedToypad::RemoveFigure(uint8_t pad, uint8_t index, bool full_remove, bool lock) {
  ToypadFigure& figure = GetFigureByIndex(index);
  if (figure.index == 255) {
    return false;
  }

  if (lock) {
    state_lock_.lock();
  }

  // Notify the game: pad, index, direction (0x01 = removed) and UID.
  if (full_remove) {
    std::array<uint8_t, 32> figure_change_response = {
        0x56,           0x0b,           pad,            0x00,           figure.index,
        0x01,           figure.data[0], figure.data[1], figure.data[2], figure.data[4],
        figure.data[5], figure.data[6], figure.data[7]};
    figure_change_response[13] = GenerateChecksum(figure_change_response, 13);
    responses_.push(figure_change_response);
    figure.Save();
    figure.file_path.clear();
  }

  figure.index = 255;
  figure.pad = 255;
  figure.id = 0;

  if (lock) {
    state_lock_.unlock();
  }
  return true;
}

bool EmulatedToypad::TempRemove(uint8_t index) {
  std::lock_guard<std::mutex> guard(state_lock_);

  const ToypadFigure& figure = GetFigureByIndex(index);
  if (figure.index == 255) {
    return false;
  }

  // Report the figure as "picked up" until the move completes or cancels.
  std::array<uint8_t, 32> figure_change_response = {
      0x56,           0x0b,           figure.pad,     0x00,           figure.index,
      0x01,           figure.data[0], figure.data[1], figure.data[2], figure.data[4],
      figure.data[5], figure.data[6], figure.data[7]};
  figure_change_response[13] = GenerateChecksum(figure_change_response, 13);
  responses_.push(figure_change_response);
  return true;
}

bool EmulatedToypad::CancelRemove(uint8_t index) {
  std::lock_guard<std::mutex> guard(state_lock_);

  ToypadFigure& figure = GetFigureByIndex(index);
  if (figure.index == 255) {
    return false;
  }

  std::array<uint8_t, 32> figure_change_response = {
      0x56,           0x0b,           figure.pad,     0x00,           figure.index,
      0x00,           figure.data[0], figure.data[1], figure.data[2], figure.data[4],
      figure.data[5], figure.data[6], figure.data[7]};
  figure_change_response[13] = GenerateChecksum(figure_change_response, 13);
  responses_.push(figure_change_response);
  return true;
}

bool EmulatedToypad::MoveFigure(uint8_t pad, uint8_t index, uint8_t old_pad, uint8_t old_index) {
  if (old_index == index) {
    // Don't bother removing and loading again, just answer the game.
    CancelRemove(index);
    return true;
  }

  std::lock_guard<std::mutex> guard(state_lock_);

  // Clear the destination, remove from the source, load into destination.
  RemoveFigure(pad, index, true, false);

  ToypadFigure& figure = GetFigureByIndex(old_index);
  const std::array<uint8_t, kToypadTagSize> data = figure.data;
  std::string file_path = std::move(figure.file_path);

  RemoveFigure(old_pad, old_index, false, false);
  LoadFigure(data, std::move(file_path), pad, index, false);
  return true;
}

// ============================================================================
// Companion-app TCP listener (wire contract shared with the Cemu/RPCS3 forks)
// ============================================================================

// Keeping a connection open is for the colour poll and nothing else. A client
// that sends a placement command and then waits for the server to hang up -
// which is what the Windows companion app does - would otherwise sit there
// until its own timeout, and every place, move and remove would feel slow.
// So LOAD, REMOVE and MOVE close the connection the moment they are done,
// exactly as they did before, and only GET_LED stays on the line.
void EmulatedToypad::HandleClient(uintptr_t client_socket) {
  while (listener_running_.load() && HandleCommand(client_socket)) {
  }
  CloseSock(static_cast<socket_t>(client_socket));
  ForgetClient(client_socket);
  client_count_.fetch_sub(1);
}

void EmulatedToypad::ForgetClient(uintptr_t client_socket) {
  std::lock_guard<std::mutex> guard(clients_lock_);
  const auto it = std::find(client_sockets_.begin(), client_sockets_.end(), client_socket);
  if (it != client_sockets_.end()) {
    client_sockets_.erase(it);
  }
}

bool EmulatedToypad::HandleCommand(uintptr_t client_socket) {
  const socket_t client = static_cast<socket_t>(client_socket);

  uint8_t header[5];
  if (!RecvAll(client, header, sizeof(header))) {
    return false;
  }

  // Commands still run one at a time, in arrival order, exactly as they did
  // when the accept loop handled them inline.
  std::lock_guard<std::mutex> serialise(command_lock_);

  const uint8_t cmd = header[0];
  const uint8_t pad = header[1];
  const uint8_t index = header[2];

  // The colour query carries no pad or figure, so it has to be answered before
  // the checks the placement commands need. The companion app polls it many
  // times a second: failing it here left the app showing stale pad colours and
  // buried the log in rejections.
  if (cmd == 0x04) {
    const std::array<uint8_t, kLedSnapshotSize> snapshot = LedSnapshot();
    return SendAll(client, snapshot.data(), snapshot.size());
  }

  // A rejected command closes the connection: for LOAD its tag and path are
  // still on the wire, and there is no way to resynchronise a stream cleanly.
  if (pad < 1 || pad > 3 || index >= kToypadFigureCount) {
    REXLOG_WARN("Toypad listener: rejected message cmd={:02X} pad={} index={}", cmd, pad, index);
    return false;
  }

  switch (cmd) {
    case 0x01: {  // LOAD
      std::array<uint8_t, kToypadTagSize> tag{};
      if (!RecvAll(client, tag.data(), tag.size())) {
        return false;
      }

      uint8_t len_buf[2];
      if (!RecvAll(client, len_buf, sizeof(len_buf))) {
        return false;
      }
      const uint16_t path_len = static_cast<uint16_t>(len_buf[0] | (len_buf[1] << 8));

      std::string path;
      if (path_len != 0) {
        std::vector<uint8_t> path_buf(path_len);
        if (!RecvAll(client, path_buf.data(), path_len)) {
          return false;
        }
        path.assign(reinterpret_cast<const char*>(path_buf.data()), path_len);
      }

      // The listener contract is that LOAD silently overwrites an occupied
      // slot.
      RemoveFigure(pad, index, true, true);
      LoadFigure(tag, std::move(path), pad, index, true);
      REXLOG_INFO("Toypad listener: LOAD -> pad={} index={} (path len {})", pad, index, path_len);
      break;
    }
    case 0x02: {  // REMOVE
      RemoveFigure(pad, index, true, true);
      REXLOG_INFO("Toypad listener: REMOVE -> pad={} index={}", pad, index);
      break;
    }
    case 0x03: {  // MOVE
      const uint8_t old_pad = header[3];
      const uint8_t old_index = header[4];
      if (old_pad < 1 || old_pad > 3 || old_index >= kToypadFigureCount) {
        REXLOG_WARN("Toypad listener: rejected MOVE source pad={} index={}", old_pad, old_index);
        return false;
      }

      if (!TempRemove(old_index)) {
        REXLOG_WARN("Toypad listener: ignored MOVE from empty slot pad={} index={}", old_pad,
                    old_index);
        return false;
      }

      std::this_thread::sleep_for(kMovePickupDelay);
      MoveFigure(pad, index, old_pad, old_index);
      REXLOG_INFO("Toypad listener: MOVE {}/{} -> {}/{}", old_pad, old_index, pad, index);
      break;
    }
    default: {
      REXLOG_WARN("Toypad listener: unknown command {:02X}", cmd);
      break;
    }
  }

  // A placement command is finished business: hang up, so a client waiting on
  // end-of-stream gets it immediately.
  return false;
}

void EmulatedToypad::ListenerRun(uint16_t port) {
  const socket_t listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_sock == kInvalidSocket) {
    REXLOG_WARN("Toypad listener: could not create socket");
    return;
  }

  int reuse = 1;
  ::setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(listen_sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(listen_sock, 16) != 0) {
    REXLOG_WARN("Toypad listener: could not bind/listen on 127.0.0.1:{}", port);
    CloseSock(listen_sock);
    return;
  }

  listen_socket_.store(static_cast<uintptr_t>(listen_sock));
  REXLOG_INFO("Toypad listener active on 127.0.0.1:{}", port);

  while (listener_running_.load()) {
    const socket_t client = ::accept(listen_sock, nullptr, nullptr);
    if (client == kInvalidSocket) {
      break;  // socket closed by the destructor
    }

    // A client that connects and then says nothing used to wedge the accept
    // loop forever, because the first recv had no deadline.
    SetRecvTimeout(client, kClientIdleTimeoutMs);

    const uintptr_t handle = static_cast<uintptr_t>(client);
    {
      std::lock_guard<std::mutex> guard(clients_lock_);
      client_sockets_.push_back(handle);
    }
    client_count_.fetch_add(1);
    // Detached: a connection lives as long as the app keeps it open, which is
    // not something the accept loop can wait on. Shutdown closes the sockets
    // below and waits for the count to drain instead.
    std::thread(&EmulatedToypad::HandleClient, this, handle).detach();
  }

  // Drop every open connection, then wait for the threads serving them.
  {
    std::lock_guard<std::mutex> guard(clients_lock_);
    for (const uintptr_t handle : client_sockets_) {
      CloseSock(static_cast<socket_t>(handle));
    }
  }
  for (int spins = 0; client_count_.load() != 0 && spins < 200; ++spins) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void EmulatedToypad::PickerWatchRun() {
#ifdef _WIN32
  HANDLE event_handle = nullptr;
  uint64_t last_open_ms = 0;
  bool active_prev = false;

  while (listener_running_.load()) {
    // Re-open the handle periodically: if the companion app restarts, it
    // creates a fresh event object under the same name and a stale handle
    // would keep watching the dead one.
    const uint64_t now_ms = GetTickCount64();
    if (!event_handle || now_ms - last_open_ms >= 1000) {
      if (event_handle) {
        CloseHandle(event_handle);
      }
      event_handle = OpenEventW(SYNCHRONIZE, FALSE, kPickerEventName);
      last_open_ms = now_ms;
    }

    const bool active = event_handle && WaitForSingleObject(event_handle, 0) == WAIT_OBJECT_0;
    if (active != active_prev) {
      s_picker_active.store(active);
      REXLOG_INFO("Toypad: picker overlay {} - game input {}", active ? "opened" : "closed",
                  active ? "blocked" : "released");
      active_prev = active;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  s_picker_active.store(false);
  if (event_handle) {
    CloseHandle(event_handle);
  }
#endif
}

}  // namespace rex::input
