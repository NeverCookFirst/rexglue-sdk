"""Execute actual CodegenWriter output for explicit callable interior entries."""
import argparse
import json
from pathlib import Path
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--compiler', default='clang++')
parser.add_argument('--producer', type=Path, required=True)
parser.add_argument('--sdk', type=Path)
args = parser.parse_args()
sdk = args.sdk or Path(__file__).resolve().parents[2]
args.output.mkdir(parents=True, exist_ok=True)
subprocess.run([str(args.producer.resolve()), str(args.output.resolve())],
               check=True, timeout=15)
sealed = args.output / 'generated'
generated = args.output / 'deferred/generated'
sources = sorted(generated.glob('interior_recomp.*.cpp'))
assert sources, 'producer emitted no real recompiled translation units'
for source in sources:
    assert source.read_bytes() == (sealed / source.name).read_bytes(), \
        'safely deferred alias changed the emitted instructions'
combined = '\n'.join(source.read_text() for source in sources)
assert 'goto loc_82000018;' in combined
assert 'goto loc_82000048;' in combined
assert 'REX_FATAL' not in combined, 'body preservation left an unresolved branch'
assert 'branch to 0x82000010 outside function' not in combined
# The isolated entry's conflicting classification must not tail-call its owner.
# Full generated sources and mapping table are compiled unchanged below.
main = args.output / 'execute.cpp'
main.write_text(r'''
#include "interior_init.h"
#include <cstdio>
DECLARE_REX_FUNC(pending_external_snapshot);

PPCFunc* Find(size_t address) {
  for (auto* mapping = PPCFuncMappings; mapping->host; ++mapping)
    if (mapping->guest == address) return mapping->host;
  return nullptr;
}

int main() {
  alignas(32) uint8_t memory[32]{};
  auto normal = Find(0x82000000);
  auto interior = Find(0x82000018);
  auto extraNormal = Find(0x82000040);
  auto extraInterior = Find(0x82000048);
  if (!normal || !interior || normal == interior ||
      !extraNormal || !extraInterior || extraNormal == extraInterior) return 1;
  PPCContext ctx{};
  ctx.r1.u64 = 0x1000;
  normal(ctx, memory);
  if (ctx.r1.u64 != 0x1000 || ctx.r3.u64 != 4 || ctx.r5.u64 != 4 || ctx.ctr.u64 != 0)
    return 2;
  // The caller is already inside the guest frame. The interior entry must
  // preserve r3/CTR, loop backwards, and run the single existing epilogue.
  ctx = {};
  ctx.r1.u64 = 0xFF0;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = 99;
  ctx.ctr.u64 = 3;
  interior(ctx, memory);
  if (ctx.r1.u64 != 0x1000 || ctx.r3.u64 != 3 || ctx.r5.u64 != 3 ||
      ctx.r4.u64 != 99 || ctx.ctr.u64 != 0) return 3;
  ctx = {};
  ctx.r1.u64 = 0xFF0;
  ctx.r3.u64 = 7;
  ctx.ctr.u64 = 2;
  interior(ctx, memory);
  if (ctx.r1.u64 != 0x1000 || ctx.r3.u64 != 8 || ctx.r5.u64 != 1 || ctx.ctr.u64 != 0)
    return 4;
  ctx = {};
  ctx.r1.u64 = 0x1000;
  extraNormal(ctx, memory);
  if (ctx.r1.u64 != 0x1000 || ctx.r6.u64 != 0) return 5;
  ctx = {};
  ctx.r1.u64 = 0xFF0;
  ctx.r6.u64 = 10;
  extraInterior(ctx, memory);
  if (ctx.r1.u64 != 0x1000 || ctx.r6.u64 != 11) return 6;
  ctx = {};
  ctx.r1.u64 = 0xFF0;
  ctx.r3.u64 = 1;
  ctx.r7.u64 = 40;
  pending_external_snapshot(ctx, memory);
  if (ctx.r1.u64 != 0x1000 || ctx.r7.u64 != 41) return 7;
  std::puts("Actual generated normal/interior mappings, backward branches, CTR, calls, extra blocks and epilogues passed");
}
''')
binary = args.output / 'execute'
command = [args.compiler, '-std=c++23', '-O1', '-march=x86-64-v2',
           '-DSPDLOG_FMT_EXTERNAL',
           '-I' + str(generated), '-I' + str(sdk / 'include'),
           '-I' + str(sdk / 'thirdparty/simde'),
           '-I' + str(sdk / 'thirdparty/fmt/include'),
           '-I' + str(sdk / 'thirdparty/spdlog/include'),
           *map(str, sources), str(generated / 'interior_init.cpp'),
           str(args.output / 'external_snapshot.cpp'), str(main),
           '-o', str(binary)]
(args.output / 'compile-command.txt').write_text('\n'.join(command) + '\n')
subprocess.run(command, check=True, timeout=40)
subprocess.run([str(binary.resolve())], check=True, timeout=5)

# Compile the actual registration source unchanged. Strong symbol hooks and
# imports must survive constant pointer relocations just as direct calls did.
registration = args.output / 'registration/generated'
registration_main = args.output / 'registration.cpp'
registration_main.write_text(r'''
#include "interior_init.h"
#include <rex/system/function_dispatcher.h>
#include <cassert>
#include <vector>
#include <utility>

void interior_RegisterFunctions(rex::runtime::IModuleRegistrar*);
REX_EXTERN(registration_hooked) { ctx.r9.u64 = 0xC0DE; }
REX_EXTERN(__imp__registration_import) { ctx.r8.u64 = 77; }

struct Recorder : rex::runtime::IModuleRegistrar {
  std::vector<std::pair<uint32_t, PPCFunc*>> calls;
  bool SetFunction(uint32_t address, PPCFunc* function) override {
    calls.emplace_back(address, function);
    return false; // Every later entry must still be visited.
  }
};
int main() {
  Recorder recorder;
  interior_RegisterFunctions(&recorder);
  const std::vector<std::pair<uint32_t, PPCFunc*>> expected{
    {0x81FFFFF8, __imp__registration_import},
    {0x82000000, sub_82000000}, {0x82000018, sub_82000018},
    {0x82000030, registration_hooked},
    {0x82000040, sub_82000040}, {0x82000048, sub_82000048},
    {0x82000080, xstart}, {0x82000088, sub_82000088}
  };
  assert(recorder.calls == expected); // Below-code non-import omitted; equality included.
  assert(recorder.calls[1].second != recorder.calls[2].second);
  assert(recorder.calls[3].second != __imp__registration_hooked);
  alignas(32) uint8_t memory[32]{};
  PPCContext ctx{};
  recorder.calls[0].second(ctx, memory); assert(ctx.r8.u64 == 77);
  recorder.calls[3].second(ctx, memory); assert(ctx.r9.u64 == 0xC0DE);
  __imp__registration_hooked(ctx, memory); assert(ctx.r9.u64 == 0xC0DE);
  ctx = {}; ctx.r1.u64 = 0xFF0; ctx.ctr.u64 = 3;
  recorder.calls[2].second(ctx, memory);
  assert(ctx.r1.u64 == 0x1000 && ctx.r3.u64 == 3 && ctx.r5.u64 == 3);
}
''')


def execute_registration(directory, project, main):
    sources = sorted(directory.glob(f'{project}_recomp.*.cpp'))
    executable = main.with_suffix('.exe')
    command = [args.compiler, '-std=c++23', '-O1', '-UNDEBUG', '-march=x86-64-v2',
               '-DSPDLOG_FMT_EXTERNAL', '-I' + str(directory),
               '-I' + str(sdk / 'include'), '-I' + str(sdk / 'thirdparty/simde'),
               '-I' + str(sdk / 'thirdparty/fmt/include'),
               '-I' + str(sdk / 'thirdparty/spdlog/include'),
               *map(str, sources), str(directory / f'{project}_init.cpp'),
               str(directory / f'{project}_register.cpp'), str(main), '-o', str(executable)]
    main.with_suffix('.command.txt').write_text('\n'.join(command) + '\n')
    subprocess.run(command, check=True, timeout=45)
    subprocess.run([str(executable.resolve())], check=True, timeout=5)


execute_registration(registration, 'interior', registration_main)
for project, count, dll in [('empty', 0, False), ('single', 1, False), ('dll', 1, True)]:
    small_main = args.output / f'{project}.cpp'
    registration_declaration = ('extern "C" void ReXModule_Register(rex::runtime::IModuleRegistrar*);'
                                if dll else f'void {project}_RegisterFunctions(rex::runtime::IModuleRegistrar*);')
    registration_call = 'ReXModule_Register' if dll else f'{project}_RegisterFunctions'
    small_main.write_text(f'#include "{project}_init.h"\n'
                         '#include <rex/system/function_dispatcher.h>\n#include <cassert>\n' +
                         registration_declaration + r'''
struct Recorder : rex::runtime::IModuleRegistrar {
  unsigned calls = 0;
  bool SetFunction(uint32_t address, PPCFunc* function) override {
    ++calls;assert(address == 0x82000000 && function);return false;
  }
};
''' + f'int main(){{Recorder r;{registration_call}(&r);assert(r.calls=={count});}}\n')
    execute_registration(args.output / project / 'generated', project, small_main)
(args.output / 'registration-verification.json').write_text(json.dumps({
    'passed': True, 'actual_writer_and_registration_sources': True,
    'cases': ['sorted order', 'interior alias execution', 'strong hook relocation',
              'import below code base', 'suppressed non-import below code base',
              'code-base equality', 'false return continuation', 'empty', 'single', 'DLL'],
    'limits': 'Synthetic bytes and mocked registrar; no guest runtime or game execution'
}, indent=2) + '\n')
print('Actual registration tables: order, imports, hooks, aliases, empty/single/DLL passed')
