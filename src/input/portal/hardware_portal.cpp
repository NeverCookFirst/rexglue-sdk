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

#include <algorithm>
#include <string_view>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(toypad_passthrough_frame, "auto", "Input",
                      "How to forward command frames to a physical portal: auto picks by "
                      "device (keep the Xbox 360 wrapper, strip it for a PC/PS3 pad), "
                      "keep and strip force it")
    .allowed({"auto", "keep", "strip"});

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

bool HardwarePortal::FindEndpoints(libusb_device_handle* handle) {
  read_endpoint_ = 0;
  write_endpoint_ = 0;
  interface_number_ = 0;

  libusb_config_descriptor* config = nullptr;
  if (libusb_get_active_config_descriptor(libusb_get_device(handle), &config) !=
      LIBUSB_SUCCESS) {
    return false;
  }

  for (uint8_t i = 0; i < config->bNumInterfaces && !read_endpoint_; i++) {
    const libusb_interface& iface = config->interface[i];
    for (int a = 0; a < iface.num_altsetting; a++) {
      const libusb_interface_descriptor& alt = iface.altsetting[a];
      uint8_t in = 0, out = 0;
      for (uint8_t e = 0; e < alt.bNumEndpoints; e++) {
        const libusb_endpoint_descriptor& ep = alt.endpoint[e];
        if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
            LIBUSB_TRANSFER_TYPE_INTERRUPT) {
          continue;
        }
        if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
          if (!in) in = ep.bEndpointAddress;
        } else if (!out) {
          out = ep.bEndpointAddress;
        }
      }
      if (in && out) {
        read_endpoint_ = in;
        write_endpoint_ = out;
        interface_number_ = alt.bInterfaceNumber;
        break;
      }
    }
  }

  libusb_free_config_descriptor(config);
  if (!read_endpoint_) {
    return false;
  }

  REXLOG_INFO("Portal: interface {}, interrupt endpoints in 0x{:02X} out 0x{:02X}",
              interface_number_, read_endpoint_, write_endpoint_);
  return true;
}

uint8_t HardwarePortal::NextGipSequence() {
  const uint8_t sequence = gip_sequence_;
  gip_sequence_ =
      gip_sequence_ == 255 ? 1 : static_cast<uint8_t>(gip_sequence_ + 1);
  return sequence;
}

bool HardwarePortal::OpenGipGateway() {
  // 55 0F B0 01 "(c) LEGO 2014" F7 - the same wake frame the PC pad takes, but
  // here it has to travel inside GIP.
  static constexpr uint8_t kLegoWake[] = {0x55, 0x0F, 0xB0, 0x01, 0x28, 0x63,
                                          0x29, 0x20, 0x4C, 0x45, 0x47, 0x4F,
                                          0x20, 0x32, 0x30, 0x31, 0x34, 0xF7};

  std::array<uint8_t, kGipHeaderSize + kPortalBufferSize> wake{};
  wake[0] = 0x21;
  wake[1] = 0x00;
  wake[2] = NextGipSequence();
  wake[3] = kPortalBufferSize;
  std::memcpy(wake.data() + kGipHeaderSize, kLegoWake, sizeof(kLegoWake));

  int result =
      libusb_interrupt_transfer(handle_, write_endpoint_, wake.data(),
                                static_cast<int>(wake.size()), nullptr, kTimeoutMs);
  if (result < 0) {
    REXLOG_ERROR("Portal: GIP wake failed: {}", libusb_error_name(result));
    return false;
  }

  // The wake on its own draws no answer. The pad holds it back until the host
  // declares authentication complete, and that packet is what opens the
  // gateway - no real crypto handshake is needed.
  uint8_t auth[] = {0x06, 0x20, NextGipSequence(), 0x02, 0x01, 0x00};
  result = libusb_interrupt_transfer(handle_, write_endpoint_, auth,
                                     static_cast<int>(sizeof(auth)), nullptr,
                                     kTimeoutMs);
  if (result < 0) {
    REXLOG_ERROR("Portal: GIP authenticate failed: {}", libusb_error_name(result));
    return false;
  }

  // The reply lands about 10 ms later. Anything ahead of it is the ANNOUNCE the
  // pad repeats twice a second for as long as it has no host.
  std::array<uint8_t, 64> frame{};
  for (int attempt = 0; attempt < 20; attempt++) {
    int read_count = 0;
    result = libusb_interrupt_transfer(handle_, read_endpoint_, frame.data(),
                                       static_cast<int>(frame.size()),
                                       &read_count, kTimeoutMs);
    if (result == LIBUSB_ERROR_TIMEOUT) {
      continue;
    }
    if (result < 0) {
      REXLOG_ERROR("Portal: GIP handshake read failed: {}",
                   libusb_error_name(result));
      return false;
    }
    if (read_count >= static_cast<int>(kGipHeaderSize) && frame[0] == 0x21) {
      REXLOG_INFO("Portal: GIP gateway open.");
      return true;
    }
  }

  REXLOG_ERROR(
      "Portal: the Xbox One pad did not answer the GIP handshake. It goes mute "
      "after a finished session and after a stray IDENTIFY/POWER packet; "
      "unplug it and plug it back in.");
  return false;
}

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

    if (!FindEndpoints(handle)) {
      REXLOG_ERROR("Portal: {} ({:04X}:{:04X}) exposes no interrupt endpoint pair.",
                   entry.name, entry.vendor_id, entry.product_id);
      libusb_close(handle);
      continue;
    }

    const int claim_result = libusb_claim_interface(handle, interface_number_);
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

    const std::string_view policy = REXCVAR_GET(toypad_passthrough_frame);
    keep_wrapper_ = policy == "keep"   ? true
                    : policy == "strip" ? false
                                        : entry.speaks_xbox_frame;
    REXLOG_INFO("Portal: forwarding frames with the Xbox wrapper {} ({}).",
                keep_wrapper_ ? "kept" : "stripped", policy);

    speaks_gip_ = entry.speaks_gip;
    gip_sequence_ = 1;
    if (speaks_gip_ && !OpenGipGateway()) {
      CloseDevice();
      continue;
    }

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

  libusb_release_interface(handle_, interface_number_);
  libusb_close(handle_);
  handle_ = nullptr;
  device_name_ = nullptr;
  read_endpoint_ = 0;
  write_endpoint_ = 0;
  speaks_gip_ = false;
  gip_sequence_ = 1;
}

X_STATUS HardwarePortal::ReadInternal(std::span<uint8_t> data,
                                      int32_t& read_count) {
  if (!handle_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::array<uint8_t, kPortalBufferSize> frame{};
  // A GIP frame is four bytes longer than the LEGO frame it carries, so reading
  // straight into a 32-byte buffer would overflow it. Take a whole packet and
  // unwrap below.
  std::array<uint8_t, 64> gip_frame{};
  const int result = libusb_interrupt_transfer(
      handle_, read_endpoint_, speaks_gip_ ? gip_frame.data() : frame.data(),
      static_cast<int>(speaks_gip_ ? gip_frame.size() : frame.size()),
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

  if (speaks_gip_) {
    // Only gateway frames carry LEGO payloads. ANNOUNCE and any other GIP
    // chatter has to be swallowed here, or the guest would read it as a reply.
    if (read_count < static_cast<int>(kGipHeaderSize) || gip_frame[0] != 0x21) {
      read_count = 0;
      return X_ERROR_SUCCESS;
    }
    std::memcpy(frame.data(), gip_frame.data() + kGipHeaderSize,
                std::min(static_cast<size_t>(read_count) - kGipHeaderSize,
                         frame.size()));
  }

  // Hand the guest the frame back in the shape it writes in. A device that
  // speaks the wrapper already answers in that shape, so only rebuild it when
  // it was stripped on the way out. Length is the header's payload count plus
  // the type, length and checksum bytes.
  std::array<uint8_t, kPortalBufferSize> out{};
  if (frame_offset_ > 0 && !keep_wrapper_) {
    const size_t len = std::min<size_t>(size_t(frame[1]) + 3, out.size());
    out[0] = frame_prefix_byte_;
    out[1] = static_cast<uint8_t>(len);
    std::memcpy(&out[2], frame.data(), std::min(len, out.size() - 2));
  } else {
    out = frame;
  }

  const size_t count = std::min(out.size(), data.size());
  std::memcpy(data.data(), out.data(), count);
  read_count = static_cast<int32_t>(count);
  return X_ERROR_SUCCESS;
}

X_STATUS HardwarePortal::WriteInternal(std::span<uint8_t> data) {
  if (!handle_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Find the 0x55 command frame. The guest prepends a short wrapper
  // (0x0B <len> ...) that a PC/PS3 portal knows nothing about, so send each
  // device what it speaks and remember the wrapper for the reply.
  size_t offset = 0;
  const size_t scan_limit = std::min<size_t>(4, data.size());
  for (; offset < scan_limit; offset++) {
    if (data[offset] == 0x55) {
      break;
    }
  }
  if (offset == scan_limit) {
    REXLOG_WARN("Portal[Write] no 0x55 frame found; dropping it");
    return X_ERROR_SUCCESS;
  }
  if (frame_offset_ != static_cast<int>(offset)) {
    frame_offset_ = static_cast<int>(offset);
    frame_prefix_byte_ = offset > 0 ? data[0] : 0;
    REXLOG_INFO("Portal: guest frame offset {} (prefix {:02X})", offset,
                frame_prefix_byte_);
  }

  if (speaks_gip_) {
    // The Xbox One pad takes nothing but GIP: the guest wrapper always comes
    // off, and the bare LEGO frame goes out inside 21 00 <seq> 20, zero padded
    // to the full 32 bytes the pad expects.
    std::array<uint8_t, kGipHeaderSize + kPortalBufferSize> gip{};
    gip[0] = 0x21;
    gip[1] = 0x00;
    gip[2] = NextGipSequence();
    gip[3] = kPortalBufferSize;
    std::memcpy(gip.data() + kGipHeaderSize, data.data() + offset,
                std::min(data.size() - offset,
                         static_cast<size_t>(kPortalBufferSize)));

    const int gip_result = libusb_interrupt_transfer(
        handle_, write_endpoint_, gip.data(), static_cast<int>(gip.size()),
        nullptr, kTimeoutMs);
    if (gip_result == LIBUSB_ERROR_NO_DEVICE) {
      CloseDevice();
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (gip_result < 0) {
      REXLOG_WARN("Portal[Write] returned error: {}",
                  libusb_error_name(gip_result));
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    return X_ERROR_SUCCESS;
  }

  const size_t send_from = keep_wrapper_ ? 0 : offset;
  std::array<uint8_t, kPortalBufferSize> frame{};
  std::memcpy(frame.data(), data.data() + send_from,
              std::min(data.size() - send_from, frame.size()));

  const int result = libusb_interrupt_transfer(
      handle_, write_endpoint_, frame.data(), static_cast<int>(frame.size()),
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
