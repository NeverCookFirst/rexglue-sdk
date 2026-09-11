/**
 * @file        ui/overlay/console_commands.cpp
 *
 * @brief       Sample argument-taking console commands (echo, find) that
 *              exercise the REXCVAR_DEFINE_COMMAND_ARGS path end to end.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/ui/keybinds.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

void ConsoleEcho(std::string_view args) {
  REXLOG_INFO("{}", args);
}

void ConsoleFind(std::string_view args) {
  while (!args.empty() && args.front() == ' ')
    args.remove_prefix(1);
  while (!args.empty() && args.back() == ' ')
    args.remove_suffix(1);
  if (args.empty()) {
    REXLOG_INFO("find: usage: find <substr>");
    return;
  }
  std::string needle(args);
  int matches = 0;
  for (const auto& n : rex::cvar::ListFlags()) {
    if (n.find(needle) != std::string::npos) {
      REXLOG_INFO("  {}", n);
      ++matches;
    }
  }
  REXLOG_INFO("find: {} match(es) for '{}'", matches, needle);
}

// ---------------------------------------------------------------------------
// Guest memory inspection.
//
// Reverse-engineering the title means asking what a particular global holds
// while the game runs, and until now the only way to answer that was to add a
// logging hook and regenerate the recompiled sources - minutes of work for one
// 32-bit read. These read and write guest memory directly instead.
//
// Addresses are guest addresses, hexadecimal, with or without the 0x. Values
// are read and written big-endian, because the guest is.
// ---------------------------------------------------------------------------

// Reading an unmapped guest page faults the host process, so a typo must not be
// able to take the game down with it. An address window turned out to be the
// wrong guard: it was the module image plus an allowance "for the heap above
// it", which quietly excluded the heap the title allocates its objects from.
// The guest address space is
//
//   0x00000000-0x3FFFFFFF  virtual, 4K pages
//   0x40000000-0x7FFFFFFF  virtual, 64K pages
//   0x80000000-0x8BFFFFFF  the XEX image
//   0xA0000000-0xBFFFFFFF  physical, 64K pages   <- game objects live here
//   0xC0000000-0xFFFFFFFF  physical, large pages
//
// so these commands ask the heap's own page table whether a span is committed
// and readable instead of guessing from the address.

std::string_view Trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ')
    s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ')
    s.remove_suffix(1);
  return s;
}

// Splits off the first whitespace-delimited token, advancing `rest`.
std::string_view NextToken(std::string_view& rest) {
  rest = Trim(rest);
  const size_t end = rest.find(' ');
  std::string_view tok = rest.substr(0, end);
  rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
  return tok;
}

bool ParseHex(std::string_view text, uint32_t& out) {
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() > 8) {
    return false;
  }
  uint32_t value = 0;
  for (const char c : text) {
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<uint32_t>(c - 'a') + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<uint32_t>(c - 'A') + 10;
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  out = value;
  return true;
}

// Null until the guest is up, so every caller has to check.
uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) {
    return nullptr;
  }
  return ks->memory()->virtual_membase();
}

// True when every page across [address, address + bytes) is committed and
// carries `want` (kMemoryProtectRead, optionally with kMemoryProtectWrite).
bool GuestSpanHasAccess(uint32_t address, uint64_t bytes, uint32_t want) {
  auto* ks = rex::system::kernel_state();
  auto* memory = ks ? ks->memory() : nullptr;
  if (!memory || bytes == 0) {
    return false;
  }
  const uint64_t end = static_cast<uint64_t>(address) + bytes;
  if (end > 0x100000000ull) {
    return false;
  }
  uint64_t at = address;
  while (at < end) {
    auto* heap = memory->LookupHeap(static_cast<uint32_t>(at));
    if (!heap) {
      return false;
    }
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(static_cast<uint32_t>(at), &info)) {
      return false;
    }
    if (!(info.state & rex::memory::kMemoryAllocationCommit) || (info.protect & want) != want) {
      return false;
    }
    // A zero-length region would spin; treat it as a single page.
    at += info.region_size ? info.region_size : 0x1000;
  }
  return true;
}

bool GuestWord(uint32_t address, uint32_t count, uint8_t*& base_out) {
  base_out = GuestBase();
  if (!base_out) {
    REXLOG_INFO("The guest is not running yet.");
    return false;
  }
  if (address % 4 != 0) {
    REXLOG_INFO("Address 0x{:08X} is not 4-byte aligned.", address);
    return false;
  }
  if (!GuestSpanHasAccess(address, static_cast<uint64_t>(count) * 4,
                          rex::memory::kMemoryProtectRead)) {
    REXLOG_INFO("Address 0x{:08X} is not committed, readable guest memory.", address);
    return false;
  }
  return true;
}

uint32_t LoadBE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void ConsolePeek(std::string_view args) {
  std::string_view rest = args;
  const std::string_view addr_tok = NextToken(rest);
  const std::string_view count_tok = NextToken(rest);

  uint32_t address = 0;
  if (addr_tok.empty() || !ParseHex(addr_tok, address)) {
    REXLOG_INFO("peek: usage: peek <hex address> [word count]");
    return;
  }
  uint32_t count = 1;
  if (!count_tok.empty() && (!ParseHex(count_tok, count) || count == 0 || count > 64)) {
    REXLOG_INFO("peek: word count must be 1-40 (hex).");
    return;
  }

  uint8_t* base = nullptr;
  if (!GuestWord(address, count, base)) {
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = address + i * 4;
    REXLOG_INFO("  0x{:08X}: {:08X}", at, LoadBE(base + at));
  }
}

void ConsolePoke(std::string_view args) {
  std::string_view rest = args;
  const std::string_view addr_tok = NextToken(rest);
  const std::string_view value_tok = NextToken(rest);

  uint32_t address = 0;
  uint32_t value = 0;
  if (addr_tok.empty() || value_tok.empty() || !ParseHex(addr_tok, address) ||
      !ParseHex(value_tok, value)) {
    REXLOG_INFO("poke: usage: poke <hex address> <hex value>");
    return;
  }

  uint8_t* base = nullptr;
  if (!GuestWord(address, 1, base)) {
    return;
  }
  uint8_t* p = base + address;
  const uint32_t previous = LoadBE(p);
  p[0] = static_cast<uint8_t>(value >> 24);
  p[1] = static_cast<uint8_t>(value >> 16);
  p[2] = static_cast<uint8_t>(value >> 8);
  p[3] = static_cast<uint8_t>(value);
  REXLOG_INFO("poke: 0x{:08X} {:08X} -> {:08X}", address, previous, value);
}

// ---------------------------------------------------------------------------
// Calling guest functions.
//
// peek and poke answer "what does this global hold", but much of the title's
// behaviour hangs off virtual methods that nothing calls directly. The menu
// item that opens the character grid is reached only through slot 93 of a
// vtable: there is no static call site to read and no flag to flip, so the only
// way to learn what it does is to call it.
//
// Two things make that dangerous, and both are handled here rather than left to
// whoever types the command:
//
//   * The console runs on the window thread. That thread has no bound guest
//     ThreadState and no guest stack, so entering guest code from it would run
//     on a host stack with no PCR. `call` therefore never executes anything --
//     it parks the request and returns, and a guest thread picks it up.
//
//   * Even on a guest thread we are interrupting a function that expects its
//     registers back. ExecuteTrap exists for exactly this case -- it is how the
//     kernel delivers APCs -- and saves and restores the whole PPCContext
//     around the call.
//
// The drain runs from XamInputGetState, which the title polls every frame on
// the thread that owns the menus, so a queued call lands on the same thread
// that built the object it is meant to poke at.
// ---------------------------------------------------------------------------

// Register arguments only. FunctionDispatcher's stack-arg path is documented as
// wrong for anything but 32-bit values, so refuse rather than pass garbage.
constexpr size_t kMaxCallArgs = 8;

struct PendingGuestCall {
  uint32_t address = 0;
  uint64_t args[kMaxCallArgs] = {};
  size_t arg_count = 0;
  bool pending = false;
};

std::mutex g_pending_call_mutex;
PendingGuestCall g_pending_call;

// Read once per input poll, so the hot path never touches the mutex.
std::atomic<bool> g_pending_call_armed{false};

void ConsoleCall(std::string_view args) {
  std::string_view rest = args;
  const std::string_view addr_tok = NextToken(rest);

  uint32_t address = 0;
  if (addr_tok.empty() || !ParseHex(addr_tok, address)) {
    REXLOG_INFO("call: usage: call <hex address> [hex arg]...  (up to {} args, r3 onwards)",
                kMaxCallArgs);
    return;
  }
  if (address % 4 != 0) {
    REXLOG_INFO("call: address 0x{:08X} is not 4-byte aligned.", address);
    return;
  }
  // Recompiled code only ever lives in the XEX image; GetFunction() checks the
  // rest, on the guest thread.
  if (address < 0x82000000 || address >= 0x8C000000) {
    REXLOG_INFO("call: address 0x{:08X} is outside the code image 0x82000000-0x8C000000.",
                address);
    return;
  }

  PendingGuestCall call;
  call.address = address;
  while (call.arg_count < kMaxCallArgs) {
    const std::string_view arg_tok = NextToken(rest);
    if (arg_tok.empty()) {
      break;
    }
    uint32_t value = 0;
    if (!ParseHex(arg_tok, value)) {
      REXLOG_INFO("call: '{}' is not a hex value.", arg_tok);
      return;
    }
    call.args[call.arg_count++] = value;
  }
  if (!Trim(rest).empty()) {
    REXLOG_INFO("call: at most {} arguments are supported.", kMaxCallArgs);
    return;
  }
  call.pending = true;

  {
    std::lock_guard<std::mutex> lock(g_pending_call_mutex);
    if (g_pending_call.pending) {
      REXLOG_INFO("call: a call to 0x{:08X} is still queued; try again in a moment.",
                  g_pending_call.address);
      return;
    }
    g_pending_call = call;
  }
  g_pending_call_armed.store(true, std::memory_order_release);
  REXLOG_INFO("call: queued 0x{:08X} with {} argument(s); it runs on the next input poll.",
              address, call.arg_count);
}

// ---------------------------------------------------------------------------
// Finding live objects.
//
// Knowing a class's vtable address is only half of what an experiment needs:
// calling one of its methods takes a `this`, and that is a heap pointer that
// only exists while the game is running. Every C++ object with virtual methods
// stores its vtable address in its first word, so scanning guest memory for
// that word finds every live instance of the class.
//
// The scan has to skip uncommitted pages, because reading one faults the host
// process and takes the game down with it. QueryRegionInfo walks the heap's own
// page table, so whole unmapped regions are stepped over rather than touched.
//
// This is a snapshot, not a safe iteration: a guest thread is free to decommit
// a region between the query and the read. In practice the title does not churn
// its heaps mid-frame, but that is why this is a debugging command and not
// something to run in a loop.
// ---------------------------------------------------------------------------

// Enough to see a pattern without flooding the console; the total is reported
// either way.
constexpr uint32_t kMaxFindHits = 64;

// Walks every committed, readable region in [start, end) looking for a 32-bit
// big-endian word. Returns the first `max_hits` addresses and reports the true
// total through `total_out`.
std::vector<uint32_t> ScanGuestForWord(uint64_t start, uint64_t end, uint32_t needle,
                                       uint32_t max_hits, uint32_t& total_out) {
  std::vector<uint32_t> hits;
  total_out = 0;

  auto* ks = rex::system::kernel_state();
  auto* memory = ks ? ks->memory() : nullptr;
  if (!memory) {
    return hits;
  }
  uint8_t* base = memory->virtual_membase();

  uint64_t address = start;
  while (address < end) {
    auto* heap = memory->LookupHeap(static_cast<uint32_t>(address));
    if (!heap) {
      // No heap covers this address at all - step to the next 64K boundary
      // rather than crawling a page at a time through a hole.
      address = (address + 0x10000) & ~uint64_t(0xFFFF);
      continue;
    }

    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(static_cast<uint32_t>(address), &info)) {
      address += 0x1000;
      continue;
    }
    // region_size can come back as zero for a free region; never let the walk
    // stall on one.
    const uint64_t region_size = info.region_size ? info.region_size : 0x1000;
    const uint64_t region_end = std::min<uint64_t>(address + region_size, end);

    const bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0;
    const bool readable = (info.protect & rex::memory::kMemoryProtectRead) != 0;
    if (!committed || !readable) {
      address = region_end > address ? region_end : address + 0x1000;
      continue;
    }

    for (uint64_t at = address; at + 4 <= region_end; at += 4) {
      if (LoadBE(base + at) == needle) {
        if (hits.size() < max_hits) {
          hits.push_back(static_cast<uint32_t>(at));
        }
        ++total_out;
      }
    }
    address = region_end > address ? region_end : address + 0x1000;
  }
  return hits;
}

void ConsoleFindWord(std::string_view args) {
  std::string_view rest = args;
  const std::string_view value_tok = NextToken(rest);
  const std::string_view start_tok = NextToken(rest);
  const std::string_view end_tok = NextToken(rest);

  uint32_t needle = 0;
  if (value_tok.empty() || !ParseHex(value_tok, needle)) {
    REXLOG_INFO("findword: usage: findword <hex value> [hex start] [hex end]");
    return;
  }
  // The whole guest address space by default. Only committed regions are read,
  // and the holes are stepped over 64K at a time, so this costs little.
  uint64_t start = 0;
  uint64_t end = 0x100000000ull;
  uint32_t bound = 0;
  if (!start_tok.empty()) {
    if (!ParseHex(start_tok, bound)) {
      REXLOG_INFO("findword: bad start address.");
      return;
    }
    start = bound;
  }
  if (!end_tok.empty()) {
    if (!ParseHex(end_tok, bound)) {
      REXLOG_INFO("findword: bad end address.");
      return;
    }
    end = bound;
  }
  if (start >= end) {
    REXLOG_INFO("findword: empty range 0x{:08X}-0x{:08X}.", static_cast<uint32_t>(start),
                static_cast<uint32_t>(end));
    return;
  }
  start &= ~uint64_t(3);

  if (!GuestBase()) {
    REXLOG_INFO("The guest is not running yet.");
    return;
  }

  uint32_t hits = 0;
  const std::vector<uint32_t> found = ScanGuestForWord(start, end, needle, kMaxFindHits, hits);
  for (uint32_t at : found) {
    REXLOG_INFO("  0x{:08X}", at);
  }

  if (hits > kMaxFindHits) {
    REXLOG_INFO("findword: {} match(es) for {:08X}; only the first {} are listed.", hits, needle,
                kMaxFindHits);
  } else {
    REXLOG_INFO("findword: {} match(es) for {:08X}.", hits, needle);
  }
}


// ---------------------------------------------------------------------------
// Doing all of that without the console in the way.
//
// The ring menu is a hold-to-show control: it exists only while the player
// holds Y, and opening the console takes the input that keeps it up, so by the
// time a command can be typed the object is already destroyed. Everything above
// is therefore useless against it -- an address found in one console session is
// a freed block by the next.
//
// `bindcall` closes that gap. It binds a key (F6 by default) to "scan for this
// vtable, and call this function with whatever instance turns up". The player
// holds Y with the console closed, taps the key, and the scan happens while the
// object is alive. The key is consumed by the bind system, so the guest never
// sees it and the ring stays up.
//
// The scan itself runs here on the window thread, which only reads memory; the
// call it produces goes through the same queue as `call` and lands on a guest
// thread.
// ---------------------------------------------------------------------------

std::mutex g_bindcall_mutex;
uint32_t g_bindcall_vtable = 0;
uint32_t g_bindcall_func = 0;
bool g_bindcall_registered = false;

void BindCallFire() {
  uint32_t vtable = 0;
  uint32_t func = 0;
  {
    std::lock_guard<std::mutex> lock(g_bindcall_mutex);
    vtable = g_bindcall_vtable;
    func = g_bindcall_func;
  }
  if (!vtable || !func) {
    return;
  }

  uint32_t total = 0;
  const std::vector<uint32_t> found = ScanGuestForWord(0, 0x100000000ull, vtable, 8, total);
  if (found.empty()) {
    REXLOG_INFO("bindcall: no live object with vtable {:08X} right now.", vtable);
    return;
  }
  if (total > 1) {
    // More than one instance is a reason to stop and look, not to guess.
    REXLOG_INFO("bindcall: {} objects with vtable {:08X}; refusing to guess. Addresses:", total,
                vtable);
    for (uint32_t at : found) {
      REXLOG_INFO("  0x{:08X}", at);
    }
    return;
  }

  const uint32_t instance = found.front();
  {
    std::lock_guard<std::mutex> lock(g_pending_call_mutex);
    if (g_pending_call.pending) {
      REXLOG_INFO("bindcall: a call is still queued; ignoring this press.");
      return;
    }
    g_pending_call = PendingGuestCall{};
    g_pending_call.address = func;
    g_pending_call.args[0] = instance;
    g_pending_call.arg_count = 1;
    g_pending_call.pending = true;
  }
  g_pending_call_armed.store(true, std::memory_order_release);
  REXLOG_INFO("bindcall: found {:08X} at 0x{:08X}; queued {:08X}(0x{:08X}).", vtable, instance,
              func, instance);
}

void ConsoleBindCall(std::string_view args) {
  std::string_view rest = args;
  const std::string_view vtable_tok = NextToken(rest);
  const std::string_view func_tok = NextToken(rest);

  uint32_t vtable = 0;
  uint32_t func = 0;
  if (vtable_tok.empty() || func_tok.empty() || !ParseHex(vtable_tok, vtable) ||
      !ParseHex(func_tok, func)) {
    REXLOG_INFO("bindcall: usage: bindcall <hex vtable> <hex function>");
    REXLOG_INFO("bindcall: then hold the in-game control and press the bound key (default F6).");
    return;
  }
  if (func < 0x82000000 || func >= 0x8C000000) {
    REXLOG_INFO("bindcall: 0x{:08X} is outside the code image 0x82000000-0x8C000000.", func);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_bindcall_mutex);
    g_bindcall_vtable = vtable;
    g_bindcall_func = func;
    if (!g_bindcall_registered) {
      // Registered on first use rather than at startup: the bind system is not
      // up yet when this translation unit's statics run.
      rex::ui::RegisterBind("bind_console_bindcall", "F6",
                            "Scan for the bindcall vtable and call its function", BindCallFire);
      g_bindcall_registered = true;
    }
  }
  REXLOG_INFO("bindcall: F6 will now look for vtable {:08X} and call {:08X} with it.", vtable,
              func);
}

// ---------------------------------------------------------------------------
// bindcall2: one key press, two live objects.
//
// `bindcall` can only ever produce a one-argument call, and that is not enough
// for a method that takes a context object alongside `this`. The ring's confirm
// handler is exactly that shape: slot 7 of the item is called as
// `item->vtable[28](item, ctx)`, and `ctx` is only reachable by walking a
// couple of pointers out of another short-lived object.
//
// Both objects die when the ring closes, and opening the console closes the
// ring, so neither address survives long enough to be typed by hand. This binds
// the whole thing to a key: find object A by its vtable, find object B by its
// vtable, walk a chain of offsets out of B to produce the second argument, and
// queue `func(A, derived)`.
//
// Every intermediate is logged. A chain that dies halfway is the interesting
// result, not an error to hide.
// ---------------------------------------------------------------------------

constexpr size_t kMaxChainOffsets = 8;

std::mutex g_bindcall2_mutex;
uint32_t g_bindcall2_vtable_a = 0;
uint32_t g_bindcall2_func = 0;
uint32_t g_bindcall2_vtable_b = 0;
uint32_t g_bindcall2_chain[kMaxChainOffsets] = {};
size_t g_bindcall2_chain_len = 0;
bool g_bindcall2_registered = false;

// Quiet counterpart to GuestWord(): a failed read here is a finding to report
// with context, not a bare "not committed" line from a helper that has no idea
// what is being walked.
bool TryReadGuestWord(uint32_t address, uint32_t& out) {
  uint8_t* base = GuestBase();
  if (!base || address % 4 != 0) {
    return false;
  }
  if (!GuestSpanHasAccess(address, 4, rex::memory::kMemoryProtectRead)) {
    return false;
  }
  out = LoadBE(base + address);
  return true;
}

// Sole live instance of `vtable`, or 0 with the reason logged.
uint32_t FindSoleInstance(uint32_t vtable, const char* what) {
  uint32_t total = 0;
  const std::vector<uint32_t> found = ScanGuestForWord(0, 0x100000000ull, vtable, 8, total);
  if (found.empty()) {
    REXLOG_INFO("bindcall2: no live {} (vtable {:08X}) right now.", what, vtable);
    return 0;
  }
  if (total > 1) {
    REXLOG_INFO("bindcall2: {} objects with vtable {:08X} ({}); refusing to guess. Addresses:",
                total, vtable, what);
    for (uint32_t at : found) {
      REXLOG_INFO("  0x{:08X}", at);
    }
    return 0;
  }
  return found.front();
}

void BindCall2Fire() {
  uint32_t vtable_a = 0;
  uint32_t func = 0;
  uint32_t vtable_b = 0;
  uint32_t chain[kMaxChainOffsets] = {};
  size_t chain_len = 0;
  {
    std::lock_guard<std::mutex> lock(g_bindcall2_mutex);
    vtable_a = g_bindcall2_vtable_a;
    func = g_bindcall2_func;
    vtable_b = g_bindcall2_vtable_b;
    chain_len = g_bindcall2_chain_len;
    std::copy(std::begin(g_bindcall2_chain), std::end(g_bindcall2_chain), std::begin(chain));
  }
  if (!vtable_a || !func || !vtable_b) {
    return;
  }

  const uint32_t object_a = FindSoleInstance(vtable_a, "first object");
  if (!object_a) {
    return;
  }
  const uint32_t object_b = FindSoleInstance(vtable_b, "second object");
  if (!object_b) {
    return;
  }

  uint32_t derived = object_b;
  for (size_t i = 0; i < chain_len; ++i) {
    const uint32_t at = derived + chain[i];
    uint32_t next = 0;
    if (!TryReadGuestWord(at, next)) {
      REXLOG_INFO("bindcall2: chain step {} reads 0x{:08X} (+{}), which is not readable; stopping.",
                  i, at, chain[i]);
      return;
    }
    REXLOG_INFO("bindcall2: chain step {}: [0x{:08X}] = {:08X}", i, at, next);
    if (!next) {
      REXLOG_INFO("bindcall2: chain step {} is null; nothing was called.", i);
      return;
    }
    derived = next;
  }

  {
    std::lock_guard<std::mutex> lock(g_pending_call_mutex);
    if (g_pending_call.pending) {
      REXLOG_INFO("bindcall2: a call is still queued; ignoring this press.");
      return;
    }
    g_pending_call = PendingGuestCall{};
    g_pending_call.address = func;
    g_pending_call.args[0] = object_a;
    g_pending_call.args[1] = derived;
    g_pending_call.arg_count = 2;
    g_pending_call.pending = true;
  }
  g_pending_call_armed.store(true, std::memory_order_release);
  REXLOG_INFO("bindcall2: queued {:08X}(0x{:08X}, 0x{:08X}).", func, object_a, derived);
}

void ConsoleBindCall2(std::string_view args) {
  std::string_view rest = args;
  const std::string_view vtable_a_tok = NextToken(rest);
  const std::string_view func_tok = NextToken(rest);
  const std::string_view vtable_b_tok = NextToken(rest);
  const std::string_view chain_tok = NextToken(rest);

  uint32_t vtable_a = 0;
  uint32_t func = 0;
  uint32_t vtable_b = 0;
  if (vtable_a_tok.empty() || func_tok.empty() || vtable_b_tok.empty() ||
      !ParseHex(vtable_a_tok, vtable_a) || !ParseHex(func_tok, func) ||
      !ParseHex(vtable_b_tok, vtable_b)) {
    REXLOG_INFO("bindcall2: usage: bindcall2 <hex vtableA> <hex func> <hex vtableB> [off,off,...]");
    REXLOG_INFO("bindcall2: calls func(A, walk(B, offsets)); offsets are hex, F5 fires it.");
    return;
  }
  if (func < 0x82000000 || func >= 0x8C000000) {
    REXLOG_INFO("bindcall2: 0x{:08X} is outside the code image 0x82000000-0x8C000000.", func);
    return;
  }

  uint32_t chain[kMaxChainOffsets] = {};
  size_t chain_len = 0;
  std::string_view chain_rest = chain_tok;
  while (!chain_rest.empty()) {
    const size_t comma = chain_rest.find(',');
    const std::string_view piece = chain_rest.substr(0, comma);
    uint32_t offset = 0;
    if (piece.empty() || !ParseHex(piece, offset)) {
      REXLOG_INFO("bindcall2: '{}' is not a hex offset.", piece);
      return;
    }
    if (chain_len == kMaxChainOffsets) {
      REXLOG_INFO("bindcall2: at most {} offsets are supported.", kMaxChainOffsets);
      return;
    }
    chain[chain_len++] = offset;
    if (comma == std::string_view::npos) {
      break;
    }
    chain_rest.remove_prefix(comma + 1);
  }

  {
    std::lock_guard<std::mutex> lock(g_bindcall2_mutex);
    g_bindcall2_vtable_a = vtable_a;
    g_bindcall2_func = func;
    g_bindcall2_vtable_b = vtable_b;
    std::copy(std::begin(chain), std::end(chain), std::begin(g_bindcall2_chain));
    g_bindcall2_chain_len = chain_len;
    if (!g_bindcall2_registered) {
      rex::ui::RegisterBind("bind_console_bindcall2", "F5",
                            "Find two objects by vtable and call a function with both",
                            BindCall2Fire);
      g_bindcall2_registered = true;
    }
  }
  REXLOG_INFO("bindcall2: F5 will call {:08X}(<{:08X}>, walk(<{:08X}>, {} offset(s))).", func,
              vtable_a, vtable_b, chain_len);
}

}  // namespace

// Runs one queued `call` on the calling guest thread, or does nothing.
//
// Declared by hand in its one caller rather than in a header: a header the game
// executable also includes would force the executable to be relinked, and the
// whole point of keeping these commands here is that only rexruntime.dll has to
// be rebuilt.
extern "C" void RexConsoleDrainPendingGuestCall() {
  if (!g_pending_call_armed.load(std::memory_order_acquire)) {
    return;
  }

  PendingGuestCall call;
  {
    std::lock_guard<std::mutex> lock(g_pending_call_mutex);
    if (!g_pending_call.pending) {
      g_pending_call_armed.store(false, std::memory_order_release);
      return;
    }
    call = g_pending_call;
    g_pending_call.pending = false;
  }
  g_pending_call_armed.store(false, std::memory_order_release);

  // Not a guest thread after all - drop the request rather than guess.
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts) {
    REXLOG_INFO("call: no guest thread bound; 0x{:08X} was dropped.", call.address);
    return;
  }
  auto* ks = rex::system::kernel_state();
  auto* dispatcher = ks ? ks->function_dispatcher() : nullptr;
  if (!dispatcher) {
    REXLOG_INFO("call: no function dispatcher; 0x{:08X} was dropped.", call.address);
    return;
  }
  // The address has to name a recompiled function. Without this check the
  // dispatcher walks into its invalid-function trap, which is a far worse way
  // to find out about a typo.
  if (!dispatcher->GetFunction(call.address)) {
    REXLOG_INFO("call: 0x{:08X} is not in the function table; nothing was called.", call.address);
    return;
  }

  REXLOG_INFO("call: entering 0x{:08X} on guest thread {}...", call.address, ts->thread_id());
  const uint64_t result =
      dispatcher->ExecuteTrap(ts, call.address, call.args, call.arg_count);
  REXLOG_INFO("call: 0x{:08X} returned {:016X} (r3 low word {:08X})", call.address, result,
              static_cast<uint32_t>(result));
}


// ---------------------------------------------------------------------------
// Guest thread inspection, and a watchdog for the silent hang.
//
// When the title wedges after leaving a level the process stays alive and the
// overlay stays responsive; the only trace in the log is that the game's own
// 1.2 s ToyPad colour tick stops and never resumes. Nothing says which guest
// thread stopped, because a statically recompiled build has no program counter
// to sample - PPCContext carries no NIA.
//
// It carries enough to tell running from wedged, though. lr moves on every bl,
// last_indirect_target moves on every virtual call, r1 moves with call depth,
// and ctr moves with every indirect branch. A thread whose (lr, ind, r1, ctr)
// tuple is bit-for-bit identical for tens of seconds is not running guest code.
//
// Sampling another thread's context without stopping it is racy by
// construction, and that is fine here: a torn read changes the fingerprint,
// which can only make a wedged thread look alive, never the reverse. A false
// "moving" costs one more sample; a false "still" would cost a wrong diagnosis.

struct ThreadTrack {
  uint64_t fingerprint = 0;
  int64_t first_seen_ms = 0;
  int64_t last_move_ms = 0;
  bool seen_moving = false;
};

std::mutex g_thread_track_mutex;
std::unordered_map<uint32_t, ThreadTrack> g_thread_tracks;

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint64_t ContextFingerprint(const PPCContext* ctx) {
  constexpr uint64_t kPrime = 1099511628211ull;
  uint64_t fp = 1469598103934665603ull;
  fp = (fp ^ ctx->lr) * kPrime;
  fp = (fp ^ ctx->last_indirect_target) * kPrime;
  fp = (fp ^ ctx->r1.u64) * kPrime;
  fp = (fp ^ ctx->ctr.u64) * kPrime;
  return fp;
}

// Samples every live guest thread once. Returns one line per thread and, in
// stalled_out, the ids that have not moved for stall_ms.
std::vector<std::string> SampleGuestThreads(int64_t stall_ms, std::vector<uint32_t>* stalled_out) {
  std::vector<std::string> lines;
  auto* kernel = rex::system::kernel_state();
  if (!kernel) {
    lines.emplace_back("  (no kernel state - the title has not started)");
    return lines;
  }

  auto threads = kernel->object_table()->GetObjectsByType<rex::system::XThread>();
  std::sort(threads.begin(), threads.end(),
            [](const auto& a, const auto& b) { return a->thread_id() < b->thread_id(); });

  const int64_t now = SteadyMs();
  std::lock_guard<std::mutex> lock(g_thread_track_mutex);
  for (auto& thread : threads) {
    if (!thread->is_guest_thread() || !thread->is_running()) {
      continue;
    }
    auto* thread_state = thread->thread_state();
    auto* ctx = thread_state ? thread_state->context() : nullptr;
    if (!ctx) {
      continue;
    }

    const uint32_t id = thread->thread_id();
    const uint64_t fingerprint = ContextFingerprint(ctx);
    ThreadTrack& track = g_thread_tracks[id];
    if (track.first_seen_ms == 0) {
      track.first_seen_ms = now;
      track.last_move_ms = now;
      track.fingerprint = fingerprint;
    } else if (fingerprint != track.fingerprint) {
      track.fingerprint = fingerprint;
      track.last_move_ms = now;
      track.seen_moving = true;
    }

    // A thread that has not moved since it was first looked at counts as
    // stalled too, once it has been watched long enough to say so. The first
    // version required a thread to have moved at least once, which made the
    // tool useless in the one case it was written for - a guest that was
    // already frozen before anyone asked. It reported "0 stalled" while every
    // register in the process sat unchanged for half a minute.
    const int64_t still_ms = now - track.last_move_ms;
    const int64_t watched_ms = now - track.first_seen_ms;
    const bool stalled = still_ms >= stall_ms && watched_ms >= stall_ms;
    if (stalled && stalled_out) {
      stalled_out->push_back(id);
    }

    std::string state;
    if (stalled) {
      state = fmt::format("STILL {:.1f}s{}", still_ms / 1000.0,
                          track.seen_moving ? "" : " (never seen moving)");
    } else if (!track.seen_moving) {
      state = fmt::format("no movement yet, watched {:.1f}s", watched_ms / 1000.0);
    } else {
      state = "moving";
    }

    lines.push_back(fmt::format("  tid {:04X} {:<4} lr={:08X} ind={:08X} r1={:08X} r3={:08X}  {}",
                                id, thread->main_thread() ? "main" : "", uint32_t(ctx->lr),
                                ctx->last_indirect_target, uint32_t(ctx->r1.u64),
                                uint32_t(ctx->r3.u64), state));
  }

  if (lines.empty()) {
    lines.emplace_back("  (no running guest threads)");
  }
  return lines;
}

void ConsoleThreads(std::string_view args) {
  std::string_view rest = args;
  const std::string_view seconds_token = NextToken(rest);
  int64_t stall_ms = 5000;
  if (!seconds_token.empty()) {
    const int seconds = std::atoi(std::string(seconds_token).c_str());
    if (seconds <= 0) {
      REXLOG_INFO("threads: usage: threads [seconds still before it counts as stalled]");
      return;
    }
    stall_ms = int64_t(seconds) * 1000;
  }

  // Sample, wait out the stall threshold, sample again. Anything that has not
  // moved across that gap is genuinely not running, whether or not this is the
  // first time it was looked at. 250 ms could never reach a 5 s threshold from
  // a standing start, so a single `threads` call used to report nothing.
  SampleGuestThreads(stall_ms, nullptr);
  std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms + 250));

  std::vector<uint32_t> stalled;
  const auto lines = SampleGuestThreads(stall_ms, &stalled);
  REXLOG_INFO("threads: guest threads (STILL = no guest code for {}s):", stall_ms / 1000);
  for (const auto& line : lines) {
    REXLOG_INFO("{}", line);
  }
  REXLOG_INFO("threads: {} stalled.", stalled.size());
}

// ---------------------------------------------------------------------------
// The watchdog itself.
//
// Started by hand rather than from a static initialiser: creating a thread
// while the DLL's static objects are constructed runs under the Windows loader
// lock, which is a deadlock waiting to happen. A debugging aid has no business
// risking that, and the hang is reproduced deliberately anyway - typing
// "hangwatch on" as the session starts costs nothing.

std::atomic<bool> g_watchdog_running{false};
std::atomic<bool> g_watchdog_enabled{false};
std::atomic<int64_t> g_watchdog_stall_ms{20000};

void WatchdogLoop() {
  int64_t last_report_ms = 0;
  bool was_stalled = false;
  while (g_watchdog_running.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!g_watchdog_enabled.load()) {
      continue;
    }

    std::vector<uint32_t> stalled;
    const auto lines = SampleGuestThreads(g_watchdog_stall_ms.load(), &stalled);
    const int64_t now = SteadyMs();
    if (stalled.empty()) {
      if (was_stalled) {
        REXLOG_INFO("hangwatch: guest threads are running again.");
        was_stalled = false;
      }
      continue;
    }

    // One block when the stall starts, then a reminder once a minute, so a
    // hang that lasts an hour does not produce an hour of log.
    if (was_stalled && now - last_report_ms < 60000) {
      continue;
    }
    last_report_ms = now;
    REXLOG_WARN("hangwatch: {} guest thread(s) have run no guest code for {}s:", stalled.size(),
                   g_watchdog_stall_ms.load() / 1000);
    for (const auto& line : lines) {
      REXLOG_WARN("{}", line);
    }
    was_stalled = true;
  }
}

void ConsoleHangWatch(std::string_view args) {
  std::string_view rest = args;
  const std::string_view token = NextToken(rest);

  if (token == "off") {
    g_watchdog_enabled.store(false);
    REXLOG_INFO("hangwatch: off.");
    return;
  }
  if (!token.empty() && token != "on") {
    const int seconds = std::atoi(std::string(token).c_str());
    if (seconds <= 0) {
      REXLOG_INFO("hangwatch: usage: hangwatch [on|off|<seconds>]");
      return;
    }
    g_watchdog_stall_ms.store(int64_t(seconds) * 1000);
  }

  if (!g_watchdog_running.exchange(true)) {
    std::thread(WatchdogLoop).detach();
  }
  g_watchdog_enabled.store(true);
  REXLOG_INFO("hangwatch: on, reporting any guest thread that runs no guest code for {}s.",
              g_watchdog_stall_ms.load() / 1000);
}


REXCVAR_DEFINE_COMMAND_ARGS(peek, ConsolePeek, "Console",
                            "Read guest memory: peek <hex address> [word count]");
REXCVAR_DEFINE_COMMAND_ARGS(poke, ConsolePoke, "Console",
                            "Write a guest word: poke <hex address> <hex value>");

REXCVAR_DEFINE_COMMAND_ARGS(bindcall, ConsoleBindCall, "Console",
                            "Bind a key to: find an object by vtable, then call a function with it");

REXCVAR_DEFINE_COMMAND_ARGS(bindcall2, ConsoleBindCall2, "Console",
                            "Bind a key to: find two objects by vtable and call func(A, walk(B))");

REXCVAR_DEFINE_COMMAND_ARGS(findword, ConsoleFindWord, "Console",
                            "Scan guest memory for a 32-bit value: findword <hex value> [start] [end]");

REXCVAR_DEFINE_COMMAND_ARGS(call, ConsoleCall, "Console",
                            "Call a guest function: call <hex address> [hex arg]...");

REXCVAR_DEFINE_COMMAND_ARGS(echo, ConsoleEcho, "Console", "Echo arguments to the console");
REXCVAR_DEFINE_COMMAND_ARGS(find, ConsoleFind, "Console",
                            "List cvar/command names containing a substring");

REXCVAR_DEFINE_COMMAND_ARGS(threads, ConsoleThreads, "Console",
                            "Dump guest threads and whether each is running: threads [seconds]");
REXCVAR_DEFINE_COMMAND_ARGS(hangwatch, ConsoleHangWatch, "Console",
                            "Report guest threads that stop running: hangwatch [on|off|<seconds>]");
