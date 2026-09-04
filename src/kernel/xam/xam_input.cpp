/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/input/portal/emulated_toypad.h>
#include <rex/input/portal/portal.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

constexpr uint32_t XINPUT_FLAG_GAMEPAD = 0x01;
constexpr uint32_t XINPUT_FLAG_ANY_USER = 1 << 30;

rex::input::InputSystem* input_system() {
  return static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
}

void XamResetInactivity_entry() {
  // Do we need to do anything?
}

u32 XamEnableInactivityProcessing_entry(u32 unk, u32 enable) {
  return X_ERROR_SUCCESS;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetcapabilities(v=vs.85).aspx
u32 XamInputGetCapabilities_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  REXKRNL_TRACE("[XAM] XamInputGetCapabilities called: user={}, flags=0x{:X}", (uint32_t)user_index,
                (uint32_t)flags);
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}

u32 XamInputGetCapabilitiesEx_entry(u32 unk, u32 user_index, u32 flags,
                                    ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetstate(v=vs.85).aspx
u32 XamInputGetState_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_STATE> input_state) {
  // Games call this with a NULL state ptr, probably as a query.
  static int call_count = 0;
  if (++call_count <= 5) {
    REXKRNL_TRACE("[XAM] XamInputGetState called: user={}, flags=0x{:X}", (uint32_t)user_index,
                  (uint32_t)flags);
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  // While a toypad picker owns the pad, the game is told nothing is pressed. This
  // is what puts the picker first: the button that opens it, and every button
  // while it is open, acts only there and never reaches the game as well.
  if (rex::input::EmulatedToypad::IsPickerInputActive()) {
    if (input_state) {
      std::memset(input_state.host_address(), 0, sizeof(X_INPUT_STATE));
    }
    return X_ERROR_SUCCESS;
  }

  auto* is = input_system();
  return is->GetState(actual_user_index, input_state);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputsetstate(v=vs.85).aspx
u32 XamInputSetState_entry(u32 user_index, u32 unk, ppc_ptr_t<X_INPUT_VIBRATION> vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  uint32_t actual_user_index = user_index;
  if ((user_index & 0xFF) == 0xFF) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->SetState(actual_user_index, vibration);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetkeystroke(v=vs.85).aspx
u32 XamInputGetKeystroke_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  // https://github.com/CodeAsm/ffplay360/blob/master/Common/AtgXime.cpp
  // user index = index or XUSER_INDEX_ANY
  // flags = XINPUT_FLAG_GAMEPAD (| _ANYUSER | _ANYDEVICE)

  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  return is->GetKeystroke(actual_user_index, flags, keystroke);
}

// Same as non-ex, just takes a pointer to user index.
u32 XamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, u32 flags,
                                 ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t user_index = *user_index_ptr;
  if ((user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    user_index = 0;
  }

  auto* is = input_system();
  auto result = is->GetKeystroke(user_index, flags, keystroke);
  if (XSUCCEEDED(result)) {
    *user_index_ptr = keystroke->user_index;
  }
  return result;
}

i32 XamUserGetDeviceContext_entry(u32 user_index, u32 unk, mapped_u32 out_ptr) {
  // Games check the result - usually with some masking.
  // If this function fails they assume zero, so let's fail AND
  // set zero just to be safe.
  *out_ptr = 0;
  if (!user_index || (user_index & 0xFF) == 0xFF) {
    return X_E_SUCCESS;
  } else {
    return X_E_DEVICE_NOT_CONNECTED;
  }
}

// ============================================================================
// Non-controller raw HID (the emulated LEGO Dimensions ToyPad).
// Ported from the Xenia-Seamless-Toypad-Build fork; the device ids and the
// X_ERROR_* return values are kept byte-for-byte identical to that build,
// which is known to work in-game.
// ============================================================================

u32 XamInputNonControllerGetRawEx_entry(u32 device_id, mapped_u8 buffer_ptr,
                                        mapped_u32 buffer_length_ptr, mapped_u16 state_ptr) {
  if (device_id != 5 && device_id != 6) {
    REXKRNL_WARN("XamInputNonControllerGetRawEx: rejected device_id={}", device_id);
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!state_ptr || !buffer_length_ptr || !buffer_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const uint32_t requested = *buffer_length_ptr;
  if (requested == 0 || requested > rex::input::kPortalBufferSize) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto* portal = input_system()->GetPortal();
  if (!portal) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t bytes_read = requested;
  uint16_t state = 0;

  const auto result = portal->Read({buffer_ptr.host_address(), requested}, bytes_read, state);

  if (XSUCCEEDED(result)) {
    *buffer_length_ptr = bytes_read;
    *state_ptr = state;
  }
  return result;
}

u32 XamInputNonControllerSetRawEx_entry(u32 device_id, mapped_u8 buffer_ptr, u32 buffer_length) {
  if (device_id != 5 && device_id != 6) {
    REXKRNL_WARN("XamInputNonControllerSetRawEx: rejected device_id={}", device_id);
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!buffer_ptr || !buffer_length || buffer_length > rex::input::kPortalBufferSize) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto* portal = input_system()->GetPortal();
  if (!portal) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  return portal->Write({buffer_ptr.host_address(), buffer_length});
}

u32 XamInputNonControllerGetRaw_entry(mapped_u16 state_ptr, mapped_u32 buffer_length_ptr,
                                      mapped_u8 buffer_ptr) {
  return XamInputNonControllerGetRawEx_entry(5, buffer_ptr, buffer_length_ptr, state_ptr);
}

u32 XamInputNonControllerSetRaw_entry(u32 buffer_length, mapped_u8 buffer_ptr) {
  // Normally these are handled separately with a different first param, but
  // whatever.
  return XamInputNonControllerSetRawEx_entry(5, buffer_ptr, buffer_length);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamResetInactivity, rex::kernel::xam::XamResetInactivity_entry)
REX_EXPORT(__imp__XamEnableInactivityProcessing,
           rex::kernel::xam::XamEnableInactivityProcessing_entry)
REX_EXPORT(__imp__XamInputGetCapabilities, rex::kernel::xam::XamInputGetCapabilities_entry)
REX_EXPORT(__imp__XamInputGetCapabilitiesEx, rex::kernel::xam::XamInputGetCapabilitiesEx_entry)
REX_EXPORT(__imp__XamInputGetState, rex::kernel::xam::XamInputGetState_entry)
REX_EXPORT(__imp__XamInputSetState, rex::kernel::xam::XamInputSetState_entry)
REX_EXPORT(__imp__XamInputGetKeystroke, rex::kernel::xam::XamInputGetKeystroke_entry)
REX_EXPORT(__imp__XamInputGetKeystrokeEx, rex::kernel::xam::XamInputGetKeystrokeEx_entry)
REX_EXPORT(__imp__XamUserGetDeviceContext, rex::kernel::xam::XamUserGetDeviceContext_entry)

REX_EXPORT_STUB(__imp__XamInputControl);
REX_EXPORT_STUB(__imp__XamInputEnableAutobind);
REX_EXPORT_STUB(__imp__XamInputGetDeviceStats);
REX_EXPORT_STUB(__imp__XamInputGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XamInputGetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHud);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHudEx);
REX_EXPORT_STUB(__imp__XamInputGetUserVibrationLevel);
REX_EXPORT(__imp__XamInputNonControllerGetRaw,
           rex::kernel::xam::XamInputNonControllerGetRaw_entry)
REX_EXPORT(__imp__XamInputNonControllerGetRawEx,
           rex::kernel::xam::XamInputNonControllerGetRawEx_entry)
REX_EXPORT(__imp__XamInputNonControllerSetRaw,
           rex::kernel::xam::XamInputNonControllerSetRaw_entry)
REX_EXPORT(__imp__XamInputNonControllerSetRawEx,
           rex::kernel::xam::XamInputNonControllerSetRawEx_entry)
REX_EXPORT_STUB(__imp__XamInputRawState);
REX_EXPORT_STUB(__imp__XamInputResetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSendStayAliveRequest);
REX_EXPORT_STUB(__imp__XamInputSendXenonButtonPress);
REX_EXPORT_STUB(__imp__XamInputSetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputSetKeyboardTranslationHud);
REX_EXPORT_STUB(__imp__XamInputSetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSetMinMaxAuthDelay);
REX_EXPORT_STUB(__imp__XamInputSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XamInputToggleKeyLocks);
