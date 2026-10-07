"""Execute actual CodegenWriter output for explicit callable interior entries."""
import argparse
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
