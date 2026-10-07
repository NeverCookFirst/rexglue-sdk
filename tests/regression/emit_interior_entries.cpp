// Synthetic executable PPC bytes pass through the real config loader, writer,
// decoder, and instruction builders. The companion test runs their C++ output.
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/codegen_writer.h>
#include <rex/codegen/test_support.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace rex::codegen;
namespace fs = std::filesystem;
constexpr uint32_t kBase = 0x82000000;
constexpr uint32_t kEntry = kBase + 0x18;
constexpr uint32_t kExtraOwner = kBase + 0x40;
constexpr uint32_t kExtraEntry = kBase + 0x48;

void Check(bool result, const char* message) {
  if (!result) throw std::runtime_error(message);
}

std::vector<uint8_t> Bytes(bool externalHole = false) {
  std::vector<uint8_t> bytes(0x90);
  auto put = [&](unsigned offset, uint32_t instruction) {
    for (unsigned i = 0; i < 4; ++i)
      bytes[offset + i] = static_cast<uint8_t>(instruction >> (24 - i * 8));
  };
  // A real frame, conditional backward branch, live CTR loop, and owner-only
  // call metadata. The interior entry skips all four prologue instructions.
  put(0x00, 0x3821FFF0);  // addi r1,r1,-16
  put(0x04, 0x38600000);  // li r3,0
  put(0x08, 0x38800003);  // li r4,3
  put(0x0C, 0x7C8903A6);  // mtctr r4
  put(0x10, 0x38630001);  // addi r3,r3,1
  put(0x14, 0x4800006D);  // bl helper at +0x80
  put(0x18, 0x2C030001);  // cmpwi r3,1
  put(0x1C, 0x4182FFF4);  // beq +0x10
  put(0x20, 0x4200FFF0);  // bdnz +0x10
  put(0x24, 0x38210010);  // addi r1,r1,16
  put(0x28, 0x4E800020);  // blr
  if (externalHole) {
    put(0x30, 0x38210010);  // external tail callee restores existing frame
    put(0x34, 0x38E70001);  // addi r7,r7,1
    put(0x38, 0x4E800020);  // blr
  }
  // Normal execution skips the interior-only path. Discovery from the
  // interior entry found a second block and call absent from the owner CFG.
  put(0x40, 0x3821FFF0);  // addi r1,r1,-16
  put(0x44, 0x4800000C);  // b +0x50
  put(0x48, 0x60000000);  // nop (actual entry instruction in owner block)
  put(0x4C, 0x48000014);  // b +0x60 (entry-only discovered instruction)
  put(0x50, 0x38210010);  // addi r1,r1,16
  put(0x54, 0x4E800020);  // blr
  put(0x60, 0x48000029);  // bl helper at +0x88
  put(0x64, 0x48000000 | (static_cast<uint32_t>(-0x14) & 0x03FFFFFC)); // b +0x50
  put(0x80, 0x38A50001);  // addi r5,r5,1
  put(0x84, 0x4E800020);
  put(0x88, 0x38C60001);  // addi r6,r6,1
  put(0x8C, 0x4E800020);
  return bytes;
}

CodegenContext Context(const fs::path& root, RecompilerConfig config,
                       bool missingEntry = false, uint32_t pendingTarget = 0,
                       bool externalHole = false) {
  auto bytes = Bytes(externalHole);
  if (pendingTarget) {
    uint32_t instruction = 0x41820000 | ((pendingTarget - (kBase + 0x1C)) & 0xFFFC);
    for (unsigned i = 0; i < 4; ++i)
      bytes[0x1C + i] = static_cast<uint8_t>(instruction >> (24 - i * 8));
  }
  TestModule module;
  module.Load(kBase, bytes.data(), bytes.size());
  auto ctx = CodegenContext::Create(BinaryView::fromModule(module), std::move(config));
  ctx.setConfigDir(root);
  ctx.analysisState().format = "xex";
  ctx.analysisState().loadAddress = kBase;
  ctx.analysisState().entryPoint = kBase + 0x80;
  ctx.analysisState().imageSize = static_cast<uint32_t>(bytes.size());
  auto add = [&](uint32_t address, uint32_t size, std::vector<Block> blocks) {
    auto* node = ctx.graph.addFunction(address, size, FunctionAuthority::CONFIG, true);
    Check(node != nullptr, "addFunction failed");
    node->discover(std::move(blocks), {}, {});
    return node;
  };
  auto* helper = add(kBase + 0x80, 8, {{kBase + 0x80, 8}});
  ctx.graph.setFunctionName(kBase + 0x80, "xstart");
  auto* extraHelper = add(kBase + 0x88, 8, {{kBase + 0x88, 8}});
  auto* owner = add(kBase, externalHole ? 0x40 : 0x2C, missingEntry
      ? std::vector<Block>{{kBase, 0x18}, {kBase + 0x1C, 0x10}}
      : std::vector<Block>{{kBase, 0x2C}});
  auto* entry = add(kEntry, 0x14, {{kEntry, 0x14}});
  auto* extraOwner = add(kExtraOwner, 0x18,
      {{kExtraOwner, 0xC}, {kBase + 0x50, 8}});
  auto* extraEntry = add(kExtraEntry, 0x20,
      {{kExtraEntry, 8}, {kBase + 0x50, 8}, {kBase + 0x60, 8}});
  ctx.graph.addCallToFunction(kBase, kBase + 0x14, CallTarget::function(helper));
  ctx.graph.addLabelToFunction(kBase, kBase + 0x10);
  ctx.graph.addCallToFunction(kEntry, kBase + 0x14, CallTarget::function(extraHelper));
  ctx.graph.addCallToFunction(kExtraEntry, kBase + 0x60, CallTarget::function(extraHelper));
  // A conflicting classification from isolated-entry analysis must not
  // override the actual owner's backward-branch interpretation.
  ctx.graph.addTailCallToFunction(kEntry, kBase + 0x1C, CallTarget::function(owner));
  if (pendingTarget)
    ctx.graph.addUnresolvedJumpToFunction(kEntry, kBase + 0x1C, pendingTarget, false, true);
  for (auto* node : {helper, extraHelper, owner, entry, extraOwner, extraEntry}) {
    if (node != entry || !pendingTarget) node->seal();
  }
  if (pendingTarget)
    Check(entry->isDiscovered() && !entry->isSealed() && entry->unresolvedJumps().size() == 1,
          "fixture must retain a genuinely unsealed deferred alias branch");
  return ctx;
}

RecompilerConfig Config(const fs::path& root) {
  fs::create_directories(root);
  auto path = root / "fixture.toml";
  std::ofstream(path) << "project_name = 'interior'\nfile_path = 'synthetic.bin'\n"
      "out_directory_path = 'generated'\n"
      "[functions]\n0x82000018 = { body = 0x82000000 }\n"
      "0x82000048 = { body = 0x82000040 }\n";
  RecompilerConfig config;
  Check(config.Load(path.string()), "valid body config rejected");
  Check(config.functions.at(kEntry).bodyOwner == kBase, "body owner lost by parser");
  Check(config.outDirectoryPath == "generated", "output path lost by parser");
  return config;
}

int main(int argc, char** argv) try {
  Check(argc == 2, "expected output directory");
  fs::path root = fs::absolute(argv[1]);
  auto config = Config(root);
  auto ctx = Context(root, config);
  CodegenWriter writer(ctx);
  Check(writer.write(false), "valid body emission failed");

  auto deferred = Context(root / "deferred", config, false, kBase + 0x10);
  CodegenWriter deferredWriter(deferred);
  Check(deferredWriter.write(false), "mapped unsealed interior branch rejected");

  // Capture a pending node before graph notification resolves the external
  // target. Test this stale snapshot directly with the production emitter:
  // normal discovery usually resolves it immediately, but the wrapper must
  // still honor owner metadata instead of inventing a label in a CONFIG hole.
  auto external = Context(root / "external", config, false, kBase + 0x30, true);
  FunctionNode snapshot = *external.graph.getFunction(kEntry);
  snapshot.setName("pending_external_snapshot");
  auto* externalTarget = external.graph.addFunction(kBase + 0x30, 12,
      FunctionAuthority::CONFIG, true);
  externalTarget->discover({{kBase + 0x30, 12}}, {}, {});
  externalTarget->seal();
  external.graph.addCallToFunction(kBase, kBase + 0x1C,
      CallTarget::function(externalTarget));
  const auto* externalOwner = external.graph.getFunction(kBase);
  Check(externalOwner->containsAddress(kBase + 0x30), "fixture must exercise CONFIG hole fallback");
  Check(snapshot.unresolvedJumps().size() == 1, "snapshot lost its pending branch");
  EmitContext emission{external.binary(), external.Config(), external.graph, kBase + 0x80};
  auto code = externalOwner->emitCpp(emission, &snapshot);
  Check(code.find("goto loc_82000030") == std::string::npos,
        "resolved external call was promoted to an un-emitted local label");
  std::ofstream(root / "external_snapshot.cpp")
      << "#include \"interior_pch.h\"\n"
         "DECLARE_REX_FUNC(xstart);\nDECLARE_REX_FUNC(sub_82000030);\n"
      << externalTarget->emitCpp(emission) << code;

  auto reject = [&](RecompilerConfig invalid, const char* name, bool missingEntry = false,
                    uint32_t pendingTarget = 0) {
    auto out = root / name;
    auto bad = Context(out, std::move(invalid), missingEntry, pendingTarget);
    CodegenWriter rejected(bad);
    Check(!rejected.write(true), "invalid body accepted even with force");
    Check(!fs::exists(out / "generated"), "invalid body created output");
  };
  auto invalid = config;
  invalid.functions.at(kEntry).bodyOwner = kBase + 0x30;
  reject(invalid, "missing-owner");
  reject(config, "missing-entry-in-owner", true);
  reject(config, "pending-target-gap", false, kBase + 0x30);
  reject(config, "pending-target-outside-image", false, kBase + 0x94);
  invalid = config;
  invalid.functions.at(kEntry).bodyOwner = kEntry;
  reject(invalid, "self-owner");
  invalid = config;
  invalid.functions[kBase].bodyOwner = kEntry;
  reject(invalid, "cyclic-owner");
  invalid = config;
  invalid.functions.at(kEntry).parent = kBase;
  reject(invalid, "chunk-owner");
  invalid = config;
  invalid.functions.at(kEntry).bodyOwner = kBase + 1;
  reject(invalid, "unaligned-owner");
  invalid = config;
  invalid.rexcrtFunctions["test"] = kEntry;
  reject(invalid, "rexcrt-entry");
  constexpr std::array flags{
      &RecompilerConfig::ctrAsLocalVariable, &RecompilerConfig::xerAsLocalVariable,
      &RecompilerConfig::reservedRegisterAsLocalVariable,
      &RecompilerConfig::crRegistersAsLocalVariables,
      &RecompilerConfig::nonArgumentRegistersAsLocalVariables,
      &RecompilerConfig::nonVolatileRegistersAsLocalVariables};
  unsigned index = 0;
  for (auto flag : flags) {
    invalid = config;
    invalid.*flag = true;
    auto name = "localization-" + std::to_string(index++);
    reject(invalid, name.c_str());
  }
  // Invalid body values must fail config loading, rather than silently discard
  // the override and emit the original lossy isolated function.
  for (const char* value : {"'bad'", "0", "0x82000018", "0x82000001", "-1",
                           "0x100000000"}) {
    auto path = root / "invalid.toml";
    std::ofstream(path) << "file_path = 'synthetic.bin'\n"
        "[functions]\n0x82000018 = { body = " << value << " }\n";
    RecompilerConfig bad;
    Check(!bad.Load(path.string()), "invalid TOML body accepted");
  }
  std::cout << "Real sealed/deferred codegen body emission and 21 rejection cases passed\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
