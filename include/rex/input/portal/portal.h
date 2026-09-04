/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Ported from the Xenia-Seamless-Toypad-Build fork, 2026 -
 *              adapted for the ReXGlue runtime.
 */

#pragma once

#include <mutex>
#include <span>

#include <rex/system/xtypes.h>

namespace rex::input {

constexpr uint8_t kPortalBufferSize = 0x20;

// Base class for a "non-controller" raw HID device reachable through
// XamInputNonControllerGetRaw/SetRaw. Read/Write hold the device lock and
// track the previous status so the guest sees a state-change flag.
class Portal {
 public:
  Portal();
  virtual ~Portal();

  virtual bool IsConnected() = 0;

  virtual X_STATUS Read(std::span<uint8_t> data, uint32_t& bytes_read, uint16_t& state);
  virtual X_STATUS Write(std::span<uint8_t> data);

  virtual void OnDeviceArrival() = 0;
  virtual void OnDeviceRemoval() = 0;

 private:
  virtual void OpenDevice() = 0;
  virtual void CloseDevice() = 0;

  virtual X_STATUS ReadInternal(std::span<uint8_t> data, int32_t& read_count) = 0;
  virtual X_STATUS WriteInternal(std::span<uint8_t> data) = 0;

  std::mutex lock_;
  X_STATUS previous_status_ = 0;
};

}  // namespace rex::input
