"""Exercise graphics teardown with a real worker inside production vblank bodies."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--source", type=Path)
    parser.add_argument("--legacy-order", action="store_true")
    args = parser.parse_args()
    sdk = Path(__file__).resolve().parents[2]
    path = args.source or sdk / "src/graphics/graphics_system.cpp"
    source = path.read_text()
    shutdown = function(source, "void GraphicsSystem::Shutdown()")
    if args.legacy_order:
        # Reverse only the two actual production teardown blocks. Detect the
        # lifetime violation before allowing a destructor or stale dereference.
        worker = function(shutdown, "if (vsync_worker_thread_)")
        processor = function(shutdown, "if (command_processor_)")
        assert shutdown.index(worker) < shutdown.index(processor)
        shutdown = shutdown.replace(worker, "WORKER_BLOCK", 1)
        shutdown = shutdown.replace(processor, worker, 1)
        shutdown = shutdown.replace("WORKER_BLOCK", processor, 1)
    bodies = shutdown + "\n" + "\n".join(function(source, signature) for signature in (
        "void GraphicsSystem::MarkVblank()",
        "void GraphicsSystem::DispatchInterruptCallback("))
    code = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#define assert_not_null(x) assert(x)
namespace rex {
template<class T, size_t N> constexpr size_t countof(T (&)[N]) { return N; }
}
namespace rex::system {
struct XThread {
  uint32_t cpu=0;
  static XThread* GetCurrentThread() { static thread_local XThread thread; return &thread; }
  void SetActiveCpu(uint32_t value) { cpu=value; }
  void* thread_state() { return this; }
};
}
namespace rex::graphics {
struct State {
  std::mutex mutex;
  std::condition_variable cv;
  bool active=false, entered=false, permit=false, block_counter=false;
  bool cp_stopped=false, cp_destroyed=false, worker_joined=false;
  unsigned counter=0, callbacks=0, waits=0, ui_calls=0;
  std::vector<std::string> order;
  void Park() {
    std::unique_lock lock(mutex); entered=true; cv.notify_all();
    assert(cv.wait_for(lock,std::chrono::seconds(2),[&]{return permit;}));
  }
  void AwaitEntered() {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock,std::chrono::seconds(2),[&]{return entered;}));
  }
  void Permit() {
    { std::lock_guard lock(mutex); permit=true; } cv.notify_all();
  }
};
struct Processor {
  State& state;
  explicit Processor(State& s):state(s){}
  void increment_counter() {
    assert(!state.cp_stopped && !state.cp_destroyed);
    if(state.block_counter)state.Park();
    assert(!state.cp_stopped && !state.cp_destroyed); ++state.counter;
  }
  void Shutdown() {
    if(state.active)throw std::runtime_error("vblank_processor_lifetime");
    assert(!state.cp_stopped); state.cp_stopped=true; state.order.push_back("cp_stop");
  }
  ~Processor() { state.cp_destroyed=true; state.order.push_back("cp_destroy"); }
};
struct Worker {
  State& state; std::atomic<bool>& running; std::thread thread;
  Worker(State& s,std::atomic<bool>& r,std::function<void()> body):state(s),running(r) {
    { std::lock_guard lock(state.mutex); state.active=true; }
    thread=std::thread([this,body]{
      body(); assert(!running.load());
      { std::lock_guard lock(state.mutex); state.active=false; } state.cv.notify_all();
    });
  }
  void Wait(int a,int b,int c,void* timeout) {
    assert(a==0 && b==0 && c==0 && !timeout && !running.load());
    ++state.waits; state.Permit(); thread.join(); state.worker_joined=true;
    state.order.push_back("worker_join");
  }
  ~Worker() {
    // Also clean up the deliberate legacy failure without leaving a test
    // worker behind; this path is not a replacement production teardown.
    if(thread.joinable()){running=false;state.Permit();thread.join();}
    state.order.push_back("worker_destroy");
  }
};
struct Resource {
  State& state; const char* name;
  Resource(State& s,const char* n):state(s),name(n){}
  ~Resource(){state.order.push_back(name);}
};
struct Context {
  State& state;
  template<class Callback>void CallInUIThreadSynchronous(Callback cb) {
    ++state.ui_calls; cb();
  }
};
struct GraphicsSystem;
struct Dispatcher {
  State& state; GraphicsSystem* owner=nullptr;
  void ExecuteInterrupt(void*,uint32_t,uint64_t[],size_t);
};
struct GraphicsSystem {
  State& state;
  std::unique_ptr<Processor> command_processor_;
  std::atomic<bool> vsync_worker_running_{false};
  std::unique_ptr<Worker> vsync_worker_thread_;
  std::unique_ptr<Resource> presenter_,provider_;
  Context* app_context_=nullptr;
  std::atomic<uint64_t> interrupt_callback_state_{(uint64_t(0x82000000)<<32)|0x12345678};
  Dispatcher* function_dispatcher_;
  GraphicsSystem(State& s,Dispatcher& d):state(s),function_dispatcher_(&d){d.owner=this;}
  void Shutdown(); void MarkVblank(); void DispatchInterruptCallback(uint32_t,uint32_t);
};
void Dispatcher::ExecuteInterrupt(void* thread,uint32_t callback,uint64_t args[],size_t count) {
  assert(thread && callback==0x82000000 && count==2 && args[0]==0 && args[1]==0x12345678);
  assert(static_cast<system::XThread*>(thread)->cpu==2);
  if(!state.block_counter)state.Park();
  assert(owner->command_processor_ && !state.cp_stopped && !state.cp_destroyed);
  // A final callback can need the command processor to remain operational.
  owner->command_processor_->increment_counter(); ++state.callbacks;
}
''' + bodies + r'''
} // namespace rex::graphics
using namespace rex::graphics;
int main() {
 try {
  for(bool block_counter:{true,false})for(bool use_ui:{true,false}) {
    State state;state.block_counter=block_counter;Dispatcher dispatcher{state};
    GraphicsSystem graphics(state,dispatcher);Context context{state};
    graphics.app_context_=use_ui?&context:nullptr;
    graphics.command_processor_=std::make_unique<Processor>(state);
    graphics.presenter_=std::make_unique<Resource>(state,"presenter_destroy");
    graphics.provider_=std::make_unique<Resource>(state,"provider_destroy");
    graphics.vsync_worker_running_=true;
    graphics.vsync_worker_thread_=std::make_unique<Worker>(state,graphics.vsync_worker_running_,
      [&]{graphics.MarkVblank();});
    state.AwaitEntered(); graphics.Shutdown();
    assert(state.counter==2 && state.callbacks==1 && state.waits==1 && state.worker_joined);
    assert(state.ui_calls==unsigned(use_ui) && !graphics.vsync_worker_running_);
    assert((state.order==std::vector<std::string>{"worker_join","worker_destroy","cp_stop",
      "cp_destroy","presenter_destroy","provider_destroy"}));
    const auto order=state.order;graphics.Shutdown();assert(state.order==order && state.waits==1);
  }
  // Empty and partial setup remain safe; a missing worker does not require a wait.
  for(bool cp:{false,true}) {
    State state;Dispatcher dispatcher{state};GraphicsSystem graphics(state,dispatcher);
    if(cp)graphics.command_processor_=std::make_unique<Processor>(state);
    graphics.Shutdown();graphics.Shutdown();assert(!state.waits);
    assert(state.cp_stopped==cp && state.cp_destroyed==cp);
  }
  std::cout<<"PASS: in-flight vblank/callback joins before processor teardown; partial and repeated shutdown\n";
 } catch(const std::exception& error) {
  std::cerr<<error.what()<<'\n'; return 17;
 }
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    cpp = args.output.resolve() / "graphics-shutdown.cpp"
    exe = args.output.resolve() / "graphics-shutdown"
    cpp.write_text(code)
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG", "-pthread", str(cpp), "-o", str(exe)],
                   check=True, timeout=45)
    result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=10)
    if args.legacy_order:
        assert result.returncode == 17 and "vblank_processor_lifetime" in result.stderr, result
    else:
        assert result.returncode == 0, result
    report = {"source": str(path.resolve()),
              "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
              "fixture_cpp_sha256": hashlib.sha256(code.encode()).hexdigest(),
              "legacy_order": args.legacy_order,
              "returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr,
              "boundary": "Real worker and actual teardown/vblank/dispatch bodies; XHostThread wait and guest callback are boundary mocks."}
    (args.output / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS: legacy ordering rejected by lifetime oracle" if args.legacy_order else result.stdout.strip())


if __name__ == "__main__":
    main()
