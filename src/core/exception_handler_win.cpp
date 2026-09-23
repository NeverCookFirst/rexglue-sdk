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

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include <fmt/format.h>

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
ExceptionHandler::CrashReporter crash_reporter_ = nullptr;

// Kept in its own function because MSVC refuses __try in a function that also
// has C++ objects to unwind, and the filter below has those.
bool RunCrashReporterGuarded() {
  __try {
    crash_reporter_();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

LONG WINAPI LastChanceExceptionFilter(PEXCEPTION_POINTERS ex_info) {
  const EXCEPTION_RECORD* record = ex_info->ExceptionRecord;
  const uint64_t address = reinterpret_cast<uint64_t>(record->ExceptionAddress);
  REXLOG_CRITICAL("[FATAL] Unhandled exception 0x{:08X} at host 0x{:016X}",
                  static_cast<uint32_t>(record->ExceptionCode), address);
  // ASLR moves the executable on every launch, so the raw address above cannot
  // be looked up in anything. The offset from the module base can - in the
  // .map or .pdb of the matching build - which is what turns a user's log into
  // a function name without the dump.
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &module) &&
      module) {
    wchar_t module_path[MAX_PATH] = {};
    GetModuleFileNameW(module, module_path, MAX_PATH);
    const wchar_t* module_name = wcsrchr(module_path, L'\\');
    module_name = module_name ? module_name + 1 : module_path;
    char narrow_name[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, module_name, -1, narrow_name, MAX_PATH, nullptr, nullptr);
    REXLOG_CRITICAL("[FATAL] That is {}+0x{:X} (module base 0x{:016X})", narrow_name,
                    address - reinterpret_cast<uint64_t>(module),
                    reinterpret_cast<uint64_t>(module));
  } else {
    REXLOG_CRITICAL("[FATAL] The faulting address is not inside any loaded module");
  }
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2) {
    REXLOG_CRITICAL("[FATAL] Access violation: {} of 0x{:016X}",
                    record->ExceptionInformation[0] ? "write" : "read",
                    static_cast<uint64_t>(record->ExceptionInformation[1]));
  }
  if (auto logger = ::rex::GetLogger()) {
    logger->flush();
  }
  if (crash_reporter_) {
    // Best effort: the reporter reads guest memory and kernel tables that a
    // crash may itself have corrupted, so a second fault here must not hide
    // the lines already written - hence the flush above and the guard below.
    if (!RunCrashReporterGuarded()) {
      REXLOG_CRITICAL("[FATAL] The guest crash report itself faulted; skipping it");
    }
    if (auto logger = ::rex::GetLogger()) {
      logger->flush();
    }
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

// Installed at load time, not when the first handler is: one user's game
// died 35 launches in a row between "UI font loaded" and "Guest memory arena
// mapped" without a single line, because the filter used to go in together
// with the MMIO handler - after the memory arena exists. Anything that dies
// earlier left nothing behind. Install() re-applies it; that is harmless.
// A process can also die without any SEH exception reaching the filter: an
// uncaught C++ exception ends in std::terminate, abort() raises SIGABRT, and
// the CRT's invalid-parameter and pure-call paths fast-fail. None of those
// left a line either. Each gets a handler that says what it was and flushes,
// then lets the default behaviour follow.
void LogAndFlush(const char* what) {
  REXLOG_CRITICAL("[FATAL] {}", what);
  if (auto logger = ::rex::GetLogger()) {
    logger->flush();
  }
}

[[noreturn]] void OnTerminate() {
  std::string reason = "std::terminate called";
  if (auto current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      reason = fmt::format("uncaught C++ exception: {}", e.what());
    } catch (...) {
      reason = "uncaught C++ exception of a non-std type";
    }
  }
  LogAndFlush(reason.c_str());
  if (crash_reporter_) {
    RunCrashReporterGuarded();
  }
  std::abort();
}

void OnSigAbort(int) { LogAndFlush("abort() called"); }

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                        uintptr_t) {
  LogAndFlush("CRT invalid parameter");
}

void OnPureCall() { LogAndFlush("pure virtual call"); }

const bool kLastChanceFilterInstalled = [] {
  SetUnhandledExceptionFilter(LastChanceExceptionFilter);
  std::set_terminate(OnTerminate);
  std::signal(SIGABRT, OnSigAbort);
  _set_invalid_parameter_handler(OnInvalidParameter);
  _set_purecall_handler(OnPureCall);
  return true;
}();

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

void ExceptionHandler::SetCrashReporter(CrashReporter fn) { crash_reporter_ = fn; }

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
