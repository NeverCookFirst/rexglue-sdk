"""Check actual SIMDe streaming stores, optionally including Windows Clang IR."""

import argparse
from pathlib import Path
import re
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--compiler", default="clang++")
parser.add_argument("--windows-sysroot", type=Path,
                    help="Also compile the real header for Windows and verify its non-temporal i64 store")
args = parser.parse_args()
sdk = Path(__file__).resolve().parents[2]
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
source = output / "simde-stream.cpp"
binary = output / "simde-stream.exe"
source.write_text(r'''
#define SIMDE_X86_MMX_NO_NATIVE
#include <simde/x86/sse.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#if !defined(SIMDE_X86_MMX_USE_NATIVE_TYPE) || defined(SIMDE_X86_MMX_NATIVE)
#error The regression must exercise the native m64 type with the fallback streaming store.
#endif
extern "C" void stream(simde__m64* destination, simde__m64 value) {
  simde_mm_stream_pi(destination, value);
}
int main() {
  struct alignas(64) Destination { uint64_t before; simde__m64 value; uint64_t after; };
  static_assert(sizeof(simde__m64) == sizeof(uint64_t));
  constexpr uint64_t patterns[] = {0, UINT64_MAX, 1, UINT64_C(0x8000000000000000),
      UINT64_C(0x0123456789ABCDEF), UINT64_C(0xFEDCBA9876543210),
      UINT64_C(0x7FF80000FFFFFFFF), UINT64_C(0xFFFFFFFF7FC00000)};
  auto check = [](uint64_t bits) {
    Destination destination{};
    destination.before = UINT64_C(0xDEADBEEF12345678);
    destination.after = UINT64_C(0x8877665544332211);
    simde__m64 value;
    std::memcpy(&value, &bits, sizeof(value));
    stream(&destination.value, value);
    simde_mm_sfence();
    uint64_t result;
    std::memcpy(&result, &destination.value, sizeof(result));
    assert(result == bits);
    assert(destination.before == UINT64_C(0xDEADBEEF12345678));
    assert(destination.after == UINT64_C(0x8877665544332211));
  };
  for (uint64_t bits : patterns) check(bits);
  uint64_t random = UINT64_C(0xE12CD327901A9EAB);
  for (unsigned i = 0; i != 2048; ++i) {
    random ^= random << 13; random ^= random >> 7; random ^= random << 17;
    check(random);
  }
}
''')
include = sdk / "thirdparty/simde"
subprocess.run([args.compiler, "-std=c++20", "-O2", "-UNDEBUG", "-I", str(include),
                str(source), "-o", str(binary)], check=True, timeout=30)
subprocess.run([str(binary)], check=True, timeout=10)
print("PASS: actual SIMDe fallback stream_pi preserves 2056 bit patterns and adjacent canaries")

if args.windows_sysroot:
    sysroot = args.windows_sysroot.resolve()
    windows_source = output / "simde-stream-windows.cpp"
    windows_source.write_text(
        '#include <simde/x86/sse.h>\n'
        'extern "C" void stream(simde__m64* destination, simde__m64 value) {\n'
        '  simde_mm_stream_pi(destination, value);\n}\n')
    llvm = output / "simde-stream-windows.ll"
    command = [args.compiler, "--target=x86_64-pc-windows-msvc", "-std=c++20", "-O2",
               "-march=x86-64-v3", "-fms-compatibility-version=19.40", "-I", str(include)]
    for directory in ("crt/include", "sdk/include/ucrt", "sdk/include/shared", "sdk/include/um"):
        command += ["-isystem", str(sysroot / directory)]
    command += ["-S", "-emit-llvm", str(windows_source), "-o", str(llvm)]
    subprocess.run(command, check=True, timeout=30)
    ir = llvm.read_text()
    function = re.search(r"define [^\n]*@stream\([^\n]*\).*?\n\}", ir, re.S)
    if not function or not re.search(r"store i64 [^\n]*align 8, !nontemporal", function[0]):
        raise AssertionError("Windows native union store must preserve all 64 bits and the non-temporal hint")
    print("PASS: actual Windows SIMDe header emits one non-temporal aligned i64 store")
