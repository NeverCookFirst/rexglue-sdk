"""Exercise the production ExecuteInterrupt body without game code or SDK builds."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
p.add_argument('--source', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
source_path = a.source or root / 'src/system/function_dispatcher.cpp'
source = source_path.read_text()
start = source.index('uint64_t FunctionDispatcher::ExecuteInterrupt(')
opening = source.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
body = source[start:end]
code = r'''
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#define SCOPE_profile_cpu_f(...) ((void)0)
#define PROFILE_INTERRUPT_DISPATCHED() ((void)0)
#define assert_true(value) assert(value)
namespace memory {
template<class T>T load_and_swap(const uint8_t* bytes){
  static_assert(sizeof(T)==4);T word=0;
  for(unsigned i=0;i<4;++i)word=(word<<8)|bytes[i];return word;
}
template<class T>void store_and_swap(uint8_t* bytes,T word){
  static_assert(sizeof(T)==4);
  for(unsigned i=0;i<4;++i)bytes[i]=uint8_t(word>>(24-8*i));
}
}
struct FixtureMemory {
  std::array<uint8_t,4> pcr{};
  uint8_t* TranslateVirtual(uint32_t address){assert(address==0x1000);return pcr.data();}
};
struct Register {uint64_t u64=0;};
struct Context {Register r3,r4,r5,r6,r7,r13;};
struct ThreadState {Context ctx;Context* context(){return &ctx;}};
struct CriticalRegion {
  bool held=false;
  struct Guard {CriticalRegion& owner;~Guard(){owner.held=false;}};
  Guard Acquire(){assert(!held);held=true;return Guard{*this};}
};
struct FunctionDispatcher {
  FixtureMemory* memory_;
  CriticalRegion global_critical_region_;
  std::array<uint64_t,5> observed{};
  unsigned calls=0;
  explicit FunctionDispatcher(FixtureMemory* memory):memory_(memory){}
  bool Execute(ThreadState* thread,uint32_t address){
    ++calls;assert(global_critical_region_.held);
    assert(memory::load_and_swap<uint32_t>(memory_->pcr.data())==0);
    auto* ctx=thread->context();observed={ctx->r3.u64,ctx->r4.u64,ctx->r5.u64,ctx->r6.u64,ctx->r7.u64};
    if(address==0xDEAD)return false;
    assert(address==0x82000000);ctx->r3.u64=0x123456789ABCDEF0ull;return true;
  }
  uint64_t ExecuteInterrupt(ThreadState*,uint32_t,uint64_t[],size_t);
};
''' + body + r'''
int main(){
  FixtureMemory memory_space;FunctionDispatcher dispatcher(&memory_space);
  uint64_t args[]={0x1111222233334444ull,0x2222333344445555ull,
                   0x3333444455556666ull,0x4444555566667777ull,0x5555666677778888ull};
  for(uint32_t tls:{0u,0x10203040u,0xFFFFFFFFu}){
    for(unsigned count=0;count<=5;++count){
      for(bool success:{false,true}){
        ThreadState thread;thread.ctx.r13.u64=0x1000;
        thread.ctx.r3.u64=3;thread.ctx.r4.u64=4;thread.ctx.r5.u64=5;
        thread.ctx.r6.u64=6;thread.ctx.r7.u64=7;
        memory::store_and_swap<uint32_t>(memory_space.pcr.data(),tls);
        const auto result=dispatcher.ExecuteInterrupt(&thread,
            success?0x82000000u:0xDEADu,args,count);
        assert(result==(success?0x123456789ABCDEF0ull:0xDEADBABEull));
        assert(memory::load_and_swap<uint32_t>(memory_space.pcr.data())==tls);
        assert(!dispatcher.global_critical_region_.held);
        for(unsigned i=0;i<5;++i)assert(dispatcher.observed[i]==(i<count?args[i]:i+3));
      }
    }
  }
  assert(dispatcher.calls==36);
}
'''
a.output.mkdir(parents=True, exist_ok=True)
cpp = a.output.resolve() / 'interrupt-tls.cpp'
exe = a.output.resolve() / 'interrupt-tls.exe'
cpp.write_text(code)
subprocess.run([a.compiler, '-std=c++20', '-UNDEBUG', str(cpp), '-o', str(exe)],
               check=True, timeout=45)
subprocess.run([str(exe)], check=True, timeout=10)
print('PASS: production ExecuteInterrupt restores zero/nonzero TLS after '
      'missing-function failure and successful dispatch; callback sees zero TLS')
