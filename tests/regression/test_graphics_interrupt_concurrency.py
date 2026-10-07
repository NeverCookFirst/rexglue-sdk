"""Run production counter/publication/dispatch/save bodies with bounded threads."""
import argparse
from pathlib import Path
import re
import subprocess


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
p.add_argument('--sdk', type=Path)
p.add_argument('--tsan', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
sdk = a.sdk or root
graphics = (sdk / 'src/graphics/graphics_system.cpp').read_text()
graphics_header = (sdk / 'include/rex/graphics/graphics_system.h').read_text()
processor_header = (sdk / 'include/rex/graphics/command_processor.h').read_text()
processor = (sdk / 'src/graphics/command_processor.cpp').read_text()
fields = '\n'.join(re.findall(
    r'  (?:std::atomic<uint64_t> interrupt_callback_state_\{0\};|'
    r'uint32_t interrupt_callback_ = 0;|uint32_t interrupt_callback_data_ = 0;)', graphics_header))
counter_field = re.search(
    r'  (?:std::atomic<uint32_t> counter_\{0\};|uint32_t counter_ = 0;)', processor_header).group(0)
counter_methods = '\n'.join(function(processor_header, signature) for signature in (
    'uint32_t counter() const', 'void increment_counter()'))
# Both production command-thread accesses must use the same synchronized
# primitive as the vblank thread. Do not silently test only the public getter.
if 'std::atomic<uint32_t>' in counter_field:
    assert '++counter_;' not in processor
    assert 'data_value = counter();' in processor
    assert 'increment_counter();' in processor
bodies = '\n'.join(function(graphics, signature) for signature in (
    'void GraphicsSystem::SetInterruptCallback(', 'void GraphicsSystem::DispatchInterruptCallback(',
    'bool GraphicsSystem::Save(', 'bool GraphicsSystem::Restore('))
code = r'''
#include <atomic>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>
#define REXGPU_INFO(...) ((void)0)
#define assert_not_null(value) assert(value)
namespace rex {
template<class T,size_t N>constexpr size_t countof(T (&)[N]){return N;}
namespace stream {
struct ByteStream {
  std::vector<uint32_t> words;size_t next=0;
  template<class T>void Write(T word){static_assert(sizeof(T)==4);words.push_back(uint32_t(word));}
  template<class T>T Read(){static_assert(sizeof(T)==4);return T(words.at(next++));}
};
}
}
namespace rex::system {
struct XThread {
  uint32_t cpu=0;
  void SetActiveCpu(uint32_t active){cpu=active;}
  void* thread_state(){return this;}
  static XThread* GetCurrentThread(){static thread_local XThread thread;return &thread;}
};
}
namespace rex::graphics {
struct Processor {
''' + counter_field + '\n' + counter_methods + r'''
  bool Save(rex::stream::ByteStream*){return true;}
  bool Restore(rex::stream::ByteStream*){return true;}
};
struct GraphicsSystem;
struct Dispatcher {
  std::atomic<uint32_t> calls{0};bool reentrant=false;GraphicsSystem* owner=nullptr;
  void ExecuteInterrupt(void* thread,uint32_t callback,uint64_t args[],size_t count);
};
struct GraphicsSystem {
''' + fields + r'''
  Processor processor;Processor* command_processor_=&processor;
  Dispatcher dispatcher;Dispatcher* function_dispatcher_=&dispatcher;
  GraphicsSystem(){dispatcher.owner=this;}
  void SetInterruptCallback(uint32_t,uint32_t);
  void DispatchInterruptCallback(uint32_t,uint32_t);
  bool Save(rex::stream::ByteStream*);
  bool Restore(rex::stream::ByteStream*);
};
''' + bodies + r'''
constexpr uint32_t callback_a=0x83FBF508,callback_b=0x82012340;
constexpr uint32_t data_a=0x40009A00,data_b=0x1234ABCD;
void Dispatcher::ExecuteInterrupt(void* thread,uint32_t callback,uint64_t args[],size_t count){
  assert(thread&&count==2&&args[0]==7);
  assert((callback==callback_a&&args[1]==data_a)||(callback==callback_b&&args[1]==data_b));
  assert(static_cast<system::XThread*>(thread)->cpu==2);
  calls.fetch_add(1,std::memory_order_relaxed);
  if(reentrant)owner->SetInterruptCallback(callback_b,data_b);
}
void CheckSaved(const rex::stream::ByteStream& saved){
  assert(saved.words.size()==2);
  const auto callback=saved.words[0],data=saved.words[1];
  assert((callback==callback_a&&data==data_a)||(callback==callback_b&&data==data_b)||
         (callback==0&&data==0xDEADBEEF));
}
}  // namespace rex::graphics
using namespace rex::graphics;
int main(){
  constexpr uint32_t count=200000;
  Processor processor;
  std::atomic<bool> start{false};
  const auto writer=[&]{while(!start.load(std::memory_order_acquire))std::this_thread::yield();
    for(uint32_t i=0;i<count;++i)processor.increment_counter();};
  std::thread first(writer),second(writer);
  start.store(true,std::memory_order_release);
  for(uint32_t i=0;i<count;++i)assert(processor.counter()<=count*2);
  first.join();second.join();assert(processor.counter()==count*2);
  processor.counter_=UINT32_MAX-1;processor.increment_counter();processor.increment_counter();
  assert(processor.counter()==0);
  GraphicsSystem graphics;graphics.SetInterruptCallback(callback_a,data_a);
  graphics.DispatchInterruptCallback(7,0xFFFFFFFF);
  assert(graphics.dispatcher.calls.load()==1);
  start.store(false,std::memory_order_release);
  std::thread publication([&]{
    while(!start.load(std::memory_order_acquire))std::this_thread::yield();
    for(uint32_t i=0;i<100000;++i){
      graphics.SetInterruptCallback(callback_a,data_a);
      graphics.SetInterruptCallback(callback_b,data_b);
      graphics.SetInterruptCallback(0,0xDEADBEEF);
    }
    graphics.SetInterruptCallback(callback_a,data_a);
  });
  std::thread dispatch([&]{
    while(!start.load(std::memory_order_acquire))std::this_thread::yield();
    for(uint32_t i=0;i<100000;++i)graphics.DispatchInterruptCallback(7,0xFFFFFFFF);
  });
  start.store(true,std::memory_order_release);
  for(uint32_t i=0;i<100000;++i){rex::stream::ByteStream saved;assert(graphics.Save(&saved));CheckSaved(saved);}
  publication.join();dispatch.join();
  assert(graphics.dispatcher.calls.load()>0);
  graphics.SetInterruptCallback(0,0xDEADBEEF);
  const auto calls=graphics.dispatcher.calls.load();graphics.DispatchInterruptCallback(7,2);
  assert(graphics.dispatcher.calls.load()==calls);
  rex::stream::ByteStream saved;saved.words={callback_b,data_b};
  assert(graphics.Restore(&saved));rex::stream::ByteStream restored;
  assert(graphics.Save(&restored));assert(restored.words==saved.words);
  graphics.SetInterruptCallback(callback_a,data_a);graphics.dispatcher.reentrant=true;
  graphics.DispatchInterruptCallback(7,2);rex::stream::ByteStream after;
  assert(graphics.Save(&after));assert(after.words==std::vector<uint32_t>({callback_b,data_b}));
}
'''
a.output.mkdir(parents=True, exist_ok=True)
cpp = a.output.resolve() / 'graphics-interrupt-concurrency.cpp'
exe = a.output.resolve() / 'graphics-interrupt-concurrency.exe'
cpp.write_text(code)
flags = ['-fsanitize=thread', '-g', '-O1'] if a.tsan else []
subprocess.run([a.compiler, '-std=c++20', '-UNDEBUG', '-pthread', *flags,
                str(cpp), '-o', str(exe)], check=True, timeout=45)
subprocess.run([str(exe)], check=True, timeout=15)
print('PASS: production counter and callback publication/dispatch/save/restore '
      'bodies under bounded concurrency; no lost increments or mixed callback pairs')
