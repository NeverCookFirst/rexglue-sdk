"""Run the actual SDK timer queue and publication strategy without a runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
p.add_argument('--sdk', type=Path)
p.add_argument('--latency', action='store_true',
               help='Also report idle host 1ms recurring deadline lateness and CPU cost')
a = p.parse_args()
sdk = (a.sdk or Path(__file__).resolve().parents[2]).resolve()
a.output.mkdir(parents=True, exist_ok=True)
source = (sdk / 'src/core/timer_queue.cpp').read_text()
# Compile the complete production translation unit, actual public wait-item
# header and actual DisruptorPlus implementation. Only runtime-independent
# assertion/thread-name facilities are shimmed; no queue logic is copied.
source = source.replace('#include <rex/assert.h>', '''
#include <cassert>
#include <future>
#include <iostream>
#include <string_view>
#include <thread>
#include <time.h>
#include <vector>
#define assert_not_null(value) assert(value)
#define assert_true(value) assert(value)
namespace rex::thread { inline void set_current_thread_name(const char*) {} }
''').replace('#include <rex/thread.h>', '')
fixture = r'''
using namespace rex::thread;
using namespace std::chrono_literals;
struct Gate {
 std::mutex mutex;std::condition_variable cv;bool open=false;
 void Wait(){std::unique_lock lock(mutex);assert(cv.wait_for(lock,5s,[&]{return open;}));}
 void Open(){std::lock_guard lock(mutex);open=true;cv.notify_all();}
};
template<class Future> void Complete(Future& f){
 assert(f.wait_for(5s)==std::future_status::ready);f.get();
}
std::shared_ptr<TimerQueueWaitItem> Item(TimerQueue& queue,
 std::function<void(void*)> callback,TimerQueueWaitItem::clock::time_point due,
 TimerQueueWaitItem::clock::duration interval={}){
 return std::make_shared<TimerQueueWaitItem>(std::move(callback),nullptr,&queue,due,interval);
}
int MeasureLatency(){
 using Clock=TimerQueueWaitItem::clock;
 constexpr size_t sampleCount=1000;
 const auto cpuNs=[](clockid_t clock){timespec t{};assert(clock_gettime(clock,&t)==0);
  return int64_t(t.tv_sec)*1000000000+t.tv_nsec;};
 timespec resolution{};assert(clock_getres(CLOCK_MONOTONIC,&resolution)==0);
 const int64_t processStart=cpuNs(CLOCK_PROCESS_CPUTIME_ID);
 const auto wallStart=Clock::now();
 int64_t threadStart=0,threadEnd=0;Clock::time_point first{},last{};
 std::vector<int64_t> lateness; lateness.reserve(sampleCount);
 TimerQueue queue;std::promise<void> finished;auto done=finished.get_future();
 const auto firstDue=Clock::now()+20ms;
 std::shared_ptr<TimerQueueWaitItem> item;
 Gate ready;
 item=Item(queue,[&](void*){
  ready.Wait();const auto now=Clock::now();
  const auto deadline=firstDue+1ms*int64_t(lateness.size());assert(now>=deadline);
  if(lateness.empty()){threadStart=cpuNs(CLOCK_THREAD_CPUTIME_ID);first=now;}
  lateness.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(now-deadline).count());
  if(lateness.size()==sampleCount){
   threadEnd=cpuNs(CLOCK_THREAD_CPUTIME_ID);last=now;
   item->Disarm();finished.set_value();
  }
 },firstDue,1ms);
 queue.QueueTimer(item);ready.Open();Complete(done);item->Disarm();
 const int64_t processElapsed=cpuNs(CLOCK_PROCESS_CPUTIME_ID)-processStart;
 const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-wallStart).count();
 const auto sampleElapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(last-first).count();
 std::cout<<"{\"interval_ns\":1000000,\"samples\":"<<lateness.size()
  <<",\"clock_monotonic_resolution_ns\":"<<int64_t(resolution.tv_sec)*1000000000+resolution.tv_nsec
  <<",\"timer_thread_cpu_ns\":"<<threadEnd-threadStart
  <<",\"process_cpu_ns\":"<<processElapsed<<",\"wall_elapsed_ns\":"<<elapsed
  <<",\"sample_elapsed_ns\":"<<sampleElapsed<<",\"lateness_ns\":[";
 for(size_t i=0;i<lateness.size();++i){if(i)std::cout<<',';std::cout<<lateness[i];}
 std::cout<<"]}\n";return 0;
}
int main(int argc,char** argv){
 if(argc==2&&std::string_view(argv[1])=="--latency")return MeasureLatency();
 using Clock=TimerQueueWaitItem::clock;
 // Exercise all wait-strategy overloads and already-published/expired paths.
 TimerWaitStrategy strategy;
 std::atomic<dp::sequence_t> sequence{dp::sequence_t(-1)};
 const std::atomic<dp::sequence_t>* sequences[]{&sequence};
 assert(dp::difference(strategy.wait_until_published(0,1,sequences,Clock::now()-1s),0)<0);
 assert(dp::difference(strategy.wait_until_published(0,1,sequences,0ms),0)<0);
 sequence.store(0,std::memory_order_release);
 assert(strategy.wait_until_published(0,1,sequences)==0);
 assert(strategy.wait_until_published(0,1,sequences,Clock::now()-1s)==0);
 // Publication can happen before or after predicate entry. Neither may lose
 // its notification; a spurious notification may not satisfy the predicate.
 for(int i=1;i<=100;++i){
  auto waiter=std::async(std::launch::async,[&]{
   return strategy.wait_until_published(i,1,sequences,Clock::time_point::max());
  });
  strategy.signal_all_when_blocking();
  sequence.store(i,std::memory_order_release);strategy.signal_all_when_blocking();
  assert(waiter.wait_for(5s)==std::future_status::ready);assert(waiter.get()==dp::sequence_t(i));
 }
 // Empty-queue shutdown must wake an indefinite publication wait.
 {TimerQueue empty;}
 // A newly queued earlier timer interrupts the existing far-future wait.
 {
  TimerQueue queue;std::atomic<int> farCalls=0;std::promise<void> arrived;
  auto far=Item(queue,[&](void*){++farCalls;},Clock::now()+1h);
  queue.QueueTimer(far);
  auto done=arrived.get_future();const auto due=Clock::now()+5ms;
  queue.QueueTimer(Item(queue,[&](void*){assert(Clock::now()>=due);arrived.set_value();},due));
  Complete(done);far->Disarm();assert(farCalls==0);
 }
 // Callbacks are never invoked early, even when the ring wait times out.
 {
  TimerQueue queue;std::promise<void> arrived;auto done=arrived.get_future();
  const auto due=Clock::now()+10ms;
  queue.QueueTimer(Item(queue,[&](void*){assert(Clock::now()>=due);arrived.set_value();},due));
  Complete(done);
 }
 // A disarmed pending item cannot call back; later live items still complete.
 {
  TimerQueue queue;Gate gate;std::promise<void> entered,finished;
  auto entry=entered.get_future(),done=finished.get_future();std::atomic<int> canceledCalls=0;
  queue.QueueTimer(Item(queue,[&](void*){entered.set_value();gate.Wait();},Clock::now()));
  Complete(entry);
  auto canceled=Item(queue,[&](void*){++canceledCalls;},Clock::now());canceled->Disarm();
  queue.QueueTimer(canceled);
  queue.QueueTimer(Item(queue,[&](void*){finished.set_value();},Clock::now()));
  gate.Open();Complete(done);assert(canceledCalls==0);
 }
 // Full producer ring: hold the consumer in a real callback, publish exactly
 // 512 more items, then release it. All 600 callbacks must arrive exactly once.
 {
  TimerQueue queue;Gate gate;std::promise<void> entered,full,finished;
  auto entry=entered.get_future(),filled=full.get_future(),done=finished.get_future();
  std::atomic<int> calls=0,published=0;
  queue.QueueTimer(Item(queue,[&](void*){entered.set_value();gate.Wait();},Clock::now()));
  Complete(entry);
  auto producer=std::async(std::launch::async,[&]{
   for(int i=0;i<600;++i){
    queue.QueueTimer(Item(queue,[&](void*){if(++calls==600)finished.set_value();},Clock::now()));
    ++published;if(i==511)full.set_value();
   }
  });
  Complete(filled);assert(published==512);assert(calls==0);
  gate.Open();Complete(producer);Complete(done);assert(calls==600&&published==600);
 }
 // Recurrence keeps its due-time sequence and self-disarm ends the callback
 // without deadlocking. The gate safely publishes the weak handle first.
 {
  Gate ready;std::promise<void> finished;auto done=finished.get_future();
  std::weak_ptr<TimerQueueWaitItem> recurring;std::atomic<int> calls=0;
  const auto firstDue=Clock::now();
  recurring=QueueTimerRecurring([&](void*){
   ready.Wait();const int n=++calls;assert(Clock::now()>=firstDue+2ms*(n-1));
   if(n==3){auto item=recurring.lock();assert(item);item->Disarm();finished.set_value();}
  },nullptr,firstDue,2ms);
  ready.Open();Complete(done);
  if(auto item=recurring.lock())item->Disarm();assert(calls==3);
 }
 // External Disarm blocks until the callback exits, preserving owner lifetime.
 {
  Gate release;std::promise<void> entered,started;auto entry=entered.get_future(),start=started.get_future();
  auto handle=QueueTimerOnce([&](void*){entered.set_value();release.Wait();},nullptr,Clock::now());
  Complete(entry);auto item=handle.lock();assert(item);
  auto canceled=std::async(std::launch::async,[&]{started.set_value();item->Disarm();});
  Complete(start);assert(canceled.wait_for(0ms)==std::future_status::timeout);
  release.Open();Complete(canceled);
 }
}
'''
cpp = a.output / 'timer-wait.cpp'
cpp.write_text(source + '\n' + fixture)
exe = (a.output / 'timer-wait').resolve()
subprocess.run([a.compiler, '-std=c++20', '-O2', '-pthread',
                '-I'+str(sdk/'include'),
                '-I'+str(sdk/'thirdparty/disruptorplus/include'),
                str(cpp), '-o', str(exe)], check=True, timeout=45)
for value in (None, '0', ''):
    env = os.environ.copy()
    env.pop('REX_TIMER_WAIT_BLOCKING', None)
    if value is not None:
        env['REX_TIMER_WAIT_BLOCKING'] = value
    subprocess.run([str(exe)], env=env, check=True, timeout=15)
    print(f'PASS: actual timer queue with REX_TIMER_WAIT_BLOCKING={value!r}')
print('PASS: publication, deadlines, empty shutdown, earlier insertion, backpressure, recurrence, disarm and completion')
if a.latency:
    modes = {}
    for mode, value in [('spin', None), ('blocking', '1')]:
        env = os.environ.copy()
        env.pop('REX_TIMER_WAIT_BLOCKING', None)
        if value is not None:
            env['REX_TIMER_WAIT_BLOCKING'] = value
        record = json.loads(subprocess.check_output(
            [str(exe), '--latency'], env=env, text=True, timeout=5))
        samples = sorted(record['lateness_ns'])
        assert len(samples) == 1000 and samples[0] >= 0
        record['lateness_us'] = {
            'min': samples[0]/1000, 'mean': sum(samples)/len(samples)/1000,
            'p50': samples[(len(samples)-1)//2]/1000,
            'p95': samples[int((len(samples)-1)*.95)]/1000,
            'p99': samples[int((len(samples)-1)*.99)]/1000,
            'max': samples[-1]/1000,
        }
        record['late_over_1ms_count'] = sum(n > 1000000 for n in samples)
        record['timer_thread_cpu_percent'] = 100*record['timer_thread_cpu_ns']/record['sample_elapsed_ns']
        record['process_cpu_percent'] = 100*record['process_cpu_ns']/record['wall_elapsed_ns']
        modes[mode] = record
        print(json.dumps({'mode': mode, 'lateness_us': record['lateness_us'],
                          'timer_thread_cpu_percent': record['timer_thread_cpu_percent'],
                          'late_over_1ms_count': record['late_over_1ms_count']}))
    report = {'status': 'measured_actual_production_timer_queue',
              'platform': platform.platform(), 'nice': os.getpriority(os.PRIO_PROCESS, 0),
              'cpu_affinity': sorted(os.sched_getaffinity(0)),
              'production_source_sha256': hashlib.sha256((sdk/'src/core/timer_queue.cpp').read_bytes()).hexdigest(),
              'fixture_exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'percentile_method': 'lower order statistic: floor((n-1)*q)',
              'limits': ['Idle native Linux host fixture; Windows UCRT/Proton CV behavior unmeasured',
                         'One 1000-callback run per mode, pinned to one CPU at nice10; not a gameplay/FPS benchmark',
                         'Clock resolution is nominal clock_getres precision, not scheduler wakeup resolution',
                         'Timer CPU spans first-to-last callback; process CPU also includes the production global empty queue'],
              'modes': modes}
    path = a.output / 'timer-latency.json'
    path.write_text(json.dumps(report, indent=2)+'\n')
    print('Latency report:', path)
