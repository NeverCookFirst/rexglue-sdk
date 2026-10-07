/**
 * @file        codegen/phase_gapfill.cpp
 * @brief       GapFill phase: find uncovered code regions and register them as functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/instruction.h"

#include <algorithm>
#include <functional>
#include <map>
#include <queue>
#include <unordered_set>

#include <rex/codegen/phases.h>
#include "phase_helpers.h"

#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>

#include <ppc.h>

using rex::codegen::ppc::decode_instruction;
using rex::codegen::ppc::Opcode;
using rex::memory::load_and_swap;

namespace rex::codegen {

namespace {

//=============================================================================
// GapFill to register uncovered code regions
//=============================================================================

// Split a code region into function segments based on terminators (blr, tail calls).
std::vector<CodeRegion> splitRegionOnTerminators(
    const CodeRegion& region, const BinaryView& binary,
    const std::unordered_set<uint32_t>& knownCallables) {
  std::vector<CodeRegion> segments;
  uint32_t segmentStart = region.start;

  for (uint32_t addr = region.start; addr < region.end; addr += 4) {
    const uint8_t* data = binary.translate(addr);
    if (!data)
      break;

    uint32_t raw = load_and_swap<uint32_t>(data);
    auto decoded = decode_instruction(addr, raw);
    bool shouldSplit = false;
    const char* reason = nullptr;

    // Check for terminators
    if (decoded.is_return()) {
      shouldSplit = true;
      reason = "blr";
    } else if (decoded.opcode == Opcode::b && decoded.branch_target.has_value()) {
      uint32_t target = decoded.branch_target.value();
      // Don't split on tail recursion (branch to own segment start)
      if (target != segmentStart && knownCallables.contains(target)) {
        shouldSplit = true;
        reason = "tail call";
      }
    }

    if (shouldSplit) {
      uint32_t segmentEnd = addr + 4;
      if (segmentEnd > segmentStart) {
        segments.push_back({segmentStart, segmentEnd});
        REXCODEGEN_TRACE("GapFill: split segment 0x{:08X}-0x{:08X} ({} at 0x{:08X})", segmentStart,
                         segmentEnd, reason, addr);
      }
      segmentStart = segmentEnd;
    }
  }

  // Handle remaining code after last terminator
  if (segmentStart < region.end) {
    segments.push_back({segmentStart, region.end});
  }

  return segments;
}

// Check if address looks like exception handler data (handler ptr + rdata ptr)
bool looksLikeExceptionData(const BinaryView& binary, const FunctionGraph& graph, uint32_t addr) {
  const uint8_t* data = binary.translate(addr);
  if (!data)
    return false;

  // Exception handler data pattern:
  // [addr+0]: pointer to __C_specific_handler (entry point)
  // [addr+4]: pointer to scope table in .rdata
  uint32_t firstDword = load_and_swap<uint32_t>(data);
  uint32_t secondDword = load_and_swap<uint32_t>(data + 4);

  // Check if first dword is a known entry point (like __C_specific_handler)
  if (!graph.isEntryPoint(firstDword)) {
    return false;
  }

  // Check if second dword points to .rdata section
  auto* rdataSection = binary.findSectionByName(".rdata");
  if (!rdataSection)
    return false;

  uint32_t rdataStart = rdataSection->baseAddress;
  uint32_t rdataEnd = rdataStart + rdataSection->size;

  if (secondDword >= rdataStart && secondDword < rdataEnd) {
    REXCODEGEN_TRACE(
        "GapFill: 0x{:08X} looks like exception data (handler=0x{:08X}, scope=0x{:08X}), skipping",
        addr, firstDword, secondDword);
    return true;
  }

  return false;
}

void gapFillCodeRegions(CodegenContext& ctx) {
  REXCODEGEN_TRACE("Analyze: checking for uncovered code regions...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& scan = ctx.scan;

  // Build set of known callables for tail call detection
  std::unordered_set<uint32_t> knownCallables;
  for (const auto& [addr, node] : graph.functions()) {
    knownCallables.insert(addr);
  }

  size_t gapsFound = 0;
  size_t segmentsCreated = 0;

  for (const auto& region : scan.codeRegions) {
    // Split region on terminators (blr, tail calls), then check each segment
    auto segments = splitRegionOnTerminators(region, binary, knownCallables);

    for (const auto& segment : segments) {
      // Skip if this segment's start is already a registered function entry
      if (graph.isEntryPoint(segment.start))
        continue;

      // Skip if this segment's start is inside another function
      if (auto* containingFunc = graph.getFunctionContaining(segment.start)) {
        continue;
      }

      // Skip if this looks like exception handler data (handler ptr + rdata ptr)
      if (looksLikeExceptionData(binary, graph, segment.start))
        continue;

      uint32_t segmentSize = segment.size();
      graph.addFunction(segment.start, segmentSize, FunctionAuthority::GAP_FILL, false);

      REXCODEGEN_TRACE("GapFill: registered sub_{:08X} (0x{:08X}-0x{:08X}, {} bytes)",
                       segment.start, segment.start, segment.end, segmentSize);
      segmentsCreated++;
    }

    gapsFound++;
  }

  if (segmentsCreated > 0) {
    REXCODEGEN_TRACE("Analyze: registered {} gap functions from {} regions", segmentsCreated,
                     gapsFound);
  } else {
    REXCODEGEN_TRACE("Analyze: no uncovered regions found");
  }
}

//=============================================================================
// Cleanup absorbed GAP_FILL functions
//=============================================================================

void cleanupAbsorbedGapFills(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  std::vector<const FunctionNode*> ordered;
  ordered.reserve(graph.functions().size());
  for (const auto& [addr, node] : graph.functions()) {
    ordered.push_back(node.get());
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
    return a->base() < b->base();
  });

  // containsAddress first rejects points outside [base, base + size). Only
  // lower-base owners whose overall end remains above this entry can absorb it.
  // Keep the real block/authority predicate for holes and declared-size owners.
  using EndEntry = std::pair<uint32_t, uint32_t>;
  std::priority_queue<EndEntry, std::vector<EndEntry>, std::greater<EndEntry>> endings;
  std::map<uint32_t, const FunctionNode*> active;
  std::unordered_set<uint32_t> absorbed;
  for (const auto* node : ordered) {
    const uint32_t addr = node->base();
    while (!endings.empty() && endings.top().first <= addr) {
      active.erase(endings.top().second);
      endings.pop();
    }

    if (node->authority() == FunctionAuthority::GAP_FILL) {
      for (const auto& [otherAddr, otherNode] : active) {
        if (!otherNode->containsAddress(addr))
          continue;
        if (otherNode->authority() != FunctionAuthority::GAP_FILL || otherAddr < addr) {
          absorbed.insert(addr);
          break;
        }
      }
    }

    // Match uint32_t wrap behavior: wrapped/empty bounds contain no address.
    // Even an absorbed owner remains visible until the final batch removal.
    const uint32_t end = node->end();
    if (end > addr) {
      active.emplace(addr, node);
      endings.emplace(end, addr);
    }
  }

  // Preserve the original removal order, independent of the sorted sweep.
  std::vector<uint32_t> toRemove;
  toRemove.reserve(absorbed.size());
  for (const auto& [addr, node] : graph.functions()) {
    if (absorbed.contains(addr)) {
      toRemove.push_back(addr);
    }
  }

  for (uint32_t addr : toRemove) {
    graph.removeFunction(addr);
  }

  if (!toRemove.empty()) {
    REXCODEGEN_TRACE("Analyze: removed {} absorbed GAP_FILL functions", toRemove.size());
  }
}

}  // anonymous namespace

namespace phases {

VoidResult GapFill(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  gapFillCodeRegions(ctx);

  // Discover blocks for gap-filled functions
  auto known = buildKnownFunctions(ctx.graph, /*excludeGapFill=*/true);
  size_t discovered = discoverPendingFunctions(ctx, known);
  REXCODEGEN_TRACE("Analyze: discovered blocks for {} gap-filled functions", discovered);

  cleanupAbsorbedGapFills(ctx);

  return Ok();
}

}  // namespace phases

}  // namespace rex::codegen
