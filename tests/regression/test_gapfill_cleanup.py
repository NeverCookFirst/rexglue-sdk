"""Compare the production cleanup sweep with its former all-graph algorithm."""
import argparse
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--compiler', default='clang++')
parser.add_argument('--sdk', type=Path)
args = parser.parse_args()
sdk = args.sdk or Path(__file__).resolve().parents[2]
cleanup = function((sdk / 'src/codegen/phase_gapfill.cpp').read_text(),
                   'void cleanupAbsorbedGapFills(')
contains = function((sdk / 'src/codegen/function_graph.cpp').read_text(),
                    'bool FunctionNode::containsAddress(')
opening = contains.index('{') + 1
contains = contains[:opening] + '\n  ++containsQueries;\n' + contains[opening:]
types = (sdk / 'include/rex/codegen/function_types.h').read_text()
block_start = types.index('struct Block {')
block = types[block_start:types.index('};', block_start) + 2]
node_header = (sdk / 'include/rex/codegen/function_node.h').read_text()
queries = '\n'.join(function(node_header, signature) for signature in (
    'uint32_t base() const', 'uint32_t end() const', 'FunctionAuthority authority() const'))
# Preserve the exact former predicate and batch-removal order. Both algorithms
# execute the current production containsAddress/Block::contains implementations.
baseline = r'''
void cleanupAbsorbedGapFills(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  std::vector<uint32_t> toRemove;
  for (const auto& [addr, node] : graph.functions()) {
    if (node->authority() != FunctionAuthority::GAP_FILL) continue;
    for (const auto& [otherAddr, otherNode] : graph.functions()) {
      if (otherAddr == addr) continue;
      if (!otherNode->containsAddress(addr)) continue;
      if (otherNode->authority() != FunctionAuthority::GAP_FILL) {
        toRemove.push_back(addr);break;
      } else if (otherAddr < addr) {
        toRemove.push_back(addr);break;
      }
    }
  }
  for (uint32_t addr : toRemove) graph.removeFunction(addr);
}
'''
harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#define REXCODEGEN_TRACE(...) ((void)0)
enum class FunctionAuthority {GAP_FILL, DISCOVERED, VTABLE, HELPER, PDATA, CONFIG, IMPORT};
''' + block + r'''
struct FunctionNode {
  static inline uint64_t containsQueries=0;
  uint32_t base_,size_;FunctionAuthority authority_;std::vector<Block> blocks_;
  FunctionNode(uint32_t base,uint32_t size,FunctionAuthority authority,std::vector<Block> blocks)
      :base_(base),size_(size),authority_(authority),blocks_(std::move(blocks)){}
  bool containsAddress(uint32_t)const;
''' + queries + r'''
};
struct FunctionGraph {
  using Storage=std::unordered_map<uint32_t,std::unique_ptr<FunctionNode>>;
  Storage entries;mutable uint64_t iterations=0,scans=0;std::vector<uint32_t> removed;
  struct Range {
    const FunctionGraph* graph;
    struct Iterator {
      Storage::const_iterator cursor;const FunctionGraph* graph;
      auto& operator*()const{return *cursor;}
      Iterator& operator++(){++graph->iterations;++cursor;return *this;}
      bool operator!=(const Iterator& other)const{return cursor!=other.cursor;}
    };
    auto begin()const{++graph->scans;return Iterator{graph->entries.begin(),graph};}
    auto end()const{return Iterator{graph->entries.end(),graph};}
    auto size()const{return graph->entries.size();}
  };
  Range functions()const{return {this};}
  bool removeFunction(uint32_t addr){removed.push_back(addr);return entries.erase(addr)==1;}
};
struct CodegenContext {FunctionGraph graph;};
''' + contains + '\nnamespace before {\n' + baseline + '\n}\nnamespace after {\n' + cleanup + '\n}\n'
tests = r'''
using Authority=FunctionAuthority;
struct Spec {uint32_t base,size;Authority authority;std::vector<Block> blocks;};
CodegenContext Make(const std::vector<Spec>& specs) {
  CodegenContext ctx;
  for(const auto& spec:specs) {
    assert(!ctx.graph.entries.contains(spec.base));
    ctx.graph.entries.emplace(spec.base,std::make_unique<FunctionNode>(spec.base,spec.size,spec.authority,spec.blocks));
  }
  return ctx;
}
struct Counts {uint64_t beforeQueries,afterQueries,beforeIterations,afterIterations;};
Counts Compare(const std::vector<Spec>& specs,const std::set<uint32_t>* expected=nullptr) {
  auto old=Make(specs),current=Make(specs);
  FunctionNode::containsQueries=0;before::cleanupAbsorbedGapFills(old);
  auto oldQueries=FunctionNode::containsQueries;
  FunctionNode::containsQueries=0;after::cleanupAbsorbedGapFills(current);
  auto newQueries=FunctionNode::containsQueries;
  assert(old.graph.removed==current.graph.removed); // Includes original hash-map removal order.
  assert(old.graph.entries.size()==current.graph.entries.size());
  for(const auto& [addr,node]:old.graph.entries)assert(current.graph.entries.contains(addr));
  if(expected)assert(std::set<uint32_t>(current.graph.removed.begin(),current.graph.removed.end())==*expected);
  return {oldQueries,newQueries,old.graph.iterations,current.graph.iterations};
}
void Expect(std::vector<Spec> specs,std::set<uint32_t> expected) {Compare(specs,&expected);}
void Cases() {
  Expect({},{});Expect({{0x1000,16,Authority::GAP_FILL,{}}},{});
  // Empty and wrapped owners never contain anything; zero-size targets may be absorbed.
  Expect({{0x1000,0,Authority::CONFIG,{}},{0x1004,0,Authority::GAP_FILL,{}}},{});
  Expect({{0x1000,16,Authority::CONFIG,{}},{0x1004,0,Authority::GAP_FILL,{}}},{0x1004});
  Expect({{0xFFFFFFF0,0x20,Authority::CONFIG,{}},
          {0xFFFFFFF4,4,Authority::GAP_FILL,{}},{4,4,Authority::GAP_FILL,{}}},{});
  Expect({{0,0xFFFFFFFF,Authority::PDATA,{}},
          {0xFFFFFFF0,0x20,Authority::GAP_FILL,{}},{0xFFFFFFFF,0,Authority::GAP_FILL,{}}},{0xFFFFFFF0});
  // End is exclusive, base is inclusive, and owners with larger bases cannot absorb backwards.
  Expect({{0x1000,16,Authority::HELPER,{}},{0x0FFC,4,Authority::GAP_FILL,{}},
          {0x1004,4,Authority::GAP_FILL,{}},{0x1010,4,Authority::GAP_FILL,{}}},{0x1004});
  // A nearer overlapping owner does not hide a containing owner with a lower base.
  Expect({{0x1000,0x400,Authority::PDATA,{}},{0x1100,4,Authority::CONFIG,{}},
          {0x1200,4,Authority::GAP_FILL,{}}},{0x1200});
  for(auto authority:{Authority::GAP_FILL,Authority::DISCOVERED,Authority::VTABLE,
                       Authority::HELPER,Authority::IMPORT}) {
    Expect({{0x1000,0x100,authority,{{0x1000,4},{0x1080,8}}},
            {0x1004,4,Authority::GAP_FILL,{}},{0x1080,4,Authority::GAP_FILL,{}},
            {0x1088,4,Authority::GAP_FILL,{}},{0x1100,4,Authority::GAP_FILL,{}}},{0x1080});
  }
  for(auto authority:{Authority::PDATA,Authority::CONFIG}) {
    Expect({{0x1000,0x100,authority,{{0x1000,4},{0x1080,8}}},
            {0x1004,4,Authority::GAP_FILL,{}},{0x1080,4,Authority::GAP_FILL,{}},
            {0x1088,4,Authority::GAP_FILL,{}},{0x1100,4,Authority::GAP_FILL,{}}},
            {0x1004,0x1080,0x1088});
  }
  // Blocks outside overall bounds and wrapped blocks remain ineffective.
  Expect({{0x1000,16,Authority::DISCOVERED,{{0x1050,32}}},
          {0x1050,4,Authority::GAP_FILL,{}}},{});
  Expect({{0,0x100,Authority::DISCOVERED,{{0xFFFFFFF0,0x30}}},
          {4,4,Authority::GAP_FILL,{}}},{});
  // B is absorbed by A, but must remain an owner while C is classified.
  Expect({{0x1000,0x104,Authority::GAP_FILL,{{0x1000,4},{0x1100,4}}},
          {0x1100,0x104,Authority::GAP_FILL,{{0x1100,4},{0x1200,4}}},
          {0x1200,4,Authority::GAP_FILL,{}}},{0x1100,0x1200});
  // Multiple ends tie at a target point and must all expire before checking it.
  Expect({{0x1000,0x100,Authority::HELPER,{}},{0x1080,0x80,Authority::GAP_FILL,{}},
          {0x1100,4,Authority::GAP_FILL,{}}},{0x1080});
}
void RandomGraphs() {
  std::mt19937 random(0xD1A3);
  for(unsigned round=0;round<512;++round) {
    std::vector<Spec> specs;
    for(unsigned i=0;i<48;++i) {
      uint32_t base=(round%3==0?0xFFFFFC00u:0x1000u)+i*16;
      uint32_t size=(random()%40)*4;
      if(random()%17==0)size=0xFFFFFFFFu;
      Spec spec{base,size,static_cast<Authority>(random()%7),{}};
      if(random()%3!=0) {
        for(unsigned j=0,count=1+random()%4;j<count;++j)
          spec.blocks.push_back({static_cast<uint32_t>(base+(random()%48)*4),
                                 static_cast<uint32_t>((random()%16)*4)});
      }
      specs.push_back(std::move(spec));
    }
    std::shuffle(specs.begin(),specs.end(),random);Compare(specs);
  }
}
void Scaling() {
  for(uint32_t count:{200u,1000u}) {
    std::vector<Spec> sparse;
    for(uint32_t i=0;i<count;++i)
      sparse.push_back({0x1000+i*16,4,i%2?Authority::GAP_FILL:Authority::PDATA,{}});
    auto counts=Compare(sparse);
    assert(counts.beforeQueries==uint64_t(count/2)*(count-1));
    assert(counts.beforeIterations==count+uint64_t(count/2)*count);
    assert(counts.afterQueries==0&&counts.afterIterations==2*count);
    std::printf("%u nodes: containsAddress calls %llu -> %llu; graph iterations %llu -> %llu\n",count,
        static_cast<unsigned long long>(counts.beforeQueries),static_cast<unsigned long long>(counts.afterQueries),
        static_cast<unsigned long long>(counts.beforeIterations),static_cast<unsigned long long>(counts.afterIterations));
  }
  std::vector<Spec> nested;
  for(uint32_t i=0;i<128;++i)nested.push_back({0x1000+i*4,0x10000-i*4,Authority::GAP_FILL,{}});
  auto counts=Compare(nested);assert(counts.afterQueries==127&&counts.afterIterations==256);
}
int main(){Cases();RandomGraphs();Scaling();
  std::puts("PASS: production gap cleanup equivalence, removal order, holes, overlap, wraps, chains and bounded scaling");}
'''
args.output.mkdir(parents=True, exist_ok=True)
cpp = args.output.resolve() / 'gapfill-cleanup.cpp'
exe = args.output.resolve() / 'gapfill-cleanup.exe'
cpp.write_text(harness + tests)
subprocess.run([args.compiler, '-std=c++20', '-UNDEBUG', str(cpp), '-o', str(exe)],
               check=True, timeout=45)
subprocess.run([str(exe)], check=True, timeout=10)
