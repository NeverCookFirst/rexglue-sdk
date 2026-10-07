"""Exercise actual TimerQueue closure, callback ingress and lifetime boundaries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
p.add_argument('--sdk', type=Path)
p.add_argument('--legacy-source', type=Path,
               help='Also require exact old full-ring deadlocks from this saved source')
a = p.parse_args()
sdk = (a.sdk or Path(__file__).resolve().parents[2]).resolve()
a.output.mkdir(parents=True, exist_ok=True)
# Visibility only: observe the real claim reservation before closing the
# full queue, proving that the producer reached Disruptor's blocking barrier.
# All claim/publish implementation bytes remain unchanged.
claim_header = sdk/'thirdparty/disruptorplus/include/disruptorplus/multi_threaded_claim_strategy.hpp'
claim_original = claim_header.read_text()
claim_overlay = a.output/'visibility/disruptorplus/multi_threaded_claim_strategy.hpp'
claim_overlay.parent.mkdir(parents=True, exist_ok=True)
assert 'private:' in claim_original
claim_overlay.write_text(claim_original.replace('private:', 'public:'))
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
soft_as, hard_as = resource.getrlimit(resource.RLIMIT_AS)
limit = min(3 * 1024**3, soft_as) if soft_as != resource.RLIM_INFINITY else 3 * 1024**3
resource.setrlimit(resource.RLIMIT_AS, (limit, hard_as))
fixture = r'''
using namespace rex::thread;using namespace std::chrono_literals;
using Clock=TimerQueueWaitItem::clock;
void Mark(const char* text){std::cout<<text<<std::endl;}
template<class Future>void Ready(Future& f){if(f.wait_for(2s)!=std::future_status::ready){Mark("SETUP_FAILED: future deadline");std::abort();}f.get();}
struct Gate{std::mutex mutex;std::condition_variable cv;bool open=false;
 void Wait(){std::unique_lock lock(mutex);assert(cv.wait_for(lock,3s,[&]{return open;}));}
 void Open(){std::lock_guard lock(mutex);open=true;cv.notify_all();}};
auto Item(TimerQueue& q,std::function<void(void*)> cb,Clock::time_point due,Clock::duration interval={}){
 return std::make_shared<TimerQueueWaitItem>(std::move(cb),nullptr,&q,due,interval);}
int main(int argc,char** argv){assert(argc==2);const std::string_view test=argv[1];
 if(test=="shutdown-full"){
  auto queue=std::make_unique<TimerQueue>();Gate release;std::promise<void> entered,left;
  auto entry=entered.get_future(),exit=left.get_future();
  queue->QueueTimer(Item(*queue,[&](void*){entered.set_value();release.Wait();Mark("CALLBACK_EXITED");left.set_value();},Clock::now()));
  Ready(entry);assert(queue->consumed_.last_published()==0);
  for(int i=0;i<512;++i)queue->QueueTimer(Item(*queue,[](void*){},Clock::now()+1h));
  assert(queue->consumed_.last_published()==0);Mark("FULL_RING: 512 published, consumed=0, no producers active");
  const auto stop=queue->dispatch_thread_.get_stop_token();
  std::thread destroyer([q=std::move(queue)]()mutable{Mark("DESTRUCTOR_ENTERED");q.reset();Mark("DESTRUCTOR_RETURNED");});
  const auto deadline=Clock::now()+1s;while(!stop.stop_requested()&&Clock::now()<deadline)std::this_thread::yield();
  assert(stop.stop_requested());Mark("STOP_REQUESTED");release.Open();Ready(exit);Mark("CASE_ARMED: shutdown-full");
  destroyer.join();Mark("PASS: shutdown-full");return 0;
 }
 if(test=="reentrant-full"){
  TimerQueue queue;constexpr int count=1537;std::array<std::atomic<int>,count> calls{};
  std::atomic<int> total=0;std::promise<void> finished;auto done=finished.get_future();
  queue.QueueTimer(Item(queue,[&](void*){
   assert(queue.consumed_.last_published()==0);Mark("CALLBACK_ENTERED");
   for(int i=0;i<count;++i){
    if(i==512){assert(queue.consumed_.last_published()==0);Mark("REENTRANT_FULL_RING_BOUNDARY: 512 enqueued, consumed=0");Mark("CASE_ARMED: reentrant-full");}
    const auto due=Clock::now();auto handle=queue.QueueTimer(Item(queue,[&,i,due](void*){
     assert(Clock::now()>=due);assert(++calls[i]==1);if(++total==count)finished.set_value();
    },due));assert(!handle.expired());
   }
   Mark("REENTRANT_CLAIMS_RETURNED");
  },Clock::now()));
  Ready(done);assert(total==count);for(auto& n:calls)assert(n==1);
  Mark("PASS: reentrant-full");return 0;
 }
 if(test=="mixed"){
  TimerQueue queue;Gate release;std::promise<void> entered,finished,full;
  auto entry=entered.get_future(),done=finished.get_future(),filled=full.get_future();
  constexpr int externalCount=600,nestedCount=1100;std::array<std::atomic<int>,externalCount+nestedCount> calls{};
  std::atomic<int> total=0,published=0;std::vector<std::weak_ptr<TimerQueueWaitItem>> canceled;
  queue.QueueTimer(Item(queue,[&](void*){
   entered.set_value();release.Wait();
   for(int i=0;i<nestedCount;++i){
    auto h=queue.QueueTimer(Item(queue,[&,i](void*){assert(++calls[externalCount+i]==1);if(++total==externalCount+nestedCount)finished.set_value();},Clock::now()));assert(!h.expired());
   }
   for(int i=0;i<20;++i){auto h=queue.QueueTimer(Item(queue,[](void*){assert(false&&"canceled deferred timer ran");},Clock::now()));auto strong=h.lock();assert(strong);strong->Disarm();canceled.push_back(h);}
  },Clock::now()));Ready(entry);
  auto producer=std::async(std::launch::async,[&]{
   for(int i=0;i<externalCount;++i){
    // A completed timer may legitimately expire before QueueTimer returns.
    // Exact callback accounting, rather than weak-handle liveness, proves it
    // was accepted while the running consumer drains the producer backlog.
    queue.QueueTimer(Item(queue,[&,i](void*){assert(++calls[i]==1);if(++total==externalCount+nestedCount)finished.set_value();},Clock::now()));
    if(++published==512)full.set_value();
   }
  });Ready(filled);assert(published==512);assert(total==0);release.Open();Ready(producer);Ready(done);
  assert(total==externalCount+nestedCount);for(auto& n:calls)assert(n==1);
  // A barrier callback runs after the canceled deferred nodes have been processed.
  std::promise<void> barrier;auto ended=barrier.get_future();queue.QueueTimer(Item(queue,[&](void*){barrier.set_value();},Clock::now()));Ready(ended);
  for(auto& h:canceled)assert(h.expired());
  // A recurring callback may enqueue new work, then self-disarm without a lock cycle.
  Gate ready;std::promise<void> recurringDone;auto recurDone=recurringDone.get_future();
  std::shared_ptr<TimerQueueWaitItem> recurring;std::atomic<int> repeats=0,nested=0;
  const auto firstDue=Clock::now();recurring=Item(queue,[&](void*){
   ready.Wait();const int n=++repeats;assert(Clock::now()>=firstDue+1ms*(n-1));
   queue.QueueTimer(Item(queue,[&](void*){if(++nested==3)recurringDone.set_value();},Clock::now()));
   if(n==3)recurring->Disarm();
  },firstDue,1ms);queue.QueueTimer(recurring);ready.Open();Ready(recurDone);recurring->Disarm();assert(repeats==3&&nested==3);
  Mark("PASS: mixed");return 0;
 }
 if(test=="closure"){
  // Stop must wake both consumer-publication and producer-claim waits without
  // manufacturing sequence availability. Keep the queue alive until callers exit.
  TimerWaitStrategy strategy;std::atomic<dp::sequence_t> sequence{dp::sequence_t(-1)};
  const std::atomic<dp::sequence_t>* seqs[]{&sequence};
  std::promise<void> waiting;auto started=waiting.get_future();
  auto wait=std::async(std::launch::async,[&]{waiting.set_value();try{strategy.wait_until_published(0,1,seqs);assert(false);}catch(const TimerWaitStrategy::Stopped&){}});
  Ready(started);assert(wait.wait_for(10ms)==std::future_status::timeout);
  strategy.Stop();Ready(wait);assert(sequence==dp::sequence_t(-1));
  for(int kind=0;kind<3;++kind){bool stopped=false;try{
   if(kind==0)strategy.wait_until_published(0,1,seqs);
   else if(kind==1)strategy.wait_until_published(0,1,seqs,1ms);
   else strategy.wait_until_published(0,1,seqs,Clock::time_point::max());
  }catch(const TimerWaitStrategy::Stopped&){stopped=true;}assert(stopped);}
  TimerQueue queue;Gate release;std::promise<void> entered,published;
  auto entry=entered.get_future(),full=published.get_future();
  queue.QueueTimer(Item(queue,[&](void*){entered.set_value();release.Wait();},Clock::now()));Ready(entry);
  for(int i=0;i<512;++i)queue.QueueTimer(Item(queue,[](void*){assert(false);},Clock::now()+1h));
  auto producer=std::async(std::launch::async,[&]{published.set_value();return queue.QueueTimer(Item(queue,[](void*){assert(false);},Clock::now()+1h));});
  Ready(full);
  const auto claimDeadline=Clock::now()+1s;
  while(queue.claim_strategy_.m_nextClaimable.load(std::memory_order_acquire)!=514&&Clock::now()<claimDeadline)std::this_thread::yield();
  assert(queue.claim_strategy_.m_nextClaimable.load(std::memory_order_acquire)==514);
  assert(queue.consumed_.last_published()==0);
  assert(producer.wait_for(0ms)==std::future_status::timeout);
  queue.dispatch_thread_.request_stop();queue.wait_strategy_.Stop();
  assert(producer.wait_for(1s)==std::future_status::ready);assert(producer.get().expired());
  assert(queue.QueueTimer(Item(queue,[](void*){assert(false);},Clock::now())).expired());
  release.Open();queue.dispatch_thread_.join();Mark("PASS: closure");return 0;
 }
 if(test=="multi-producer"){
  TimerQueue queue;constexpr int count=4000;std::array<std::atomic<int>,count> calls{};
  std::atomic<int> total=0;std::promise<void> finished;auto done=finished.get_future();
  std::array<std::future<void>,4> producers;
  for(int p=0;p<4;++p)producers[p]=std::async(std::launch::async,[&,p]{
   for(int j=0;j<count/4;++j){int i=p*(count/4)+j;
    queue.QueueTimer(Item(queue,[&,i](void*){assert(++calls[i]==1);if(++total==count)finished.set_value();},Clock::now()));
   }
  });for(auto& p:producers)Ready(p);Ready(done);assert(total==count);for(auto& n:calls)assert(n==1);
  Mark("PASS: multi-producer");return 0;
 }
 if(test=="idle-shutdown"){
  for(int i=0;i<100;++i){TimerQueue empty;}
  {TimerQueue q;q.QueueTimer(Item(q,[](void*){assert(false);},Clock::now()+1h));}
  Mark("PASS: idle-shutdown");return 0;
 }
 return 3;
}
'''

def group_run(cmd, timeout, env=None):
    child = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, start_new_session=True, env=env)
    timeout_hit = False
    try:
        stdout, stderr = child.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timeout_hit = True
        os.killpg(child.pid, signal.SIGKILL)
        stdout, stderr = child.communicate(timeout=2)
    return child.returncode, stdout, stderr, timeout_hit

def compile_source(path, label, legacy=False):
    original = path.read_text()
    source = original.replace('#include <rex/assert.h>', '''
#include <cassert>
#include <array>
#include <future>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>
#define assert_not_null(value) assert(value)
#define assert_true(value) assert(value)
namespace rex::thread { inline void set_current_thread_name(const char*) {} }
''').replace('#include <rex/thread.h>', '')
    begin = source.index('class TimerQueue {')
    end = source.index('rex::thread::TimerQueue timer_queue_;', begin)
    body = source[begin:end]
    assert body.count('\n private:') == 1
    source = source[:begin]+body.replace('\n private:', '\n public:')+source[end:]
    test = fixture
    if legacy:
        # Only remove the NEW-API-specific closure case; both red oracles compile
        # the exact same legacy/new consumer, destructor and reentrant bodies.
        start = test.index(' if(test=="closure")')
        end = test.index(' if(test=="idle-shutdown")', start)
        test = test[:start]+test[end:]
        # The negative child is killed at2s; its own setup deadline must be
        # later, so a correct legacy deadlock cannot race a setup-timeout abort.
        test = test.replace('wait_for(2s)', 'wait_for(5s)')
    cpp = a.output / (label+'.cpp')
    cpp.write_text(source+'\n'+test)
    exe = (a.output / label).resolve()
    rc, stdout, stderr, timeout = group_run([a.compiler, '-std=c++20', '-O2', '-UNDEBUG', '-pthread',
        '-I'+str(sdk/'include'), '-I'+str(a.output/'visibility'),
        '-I'+str(sdk/'thirdparty/disruptorplus/include'),
        '-I'+str(sdk/'thirdparty/disruptorplus/include/disruptorplus'), str(cpp), '-o', str(exe)], 45)
    (a.output/(label+'-compile.log')).write_text(stdout+stderr)
    if timeout or rc != 0:
        raise RuntimeError(f'{label}: compile/setup failed, no behavioral result claimed')
    return exe, hashlib.sha256(path.read_bytes()).hexdigest()

exe, sha = compile_source(sdk/'src/core/timer_queue.cpp', 'timer-lifecycle')
records=[]
for mode in ('spin', 'blocking'):
    env=os.environ.copy();env.pop('REX_TIMER_WAIT_BLOCKING',None)
    if mode=='blocking':env['REX_TIMER_WAIT_BLOCKING']='1'
    for case in ('shutdown-full','reentrant-full','mixed','closure','idle-shutdown','multi-producer'):
        rc,stdout,stderr,timeout=group_run([str(exe),case], 8, env)
        (a.output/(mode+'-'+case+'.log')).write_text(stdout+stderr)
        if timeout or rc or 'PASS: '+case not in stdout:
            raise RuntimeError(f'{mode}/{case}: expected real accounting/termination, inspect log')
        records.append({'mode':mode,'case':case,'passed':True,'stdout':stdout})
negative=[]
if a.legacy_source:
    old,old_sha=compile_source(a.legacy_source.resolve(),'timer-lifecycle-legacy',True)
    for mode in ('spin','blocking'):
        env=os.environ.copy();env.pop('REX_TIMER_WAIT_BLOCKING',None)
        if mode=='blocking':env['REX_TIMER_WAIT_BLOCKING']='1'
        for case in ('shutdown-full','reentrant-full'):
            rc,stdout,stderr,timeout=group_run([str(old),case],2,env)
            (a.output/('legacy-'+mode+'-'+case+'.log')).write_text(stdout+stderr)
            markers=(['FULL_RING: 512 published, consumed=0, no producers active','DESTRUCTOR_ENTERED','STOP_REQUESTED','CALLBACK_EXITED','CASE_ARMED: shutdown-full'] if case=='shutdown-full' else ['CALLBACK_ENTERED','REENTRANT_FULL_RING_BOUNDARY: 512 enqueued, consumed=0','CASE_ARMED: reentrant-full'])
            if not timeout or not all(s in stdout for s in markers) or 'SETUP_FAILED' in stdout or 'PASS:' in stdout or 'DESTRUCTOR_RETURNED' in stdout or 'REENTRANT_CLAIMS_RETURNED' in stdout:
                raise RuntimeError(f'legacy {mode}/{case}: not exact expected deadlock; timeout alone is not proof')
            negative.append({'mode':mode,'case':case,'expected_deadlock_confirmed':True,'killed_and_reaped':True,'source_sha256':old_sha,'stdout':stdout})
report={'production_sha256':sha,'public_header_sha256':hashlib.sha256((sdk/'include/rex/thread/timer_queue.h').read_bytes()).hexdigest(),'disruptor_claim_header_sha256':hashlib.sha256(claim_header.read_bytes()).hexdigest(),'fixture_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'positive_cases':records,'legacy_negative_controls':negative,'limits':['Actual production TU, public header and Disruptor; assert/thread-name facilities shimmed; TimerQueue and real Disruptor reservation visibility changed only.','No callback drops during running queue; permanent closure returns expired weak handle. Producer callers retain queue lifetime until their calls exit.','Each compiler45s/group kill and reap; positive children8s, legacy children2s;3GiB AS/core0.','Does not prove guest1ms clock freshness, OS wake precision or default promotion.']}
(a.output/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'PASS: {len(records)} actual timer lifecycle cases; {len(negative)} exact legacy deadlock controls')
