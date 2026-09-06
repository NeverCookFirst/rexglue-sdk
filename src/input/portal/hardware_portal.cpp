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

#include <rex/input/portal/hardware_portal.h>

#include <libusb.h>

#include <rex/logging.h>

namespace rex::input {

HardwarePortal::HardwarePortal() : Portal() {
  const int result = libusb_init(&context_);
  if (result != LIBUSB_SUCCESS) {
    REXLOG_ERROR("Portal: libusb_init failed: {}", libusb_error_name(result));
    context_ = nullptr;
    return;
  }

  OpenDevice();
}

HardwarePortal::~HardwarePortal() {
  CloseDevice();

  if (context_) {
    libusb_exit(context_);
    context_ = nullptr;
  }
}

bool HardwarePortal::IsConnected() { return handle_ != nullptr; }

void HardwarePortal::OpenDevice() {
  if (!context_ || handle_) {
    return;
  }

  // Allow only one portal device at a time.
  for (const auto& entry : kPortalVendorProductIdList) {
    libusb_device_handle* handle = libusb_open_device_with_vid_pid(
        context_, entry.vendor_id, entry.product_id);
    if (!handle) {
      continue;
    }

    // No-op on Windows, but keeps the device usable if this is ever built for
    // a platform with a kernel HID driver bound to the portal.
    libusb_set_auto_detach_kernel_driver(handle, 1);

    const int claim_result = libusb_claim_interface(handle, 0);
    if (claim_result != LIBUSB_SUCCESS) {
      // Almost always the stock HID driver still owning the interface; the
      // fix is installing libusb/WinUSB over it with Zadig.
      REXLOG_ERROR(
          "Portal: found {} ({:04X}:{:04X}) but could not claim it: {}. "
          "Install the libusb driver for it with Zadig.",
          entry.name, entry.vendor_id, entry.product_id,
          libusb_error_name(claim_result));
      libusb_close(handle);
      continue;
    }

    handle_ = handle;
    device_name_ = entry.name;
    REXLOG_INFO("Portal: using {} ({:04X}:{:04X}) over USB.", entry.name,
                entry.vendor_id, entry.product_id);
    return;
  }

  REXLOG_WARN(
      "Portal: no supported portal found over USB. Plug one in, or set "
      "toypad_emulation = true to use the emulated ToyPad instead.");
}

void HardwarePortal::CloseDevice() {
  if (!handle_) {
    return;
  }

  libusb_release_interface(handle_, 0);
  libusb_close(handle_);
  handle_ = nullptr;
  device_name_ = nullptr;
}

X_STATUS HardwarePortal::ReadInternal(std::span<uint8_t> data,
                                      int32_t& read_count) {
  if (!handle_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const int result = libusb_interrupt_transfer(
      handle_, kReadEndpoint, data.data(), static_cast<int>(data.size()),
      &read_count, kTimeoutMs);

  switch (result) {
    case LIBUSB_ERROR_NO_DEVICE:
      // Drop the dead handle, otherwise OpenDevice() would see a non-null
      // handle_ and refuse to reconnect when the portal is plugged back in.
      CloseDevice();
      return X_ERROR_DEVICE_NOT_CONNECTED;
    case LIBUSB_ERROR_TIMEOUT:
      // Nothing queued on the portal; an empty read is the normal idle case,
      // not a failure.
      return X_ERROR_SUCCESS;
    default:
      break;
  }

  if (result < 0) {
    REXLOG_WARN("Portal[Read] returned error: {}", libusb_error_name(result));
    return X_ERROR_FUNCTION_FAILED;
  }

  return X_ERROR_SUCCESS;
}

X_STATUS HardwarePortal::WriteInternal(std::span<uint8_t> data) {
  if (!handle_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const int result = libusb_interrupt_transfer(
      handle_, kWriteEndpoint, data.data(), static_cast<int>(data.size()),
      nullptr, kTimeoutMs);

  if (result == LIBUSB_ERROR_NO_DEVICE) {
    CloseDevice();
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (result < 0) {
    REXLOG_WARN("Portal[Write] returned error: {}", libusb_error_name(result));
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  return X_ERROR_SUCCESS;
}

void HardwarePortal::OnDeviceArrival() { OpenDevice(); }

void HardwarePortal::OnDeviceRemoval() { CloseDevice(); }

}  // namespace rex::input
