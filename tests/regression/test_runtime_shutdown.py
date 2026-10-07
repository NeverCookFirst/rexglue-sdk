"""Keep real final-worker indirect resolution valid through production shutdown."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    parser.add_argument('--legacy-order', action='store_true')
    args = parser.parse_args()
    sdk = Path(__file__).resolve().parents[2]
    source = (sdk/'src/system/runtime.cpp').read_text()
    dispatcher = (sdk/'src/system/function_dispatcher.cpp').read_text()
    shutdown = function(source, 'void Runtime::Shutdown()')
    if args.legacy_order:
        clear = function(shutdown, 'if (instance_ == this)')
        assert shutdown.index(clear) > shutdown.index('kernel_state_.reset();')
        shutdown = shutdown.replace(clear, '', 1)
        shutdown = shutdown.replace('  if (graphics_system_) {', clear+'\n  if (graphics_system_) {', 1)
    getters = function(source, 'Runtime* Runtime::instance()')
    bound = function(dispatcher, 'FunctionDispatcher* GetBoundFunctionDispatcher()')
    resolve = function(dispatcher, 'PPCFunc* ResolveIndirectFunction(')
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
namespace rex {
using PPCFunc=void();
struct State {
 std::mutex mutex;std::condition_variable cv;
 bool entered=false,permit=false;unsigned callbacks=0,profiler_stops=0;
 std::atomic<bool> resolution_failed{false};std::vector<std::string> events;
 void Event(const char* text){events.push_back(text);}
 void Require(bool value,const char* name){if(!value)throw std::runtime_error(name);}
};
State* active_state;
void Guest(){++active_state->callbacks;}
namespace runtime {
struct FunctionDispatcher {
 State& state;bool must_be_unpublished;
 FunctionDispatcher(State& s,bool owned=true):state(s),must_be_unpublished(owned){}
 PPCFunc* GetFunction(uint32_t address){return address==0x82000000?&Guest:nullptr;}
 ~FunctionDispatcher();
};
void InvalidFunctionTrap(){}
PPCFunc* ResolveIndirectFunction(uint32_t);
}
class Runtime;
struct Resource {
 State& state;const char* name;bool must_be_unpublished;
 Resource(State& s,const char* n,bool owned=true):state(s),name(n),must_be_unpublished(owned){}
 ~Resource();
};
struct Service {
 State& state;const char* name;bool fail_shutdown=false;
 std::thread worker;
 Service(State& s,const char* n,bool start_worker=false);
 void Shutdown();void Join();
 ~Service(){Join();state.Event(name);}
};
struct Kernel {
 State& state;std::unique_ptr<Service> worker;
 Kernel(State& s,bool start_worker=false):state(s){
  if(start_worker)worker=std::make_unique<Service>(s,"kernel_worker_destroy",true);
 }
 ~Kernel(){if(worker)worker->Join();state.Event("kernel_destroy");}
};
class Runtime {
public:
 static Runtime* instance_;bool setup_complete_=false;
 std::unique_ptr<Service> graphics_system_,audio_system_,input_system_;
 std::unique_ptr<Kernel> kernel_state_;
 std::unique_ptr<runtime::FunctionDispatcher> function_dispatcher_;
 std::unique_ptr<Resource> export_resolver_,file_system_,memory_;
 static Runtime* instance();void Shutdown();
 runtime::FunctionDispatcher* function_dispatcher(){return function_dispatcher_.get();}
};
Runtime* Runtime::instance_=nullptr;
namespace perf {struct Profiler{static void Shutdown(){++active_state->profiler_stops;active_state->Event("profiler_stop");}};}
''' + getters + '\nnamespace runtime {\n' + bound + '\n' + resolve + r'''
FunctionDispatcher::~FunctionDispatcher(){
 state.Require(!must_be_unpublished||!Runtime::instance(),"dispatcher_destroyed_while_published");
 state.Event("dispatcher_destroy");
}
} // runtime
Resource::~Resource(){
 state.Require(!must_be_unpublished||!Runtime::instance(),"backing_resource_destroyed_while_published");state.Event(name);
}
Service::Service(State& s,const char* n,bool start_worker):state(s),name(n){
 if(!start_worker)return;
 worker=std::thread([this]{
  {std::unique_lock lock(state.mutex);state.entered=true;state.cv.notify_all();
   if(!state.cv.wait_for(lock,std::chrono::seconds(2),[&]{return state.permit;})){
    state.resolution_failed=true;return;
   }}
  // Actual global-bound resolver is called after the service's stop begins.
  auto* owner=Runtime::instance();
  PPCFunc* target=runtime::ResolveIndirectFunction(0x82000000);
  if(!owner||!owner->memory_||target!=&Guest)state.resolution_failed=true;
  else target();
 });
 std::unique_lock lock(state.mutex);
 state.Require(state.cv.wait_for(lock,std::chrono::seconds(2),[&]{return state.entered;}),"worker_setup_timeout");
}
void Service::Join(){
 if(!worker.joinable())return;
 {std::lock_guard lock(state.mutex);state.permit=true;}state.cv.notify_all();worker.join();
}
void Service::Shutdown(){
 state.Event(name);
 if(fail_shutdown)throw std::runtime_error("service_shutdown_fixture_failure");
 Join();state.Require(!state.resolution_failed,"final_worker_indirect_resolution");
}
''' + shutdown + r'''
} // rex
using namespace rex;
void Populate(Runtime& owner,State& state,int worker_kind){
 active_state=&state;Runtime::instance_=&owner;owner.setup_complete_=true;
 owner.function_dispatcher_=std::make_unique<runtime::FunctionDispatcher>(state);
 owner.memory_=std::make_unique<Resource>(state,"memory_destroy");
 owner.export_resolver_=std::make_unique<Resource>(state,"resolver_destroy");
 owner.file_system_=std::make_unique<Resource>(state,"filesystem_destroy");
 owner.graphics_system_=std::make_unique<Service>(state,"graphics",worker_kind==0);
 owner.audio_system_=std::make_unique<Service>(state,"audio",worker_kind==1);
 owner.input_system_=std::make_unique<Service>(state,"input");
 owner.kernel_state_=std::make_unique<Kernel>(state,worker_kind==2);
}
int main(){try{
 // Each joining lifecycle boundary retains the actual indirect resolver.
 for(int worker_kind=0;worker_kind<3;++worker_kind){
  State state;Runtime owner;Populate(owner,state,worker_kind);owner.Shutdown();
  state.Require(!state.resolution_failed&&state.callbacks==1,"final_worker_indirect_resolution");
  assert(!Runtime::instance_&&!owner.setup_complete_&&!owner.memory_&&!owner.function_dispatcher_);
  std::vector<std::string> expected{"graphics","graphics","audio","audio","input","input","kernel_destroy"};
  if(worker_kind==2)expected.push_back("kernel_worker_destroy");
  for(const auto* event:{"dispatcher_destroy","resolver_destroy","filesystem_destroy","memory_destroy","profiler_stop"})
   expected.push_back(event);
  assert(state.events==expected);
  const auto events=state.events;owner.Shutdown();assert(state.events==events&&state.profiler_stops==1);
 }
 // Setup failure may have published the runtime before any backing object exists.
 for(bool memory:{false,true}){
  State state;active_state=&state;Runtime owner;Runtime::instance_=&owner;
  if(memory)owner.memory_=std::make_unique<Resource>(state,"memory_destroy");
  owner.Shutdown();assert(!Runtime::instance_&&!owner.memory_&&!owner.setup_complete_);
  const auto calls=state.profiler_stops;owner.Shutdown();assert(state.profiler_stops==calls);
 }
 // A service failure preserves publication until a later successful cleanup.
 {
  State state;Runtime owner;Populate(owner,state,-1);owner.graphics_system_->fail_shutdown=true;
  bool failed=false;try{owner.Shutdown();}catch(const std::runtime_error& error){
   failed=std::string(error.what())=="service_shutdown_fixture_failure";}
  assert(failed&&Runtime::instance_==&owner&&owner.memory_&&owner.function_dispatcher_);
  assert(runtime::ResolveIndirectFunction(0x82000000)==&Guest);
  owner.graphics_system_->fail_shutdown=false;owner.Shutdown();assert(!Runtime::instance_);
 }
 // An inactive/foreign runtime must not unpublish another runtime's dispatcher.
 {
  State state;active_state=&state;Runtime live,foreign;Runtime::instance_=&live;
  live.function_dispatcher_=std::make_unique<runtime::FunctionDispatcher>(state,false);
  foreign.Shutdown();assert(Runtime::instance_==&live);
  assert(runtime::ResolveIndirectFunction(0x82000000)==&Guest);Runtime::instance_=nullptr;
 }
 // Completely inactive shutdown remains a no-op.
 {State state;active_state=&state;Runtime owner;owner.Shutdown();assert(state.events.empty());}
 std::cout<<"PASS: production shutdown/final-worker indirect resolution, partial/repeat/failure/foreign ownership\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 17;}}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    cpp = args.output.resolve()/'runtime-shutdown.cpp'
    exe = args.output.resolve()/'runtime-shutdown.exe'
    cpp.write_text(code)
    subprocess.run([args.compiler, '-std=c++20', '-UNDEBUG', '-pthread', str(cpp), '-o', str(exe)], check=True, timeout=45)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    if args.legacy_order:
        assert result.returncode == 17 and result.stderr.strip() == 'final_worker_indirect_resolution', result
    else:
        assert result.returncode == 0, result
    report = dict(passed=True, legacy_order=args.legacy_order,
                  runtime_source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                  dispatcher_source_sha256=hashlib.sha256(dispatcher.encode()).hexdigest(),
                  generated_cpp_sha256=hashlib.sha256(code.encode()).hexdigest(),
                  returncode=result.returncode, stdout=result.stdout, stderr=result.stderr,
                  boundary='Actual Runtime Shutdown and singleton/indirect resolver bodies; real parked final callback thread. Service joins/resource destruction/function-table/memory are boundary mocks; no full SDK/guest teardown proof.')
    (args.output/'verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print('PASS: legacy early unpublish rejected by final_worker_indirect_resolution' if args.legacy_order else result.stdout.strip())


if __name__ == '__main__':
    main()
