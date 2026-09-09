/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/exception_handler.h>

#if REX_PLATFORM_WIN32

#include "platform_win.h"

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>

#include <cstdio>

namespace rex::arch {

// Handle of the added VectoredExceptionHandler.
void* veh_handle_ = nullptr;
// Handle of the added VectoredContinueHandler.
void* vch_handle_ = nullptr;

// This can be as large as needed, but isn't often needed.
// As we will be sometimes firing many exceptions we want to avoid having to
// scan the table too much or invoke many custom handlers.
constexpr size_t kMaxHandlerCount = 8;

// All custom handlers, left-aligned and null terminated.
// Executed in order.
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

LONG CALLBACK ExceptionHandlerCallback(PEXCEPTION_POINTERS ex_info) {
  // Visual Studio SetThreadName.
  if (ex_info->ExceptionRecord->ExceptionCode == 0x406D1388) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  HostThreadContext thread_context;
  thread_context.rip = ex_info->ContextRecord->Rip;
  thread_context.eflags = ex_info->ContextRecord->EFlags;
  std::memcpy(thread_context.int_registers, &ex_info->ContextRecord->Rax,
              sizeof(thread_context.int_registers));
  std::memcpy(thread_context.xmm_registers, &ex_info->ContextRecord->Xmm0,
              sizeof(thread_context.xmm_registers));

  // https://msdn.microsoft.com/en-us/library/ms679331(v=vs.85).aspx
  // https://msdn.microsoft.com/en-us/library/aa363082(v=vs.85).aspx
  Exception ex;
  switch (ex_info->ExceptionRecord->ExceptionCode) {
    case STATUS_ILLEGAL_INSTRUCTION:
      ex.InitializeIllegalInstruction(&thread_context);
      break;
    case STATUS_ACCESS_VIOLATION: {
      Exception::AccessViolationOperation access_violation_operation;
      switch (ex_info->ExceptionRecord->ExceptionInformation[0]) {
        case 0:
          access_violation_operation = Exception::AccessViolationOperation::kRead;
          break;
        case 1:
          access_violation_operation = Exception::AccessViolationOperation::kWrite;
          break;
        default:
          access_violation_operation = Exception::AccessViolationOperation::kUnknown;
          break;
      }
      ex.InitializeAccessViolation(&thread_context,
                                   ex_info->ExceptionRecord->ExceptionInformation[1],
                                   access_violation_operation);
    } break;
    default:
      // Unknown/unhandled type.
      return EXCEPTION_CONTINUE_SEARCH;
  }

  for (size_t i = 0; i < rex::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      // Exception handled.
      ex_info->ContextRecord->Rip = thread_context.rip;
      ex_info->ContextRecord->EFlags = thread_context.eflags;
      uint32_t modified_register_index;
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (rex::bit_scan_forward(modified_int_registers_remaining, &modified_register_index)) {
        modified_int_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        (&ex_info->ContextRecord->Rax)[modified_register_index] =
            thread_context.int_registers[modified_register_index];
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(&ex_info->ContextRecord->Xmm0 + modified_register_index,
                    &thread_context.xmm_registers[modified_register_index], sizeof(vec128_t));
      }
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

namespace {

// Windows kills the process the moment an exception reaches the end of the
// handler chain, and it does so without touching our log - which is why a
// crash inside recompiled game code leaves game.log ending mid-sentence, with
// no clue as to where it died. This filter is the last thing to run before
// that happens: it writes the exception and the faulting address to the log,
// flushes, and drops a minidump next to the executable so the stack can be
// read afterwards in a debugger.
LONG WINAPI LastChanceExceptionFilter(PEXCEPTION_POINTERS ex_info) {
  const EXCEPTION_RECORD* record = ex_info->ExceptionRecord;
  REXLOG_CRITICAL("[FATAL] Unhandled exception 0x{:08X} at host 0x{:016X}",
                  static_cast<uint32_t>(record->ExceptionCode),
                  reinterpret_cast<uint64_t>(record->ExceptionAddress));
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2) {
    REXLOG_CRITICAL("[FATAL] Access violation: {} of 0x{:016X}",
                    record->ExceptionInformation[0] ? "write" : "read",
                    static_cast<uint64_t>(record->ExceptionInformation[1]));
  }
  if (auto logger = ::rex::GetLogger()) {
    logger->flush();
  }

  // dbghelp is loaded on demand so the runtime keeps no link-time dependency
  // on it; if it is missing, the log lines above are still written.
  using PFNMiniDumpWriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int,
                                             void*, void*, void*);
  if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
    auto write_dump = reinterpret_cast<PFNMiniDumpWriteDump>(
        GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (write_dump) {
      wchar_t path[MAX_PATH];
      _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"crash-%lu.dmp",
                   GetCurrentProcessId());
      HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file != INVALID_HANDLE_VALUE) {
        struct {
          DWORD thread_id;
          PEXCEPTION_POINTERS exception_pointers;
          BOOL client_pointers;
        } dump_info = {GetCurrentThreadId(), ex_info, FALSE};
        // 2 = MiniDumpWithFullMemory: large, but a stack alone rarely explains
        // a crash in recompiled code.
        write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, 2,
                   &dump_info, nullptr, nullptr);
        CloseHandle(file);
        REXLOG_CRITICAL("[FATAL] Minidump written to crash-{}.dmp",
                        static_cast<uint32_t>(GetCurrentProcessId()));
        if (auto logger = ::rex::GetLogger()) {
          logger->flush();
        }
      }
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!veh_handle_) {
    veh_handle_ = AddVectoredExceptionHandler(1, ExceptionHandlerCallback);
    SetUnhandledExceptionFilter(LastChanceExceptionFilter);

    if (IsDebuggerPresent()) {
      // TODO(benvanik): do we need a continue handler if a debugger is
      // attached?
      // vch_handle_ = AddVectoredContinueHandler(1, ExceptionHandlerCallback);
    }
  }

  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < rex::countof(handlers_) - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }

  bool has_any = false;
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first) {
      has_any = true;
      break;
    }
  }
  if (!has_any) {
    if (veh_handle_) {
      RemoveVectoredExceptionHandler(veh_handle_);
      veh_handle_ = nullptr;
    }
    if (vch_handle_) {
      RemoveVectoredContinueHandler(vch_handle_);
      vch_handle_ = nullptr;
    }
  }
}

}  // namespace rex::arch

#endif  // REX_PLATFORM_WIN32
