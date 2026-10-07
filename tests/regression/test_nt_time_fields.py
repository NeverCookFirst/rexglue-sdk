"""Run real NT calendar entry bodies and chrono conversions across NT's epoch."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess


def block(source, signature):
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
    parser.add_argument('--legacy-clock', action='store_true')
    parser.add_argument('--legacy-recomposition', action='store_true')
    args = parser.parse_args()
    sdk = Path(__file__).resolve().parents[2]
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    source = (sdk/'src/kernel/xboxkrnl/xboxkrnl_rtl.cpp').read_text()
    header = (sdk/'include/rex/chrono/chrono.h').read_text()
    fields = block(source, 'struct X_TIME_FIELDS')+';'
    decompose = block(source, 'void RtlTimeToTimeFields_entry(')
    recompose = block(source, 'u32 RtlTimeFieldsToTime_entry(')
    if args.legacy_recomposition:
        old = 'std::chrono::sys_time<WinSystemClock::duration> time = dp;'
        assert recompose.count(old) == 1
        recompose = recompose.replace(old, 'std::chrono::system_clock::time_point time = dp;')
    if args.legacy_clock:
        signature = 'static constexpr std::chrono::sys_time<duration> to_sys('
        actual = block(header, signature)
        legacy = '''static constexpr std::chrono::system_clock::time_point to_sys(const time_point& tp)
    requires(domain_ == Domain::Host)
  {
    using sys_duration = std::chrono::system_clock::duration;
    using sys_time = std::chrono::system_clock::time_point;
    auto dp = tp;
    dp += unix_epoch_delta();
    auto cdp = std::chrono::time_point_cast<sys_duration>(dp);
    return sys_time{cdp.time_since_epoch()};
  }'''
        header = header.replace(actual, legacy, 1)
    # Include the actual public header, with an isolated header override only in
    # the named negative control. All other SDK headers are used unchanged.
    include = output/'include/rex/chrono'
    include.mkdir(parents=True, exist_ok=True)
    (include/'chrono.h').write_text(header)
    cases = [(1601,1,1,0,0,0,0), (1601,1,1,0,0,0,1), (1900,1,1,0,0,0,0),
             (1969,12,31,23,59,59,999), (1970,1,1,0,0,0,0),
             (2000,2,29,12,34,56,789), (2026,10,7,9,8,7,654),
             (9999,12,31,23,59,59,999)]
    epoch = datetime.datetime(1601,1,1)
    rows = []
    for y,m,d,h,minute,s,ms in cases:
        date = datetime.datetime(y,m,d,h,minute,s,ms*1000)
        delta = date-epoch
        ticks = (delta.days*86400+delta.seconds)*10000000+delta.microseconds*10
        rows.append('{'+','.join(map(str, (y,m,d,h,minute,s,ms,(date.weekday()+1)%7)))+
                    ','+str(ticks)+'ULL}')
    code = r'''
#include <rex/chrono/chrono.h>
#include <rex/types.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
namespace fixture {
using u32=uint32_t;
// Guest-address resolution is the fixture boundary; actual BE storage and all
// production calendar/conversion arithmetic remain intact.
struct mapped_u64 {
 rex::be<uint64_t>* pointer;
 uint64_t value()const{return *pointer;}
 rex::be<uint64_t>& operator*()const{return *pointer;}
};
template<class T>struct ppc_ptr_t {T* pointer;T* operator->()const{return pointer;}};
void Require(bool good,const char* oracle){if(!good){std::fprintf(stderr,"oracle:%s\n",oracle);std::exit(17);}}
''' + fields + '\nstatic_assert(sizeof(X_TIME_FIELDS)==16);\n'+decompose+'\n'+recompose+r'''
} // namespace fixture
using namespace fixture;
struct Case {uint16_t y,m,d,h,minute,s,ms,weekday;uint64_t ticks;};
int main(){
 const Case cases[]={ROWS};
 for(const auto& c:cases){
  X_TIME_FIELDS fields{};fields.year=c.y;fields.month=c.m;fields.day=c.d;
  fields.hour=c.h;fields.minute=c.minute;fields.second=c.s;fields.milliseconds=c.ms;
  rex::be<uint64_t> encoded{uint64_t(42)};
  Require(RtlTimeFieldsToTime_entry({&fields},{&encoded})==1,"valid_calendar_rejected");
  Require(uint64_t(encoded)==c.ticks,"nt_calendar_recomposition");
  const auto* bytes=reinterpret_cast<const unsigned char*>(&encoded);
  for(unsigned i=0;i<8;++i)Require(bytes[i]==((c.ticks>>(56-i*8))&255),"guest_be_ticks");
  fields={};RtlTimeToTimeFields_entry({&encoded},{&fields});
  Require(fields.year==c.y&&fields.month==c.m&&fields.day==c.d&&fields.hour==c.h&&
          fields.minute==c.minute&&fields.second==c.s&&fields.milliseconds==c.ms&&
          fields.weekday==c.weekday,"nt_calendar_decomposition");
 }
 X_TIME_FIELDS fields{};fields.year=2000;fields.month=2;fields.day=29;
 for(unsigned kind=0;kind<8;++kind){
  auto bad=fields;
  switch(kind){case 0:bad.year=1600;break;case 1:bad.month=13;break;case 2:bad.day=30;break;
   case 3:bad.hour=24;break;case 4:bad.minute=60;break;case 5:bad.second=60;break;
   case 6:bad.milliseconds=1000;break;case 7:bad.day=0;break;}
  rex::be<uint64_t> encoded{uint64_t(42)};
  Require(RtlTimeFieldsToTime_entry({&bad},{&encoded})==0,"invalid_calendar_accepted");
  Require(uint64_t(encoded)==42,"invalid_calendar_changed_output");
 }
 std::puts("8 exact NT calendar cases and 8 invalid cases passed");
}
'''.replace('ROWS', ',\n'.join(rows))
    cpp = output/'nt-time-fields.cpp'
    cpp.write_text(code)
    binary = output/'nt-time-fields.exe'
    command = [args.compiler, '-std=c++23', '-O0', '-DREX_PLATFORM_LINUX=1',
               '-I'+str(output/'include'), '-I'+str(sdk/'include'),
               '-I'+str(sdk/'thirdparty/fmt/include'), '-I'+str(sdk/'thirdparty/spdlog/include'),
               str(cpp), '-o', str(binary)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=45)
    (output/'compile.log').write_text(result.stdout+result.stderr)
    if result.returncode:
        raise RuntimeError('NT calendar fixture compile failed: '+result.stderr[-2000:])
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    (output/'run.log').write_text(result.stdout+result.stderr)
    negative = args.legacy_clock or args.legacy_recomposition
    oracle = 'oracle:nt_calendar_decomposition' if args.legacy_clock and not args.legacy_recomposition else 'oracle:nt_calendar_recomposition'
    passed = result.returncode == (17 if negative else 0) and (not negative or oracle in result.stderr)
    report = {'passed':passed, 'legacy_clock':args.legacy_clock,
              'legacy_recomposition':args.legacy_recomposition,'returncode':result.returncode,
              'expected_oracle':oracle if negative else None,'cases':cases,
              'actual_header_sha256':hashlib.sha256((sdk/'include/rex/chrono/chrono.h').read_bytes()).hexdigest(),
              'actual_nt_source_sha256':hashlib.sha256((sdk/'src/kernel/xboxkrnl/xboxkrnl_rtl.cpp').read_bytes()).hexdigest(),
              'limits':'Actual header/entry bodies and BE fields; guest pointer resolution modeled. Dates 1601..9999; not all unsigned FILETIME values or guest clock scaling.'}
    (output/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
    if not passed:
        raise RuntimeError('NT calendar fixture oracle failed: '+result.stdout+result.stderr)
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    main()
