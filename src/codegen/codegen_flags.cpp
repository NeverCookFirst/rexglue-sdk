/**
 * @file        codegen/codegen_flags.cpp
 * @brief       CVar definitions for the codegen pipeline
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/cvar.h>

// clang-format off

//=============================================================================
// Codegen/Output
//=============================================================================

REXCVAR_DEFINE_UINT32(max_file_size_bytes, 1048576, "Codegen",
                      "Target source file size in bytes; sets how many recomp files a project "
                      "is split into, chosen once and then persisted")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(65536, 67108864);

REXCVAR_DEFINE_UINT32(progress_log_frequency, 100, "Codegen",
                      "Log progress every N functions")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 100000);

//=============================================================================
// Codegen/Analysis
//=============================================================================

REXCVAR_DEFINE_UINT32(max_discovery_iterations, 1000, "Codegen",
                      "Max iterations for function discovery convergence")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 100000);

REXCVAR_DEFINE_UINT32(max_vtable_iterations, 100, "Codegen",
                      "Max iterations for vtable discovery")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 100000);

// The vtable scanner is RTTI-driven: it only reaches a table by first finding a
// Complete Object Locator. Function-pointer tables that carry no RTTI - plain
// dispatch tables, callback arrays, and vtables of classes compiled without
// RTTI - are therefore invisible to it, and every function reachable only
// through one of them ends up as a runtime "invalid or unregistered function"
// fatal. This scan walks the data sections directly and treats any run of
// consecutive aligned words pointing into executable memory as such a table.
// Off by default: it is a heuristic and will claim some false positives.
// Projects opt in on the codegen command line - and it has to be the command
// line, not this default: flipping it here had no measurable effect (164403
// functions vs 164875 with the flag passed explicitly), so the SDK template
// resources/templates/init/rexglue_cmake.inja passes it for generated projects.
REXCVAR_DEFINE_BOOL(pointer_table_scan, false, "Codegen",
                    "Discover functions from non-RTTI arrays of code pointers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Companion to pointer_table_scan for the other half of the problem: function
// addresses that are never stored in a table at all, but built inline in a
// register by a lis/addi or lis/ori pair. The SDK already carries the pass
// (functionPointerScan), it was simply never called.
REXCVAR_DEFINE_BOOL(code_pointer_scan, false, "Codegen",
                    "Discover functions from lis/addi and lis/ori pairs in code")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_UINT32(pointer_table_min_run, 4, "Codegen",
                      "Consecutive code pointers required to treat a run as a table")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(2, 1024);

REXCVAR_DEFINE_UINT32(max_resolve_iterations, 100, "Codegen",
                      "Max iterations for call target resolution")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 100000);

REXCVAR_DEFINE_UINT32(max_eh_states, 100, "Codegen",
                      "Max C++ EH states before rejecting handler")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 100000);

REXCVAR_DEFINE_UINT32(max_eh_try_blocks, 50, "Codegen",
                      "Max try blocks before rejecting handler")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 100000);

REXCVAR_DEFINE_UINT32(max_eh_ip_map_entries, 200, "Codegen",
                      "Max IP-to-state map entries")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 100000);

REXCVAR_DEFINE_UINT32(max_seh_scope_entries, 100, "Codegen",
                      "Max SEH scope table entries")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 100000);

//=============================================================================
// Codegen/Discovery
//=============================================================================

REXCVAR_DEFINE_UINT32(backward_scan_limit, 64, "Codegen",
                      "Max instructions to scan backward for jump table patterns")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 10000);

REXCVAR_DEFINE_UINT32(max_jump_table_entries, 512, "Codegen",
                      "Max entries per detected jump table")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 100000);

REXCVAR_DEFINE_UINT32(max_blocks_per_function, 10000, "Codegen",
                      "Safety limit on blocks per function")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .range(1, 1000000);

// clang-format on
