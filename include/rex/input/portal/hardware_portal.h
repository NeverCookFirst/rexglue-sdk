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

#include <array>

#include <rex/input/portal/portal.h>

struct libusb_context;
struct libusb_device_handle;

namespace rex::input {

struct PortalDeviceId {
  uint16_t vendor_id;
  uint16_t product_id;
  const char* name;
  // True when the portal's own firmware speaks the wrapped Xbox 360 frame
  // (<prefix> <len> 55 ...) rather than a bare 0x55 one, so the wrapper the
  // guest sends must be forwarded untouched instead of stripped.
  bool speaks_xbox_frame;
};

// PDP shipped the ToyPad under two different identities, and they are not the
// same device to the host. 0E6F:0241 ("LEGO READER V2.10") is the PC/PS3/Wii U
// pad and enumerates as plain HID. The Xbox 360 pad is 24C6:FA01, an XUSB
// peripheral (class FF, subclass 5D, protocol 01) that Windows has no inbox
// driver for at all - Device Manager shows "Driver is unavailable" until a
// libusb-compatible driver is installed over it with Zadig.
inline constexpr std::array<PortalDeviceId, 4> kPortalVendorProductIdList = {
    PortalDeviceId{0x0E6F, 0x0241, "LEGO Dimensions ToyPad", false},
    PortalDeviceId{0x24C6, 0xFA01, "LEGO Dimensions ToyPad (Xbox 360)", true},
    PortalDeviceId{0x1430, 0x1F17, "Skylanders Portal of Power", false},
    // Also an Xbox 360 peripheral, so it gets the same frame policy. Untested.
    PortalDeviceId{0x24C6, 0xFA00, "Disney Infinity Base", true}};

// Passthrough to a physical toy portal over USB. Reads and writes go straight
// to the device's interrupt endpoints, so the game talks to real hardware and
// none of the emulated protocol in EmulatedToypad is involved.
class HardwarePortal final : public Portal {
 public:
  HardwarePortal();
  ~HardwarePortal() override;

  bool IsConnected() override;

  void OnDeviceArrival() override;
  void OnDeviceRemoval() override;

  // Name of the portal currently open, or nullptr when none is. Used by the
  // settings UI to show what was found.
  const char* device_name() const { return device_name_; }

 private:
  void OpenDevice() override;
  void CloseDevice() override;
  X_STATUS ReadInternal(std::span<uint8_t> data, int32_t& read_count) override;
  X_STATUS WriteInternal(std::span<uint8_t> data) override;

  // Endpoint addresses differ per portal (the Skylanders one answers on
  // 0x81/0x02, the LEGO ToyPad does not), so they are read out of the device's
  // own descriptor in OpenDevice rather than assumed. A wrong guess shows up as
  // LIBUSB_ERROR_NOT_FOUND on every transfer.
  bool FindEndpoints(libusb_device_handle* handle);

  static constexpr uint16_t kTimeoutMs = 100;

  uint8_t read_endpoint_ = 0;
  uint8_t write_endpoint_ = 0;
  int interface_number_ = 0;

  // The guest hands the portal a wrapped command frame: <prefix> <length> 55
  // ..., the same shape EmulatedToypad detects. A PC/PS3 pad speaks the
  // unwrapped 0x55 frame, so for those the wrapper is stripped on the way out
  // and put back on the way in; an Xbox 360 pad speaks it natively and wants it
  // kept. -1 = no write seen yet, 0 = the guest sends bare frames already.
  int frame_offset_ = -1;
  uint8_t frame_prefix_byte_ = 0;

  // Whether to forward the wrapper to the device, resolved in OpenDevice from
  // the device entry and the toypad_passthrough_frame override.
  bool keep_wrapper_ = false;

  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  const char* device_name_ = nullptr;
};

}  // namespace rex::input
