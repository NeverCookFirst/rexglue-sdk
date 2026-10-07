"""Count production graph notifications and preserve unresolved-owner lifecycle."""
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
a = p.parse_args()
sdk = a.sdk or Path(__file__).resolve().parents[2]
source = (sdk / 'src/codegen/function_graph.cpp').read_text()
header = (sdk / 'include/rex/codegen/function_graph.h').read_text()
node_header = (sdk / 'include/rex/codegen/function_node.h').read_text()
index = re.search(r'  std::unordered_set<uint32_t> unresolvedFunctionEntries_;', header).group(0)
# Node mutation is private: a new unresolved insertion path must update the index.
assert source.count('unresolvedJumps_.push_back(') == 1
assert re.findall(r'(?:->|\.)addUnresolvedJump\(', source) == ['->addUnresolvedJump(']
assert node_header.index('void addUnresolvedJump(') > node_header.index(' private:')
notification = function(source, 'void FunctionGraph::notifyFunctionAdded(')
bodies = '\n'.join(function(source, signature) for signature in (
    'FunctionNode::FunctionNode(', 'void FunctionNode::discover(',
    'void FunctionNode::discoverAsImport(', 'bool FunctionNode::canSeal(',
    'void FunctionNode::seal(', 'void FunctionNode::addBlock(',
    'bool FunctionNode::containsAddress(', 'void FunctionNode::addLabel(',
    'void FunctionNode::addCall(', 'void FunctionNode::addTailCall(',
    'void FunctionNode::addUnresolvedJump(', 'void FunctionNode::removeUnresolvedJump(',
    'bool FunctionNode::tryResolveAgainst(', 'bool FunctionNode::tryResolveAsInternalLabel(',
    'void FunctionNode::absorbRegion(',
    'FunctionNode* FunctionGraph::addFunction(uint32_t base, uint32_t size, FunctionAuthority authority,',
    'FunctionNode* FunctionGraph::getFunction(uint32_t entryPoint)',
    'bool FunctionGraph::removeFunction(', 'void FunctionGraph::addUnresolvedJumpToFunction(',
    'size_t FunctionGraph::tryResolveFunction(', 'void FunctionGraph::absorbRegionIntoFunction(',
    'bool FunctionGraph::trySealFunction(', 'size_t FunctionGraph::sealAllReady(',
    'void FunctionGraph::sealAll(', 'void FunctionGraph::notifyFunctionAdded('))
# Count attempts inside the real node resolver. Do not depend on wall-clock timing.
resolver = 'bool FunctionNode::tryResolveAgainst(FunctionNode* newFunction) {'
assert bodies.count(resolver) == 1
bodies = bodies.replace(resolver, resolver + '\n  ++resolutionAttempts;')
node_queries = '\n'.join(function(node_header, signature) for signature in (
    'bool isPending() const', 'bool isSealed() const', 'bool isRegistered() const',
    'bool canDiscover() const', 'bool isImport() const', 'bool hasUnresolvedJumps() const'))
# The immediately preceding algorithm uses a monotonic "ever unresolved" flag,
# then scans every pending graph entry on each registration. Keep all other
# production bodies identical for the behavioral comparison.
baseline_notification = '''
void FunctionGraph::notifyFunctionAdded(FunctionNode* newFunction) {
  if (!hasUnresolvedJumpsEver_) return;
  for (auto& [base,node] : functions_) {
    if(node.get()!=newFunction && node->isPending()) node->tryResolveAgainst(newFunction);
  }
}
'''
baseline = bodies.replace(notification, baseline_notification)
append = 'node->addUnresolvedJump(site, target, isCall, conditional);'
assert baseline.count(append) == 1
baseline = baseline.replace(append, append + '\n  hasUnresolvedJumpsEver_=true;')
fixture = r'''
uint64_t resolutionAttempts=0;
enum class FunctionAuthority { GAP_FILL, DISCOVERED, VTABLE, HELPER, PDATA, CONFIG, IMPORT };
enum class FunctionState { kRegistered, kDiscovered, kSealed };
struct Block {uint32_t base,size;uint32_t end()const{return base+size;}
  bool contains(uint32_t addr)const{return addr>=base&&addr<end();}};
struct FunctionAnalysis {};
struct FunctionNode;
struct CallTarget {
  FunctionNode* node=nullptr;
  static CallTarget function(FunctionNode* node){return {node};}
  static CallTarget import(uint32_t,const std::string&){return {};}
};
struct CallEdge {uint32_t site;CallTarget target;};
struct UnresolvedJump {uint32_t site,target;bool isCall,conditional;};
struct FunctionNode {
  uint32_t base_,size_;std::string name_;FunctionAuthority authority_;FunctionState state_;
  std::vector<Block> blocks_;std::vector<rex::codegen::ppc::Instruction*> instructions_;
  std::set<uint32_t> labels_;std::optional<FunctionAnalysis> analysis_;
  std::vector<UnresolvedJump> unresolvedJumps_;
  std::vector<CallEdge> calls_,tailCalls_;
  FunctionNode(uint32_t,uint32_t,FunctionAuthority);
  uint32_t base() const{return base_;}
  FunctionAuthority authority() const{return authority_;}
  const std::string& name() const{return name_;}
  const auto& blocks()const{return blocks_;}
  const auto& unresolvedJumps()const{return unresolvedJumps_;}
  void discover(std::vector<Block>,std::vector<rex::codegen::ppc::Instruction*>,std::set<uint32_t>);
  void discoverAsImport();bool canSeal()const;void seal();
  void addBlock(Block);bool containsAddress(uint32_t)const;void addLabel(uint32_t);
  void addCall(uint32_t,CallTarget);void addTailCall(uint32_t,CallTarget);
  void addUnresolvedJump(uint32_t,uint32_t,bool,bool);void removeUnresolvedJump(uint32_t);
  bool tryResolveAgainst(FunctionNode*);bool tryResolveAsInternalLabel(uint32_t);
  void absorbRegion(uint32_t,uint32_t);
''' + node_queries + r'''
};
// Count full-map traversal, retaining real hash lookup/storage and normal moves.
struct CountedFunctions {
  using Storage=std::unordered_map<uint32_t,std::unique_ptr<FunctionNode>>;
  Storage entries;uint64_t scans=0,iterations=0;
  struct Iterator {
    Storage::iterator cursor;CountedFunctions* owner;
    auto& operator*() const{return *cursor;}
    auto operator->() const{return cursor.operator->();}
    Iterator& operator++(){++owner->iterations;++cursor;return *this;}
    bool operator==(const Iterator& other) const{return cursor==other.cursor;}
    bool operator!=(const Iterator& other) const{return !(*this==other);}
  };
  Iterator begin(){++scans;return {entries.begin(),this};}
  Iterator end(){return {entries.end(),this};}
  Iterator find(uint32_t base){return {entries.find(base),this};}
  auto& operator[](uint32_t base){return entries[base];}
  void erase(Iterator it){entries.erase(it.cursor);}
};
struct FunctionGraph {
  CountedFunctions functions_;
  std::map<uint32_t,FunctionNode*> functionsByBase_;
  std::unordered_map<uint32_t,bool> functionHasXrefs_;
  bool hasUnresolvedJumpsEver_=false; // Used only by the reconstructed baseline.
''' + index + r'''
  FunctionNode* addFunction(uint32_t,uint32_t,FunctionAuthority,bool=false);
  FunctionNode* getFunction(uint32_t);bool removeFunction(uint32_t);
  bool isImport(uint32_t base){auto* node=getFunction(base);
    return node&&node->authority()==FunctionAuthority::IMPORT;}
  void addUnresolvedJumpToFunction(uint32_t,uint32_t,uint32_t,bool,bool);
  size_t tryResolveFunction(uint32_t);void absorbRegionIntoFunction(uint32_t,uint32_t,uint32_t);
  bool trySealFunction(uint32_t);size_t sealAllReady();void sealAll();
  void notifyFunctionAdded(FunctionNode*);
};
'''
tests = r'''
void TestRegistrationAndGapFill(bool baseline) {
  FunctionGraph graph;
  constexpr uint32_t count=1000,base=0x82000000,target=0x83000000;
  for(uint32_t i=0;i<count;++i){
    auto* node=graph.addFunction(base+i*16,12,FunctionAuthority::PDATA,true);
    assert(node->isRegistered()&&!node->hasUnresolvedJumps());
    assert(graph.functionsByBase_.at(node->base())==node);
    assert(graph.functionHasXrefs_.at(node->base()));
  }
  assert(graph.unresolvedFunctionEntries_.empty()&&graph.functions_.iterations==0);
  auto* owner=graph.getFunction(base);
  graph.addUnresolvedJumpToFunction(base,base+4,target,false,false);
  assert(graph.unresolvedFunctionEntries_.size()==1);
  resolutionAttempts=0;
  for(uint32_t i=0;i<count;++i)
    graph.addFunction(base+(count+i)*16,4,FunctionAuthority::GAP_FILL);
  const uint64_t expected=uint64_t(count)*count+uint64_t(count)*(count+1)/2;
  assert(graph.functions_.scans==(baseline?count:0));
  assert(graph.functions_.iterations==(baseline?expected:0));
  assert(resolutionAttempts==(baseline?expected-count:count));
  assert(owner->unresolvedJumps().size()==1);
  std::printf("%s: %u gap registrations, %llu full-graph iterations, %llu resolver attempts\n",
      baseline?"before":"after",count,
      static_cast<unsigned long long>(graph.functions_.iterations),
      static_cast<unsigned long long>(resolutionAttempts));
  auto* targetNode=graph.addFunction(target,4,FunctionAuthority::PDATA);
  assert(!owner->hasUnresolvedJumps()&&owner->tailCalls_.back().target.node==targetNode);
  if(!baseline)assert(graph.unresolvedFunctionEntries_.empty());
  auto scans=graph.functions_.scans;
  auto attempts=resolutionAttempts;
  graph.addFunction(target+16,4,FunctionAuthority::GAP_FILL);
  if(!baseline)assert(graph.functions_.scans==scans&&resolutionAttempts==attempts);
}
void TestResolutionAndIndexLifecycle(bool baseline) {
  FunctionGraph graph;
  constexpr uint32_t caller=0x82000000,known=0x82000100,target=0x82000200,
                     other=0x82000300,sealed=0x82000400;
  auto* node=graph.addFunction(caller,32,FunctionAuthority::PDATA,true);
  node->discover({{caller,32}}, {}, {});
  auto* knownNode=graph.addFunction(known,4,FunctionAuthority::IMPORT,true);
  graph.addUnresolvedJumpToFunction(0,caller+4,target,false,false);
  graph.addUnresolvedJumpToFunction(caller,caller+4,known,true,false);
  graph.addUnresolvedJumpToFunction(caller,caller+8,known,false,false);
  assert(graph.unresolvedFunctionEntries_.empty()&&!node->hasUnresolvedJumps());
  assert(node->calls_.size()==1&&node->calls_[0].target.node==knownNode);
  assert(node->tailCalls_.size()==1&&node->tailCalls_[0].target.node==knownNode);
  graph.addUnresolvedJumpToFunction(caller,caller+12,target,true,true);
  graph.addUnresolvedJumpToFunction(caller,caller+16,target,false,false);
  auto* sealedNode=graph.addFunction(sealed,4,FunctionAuthority::IMPORT,true);
  graph.addUnresolvedJumpToFunction(sealed,sealed,target,false,false);
  sealedNode->discoverAsImport();sealedNode->seal(); // Public sealing bypasses graph helpers.
  graph.addFunction(other,4,FunctionAuthority::PDATA,true);
  assert(node->unresolvedJumps().size()==2&&sealedNode->unresolvedJumps().size()==1);
  if(!baseline)assert(!graph.unresolvedFunctionEntries_.contains(sealed));
  auto* targetNode=graph.addFunction(target,4,FunctionAuthority::PDATA,true);
  assert(!node->hasUnresolvedJumps()&&node->tailCalls_.size()==3);
  // Preserve the existing reactive resolver: even the isCall branch becomes a tail call.
  assert(node->tailCalls_[1].site==caller+12&&node->tailCalls_[1].target.node==targetNode);
  assert(node->tailCalls_[2].site==caller+16&&node->tailCalls_[2].target.node==targetNode);
  assert(sealedNode->unresolvedJumps().size()==1&&sealedNode->tailCalls_.empty());
  if(!baseline)assert(graph.unresolvedFunctionEntries_.empty());
  assert(graph.removeFunction(caller)&&graph.removeFunction(sealed));
  assert(!graph.removeFunction(caller));

  auto* low=graph.addFunction(caller,16,FunctionAuthority::DISCOVERED);
  graph.addUnresolvedJumpToFunction(caller,caller+4,target+32,false,false);
  assert(graph.addFunction(caller,4,FunctionAuthority::GAP_FILL)==low);
  assert(graph.unresolvedFunctionEntries_.contains(caller));
  auto* replacement=graph.addFunction(caller,16,FunctionAuthority::CONFIG);
  assert(replacement!=low&&!replacement->hasUnresolvedJumps());
  assert(!graph.unresolvedFunctionEntries_.contains(caller));
  graph.addUnresolvedJumpToFunction(caller,caller+4,target+48,false,false);
  assert(graph.removeFunction(caller)&&graph.unresolvedFunctionEntries_.empty());
  graph.addFunction(target+32,4,FunctionAuthority::GAP_FILL);
  graph.addFunction(target+48,4,FunctionAuthority::GAP_FILL);
}
void TestExplicitResolutionAndSealing() {
  FunctionGraph graph;
  constexpr uint32_t entry=0x82000000,target=0x82000080;
  auto* node=graph.addFunction(entry,16,FunctionAuthority::PDATA);
  graph.addUnresolvedJumpToFunction(entry,entry+4,target,false,false);
  node->discover({{entry,16}}, {}, {}); // Discovery retains a registered owner's index entry.
  assert(graph.unresolvedFunctionEntries_.contains(entry));
  assert(!graph.trySealFunction(entry)&&graph.tryResolveFunction(entry)==0);
  graph.absorbRegionIntoFunction(entry,target,4);
  assert(graph.tryResolveFunction(entry)==1&&node->labels_.contains(target));
  assert(graph.unresolvedFunctionEntries_.empty()&&graph.trySealFunction(entry));
  assert(!graph.trySealFunction(entry)&&graph.tryResolveFunction(entry)==0);
  graph.addUnresolvedJumpToFunction(entry,entry+8,target+16,false,false);
  assert(graph.unresolvedFunctionEntries_.empty()); // Sealed nodes remain ineligible.
  graph.addFunction(target+16,4,FunctionAuthority::PDATA);
  assert(node->unresolvedJumps().size()==1);

  for(int mode=0;mode<3;++mode){
    FunctionGraph imports;
    auto* import=imports.addFunction(entry,4,FunctionAuthority::IMPORT);
    imports.addUnresolvedJumpToFunction(entry,entry,target,false,false);
    import->discoverAsImport();
    if(mode==0)assert(imports.trySealFunction(entry));
    if(mode==1)assert(imports.sealAllReady()==1);
    if(mode==2)imports.sealAll();
    assert(import->isSealed()&&import->hasUnresolvedJumps());
    assert(imports.unresolvedFunctionEntries_.empty());
  }
  FunctionGraph partial;
  auto* import=partial.addFunction(entry,4,FunctionAuthority::IMPORT);
  partial.addUnresolvedJumpToFunction(entry,entry,target,false,false);import->discoverAsImport();
  auto* pending=partial.addFunction(entry+32,4,FunctionAuthority::PDATA);
  partial.addUnresolvedJumpToFunction(entry+32,entry+32,target+4,false,false);
  bool threw=false;try{partial.sealAll();}catch(const std::runtime_error&){threw=true;}
  assert(threw&&import->isSealed()&&pending->isRegistered());
  assert(!partial.unresolvedFunctionEntries_.contains(entry));
  assert(partial.unresolvedFunctionEntries_.contains(entry+32));
}
void TestMoves() {
  constexpr uint32_t entry=0x82000000,target=0x82001000;
  FunctionGraph graph;auto* owner=graph.addFunction(entry,4,FunctionAuthority::PDATA);
  graph.addUnresolvedJumpToFunction(entry,entry,target,false,false);
  FunctionGraph moved(std::move(graph));
  assert(moved.getFunction(entry)==owner&&moved.unresolvedFunctionEntries_.contains(entry));
  auto* next=moved.addFunction(target,4,FunctionAuthority::PDATA);
  assert(!owner->hasUnresolvedJumps()&&owner->tailCalls_.back().target.node==next);
  assert(moved.unresolvedFunctionEntries_.empty());
  moved.addUnresolvedJumpToFunction(entry,entry,target+32,false,false);
  FunctionGraph assigned;
  assigned.addFunction(entry+16,4,FunctionAuthority::PDATA);
  assigned.addUnresolvedJumpToFunction(entry+16,entry+16,target+64,false,false);
  assigned=std::move(moved);
  assert(!assigned.getFunction(entry+16));
  assert(assigned.unresolvedFunctionEntries_.size()==1);
  assigned.addFunction(target+32,4,FunctionAuthority::PDATA);
  assert(!owner->hasUnresolvedJumps()&&assigned.unresolvedFunctionEntries_.empty());
}
'''
code = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#define REXCODEGEN_TRACE(...) ((void)0)
#define REXCODEGEN_DEBUG(...) ((void)0)
namespace rex::codegen::ppc {struct Instruction;}
namespace fmt {template<class... Args>std::string format(const char* value,Args&&...){return value;}}
'''
code += '\nnamespace current {\n' + fixture + bodies + tests + '\n}\n'
code += '\nnamespace baseline {\n' + fixture + baseline + tests + '\n}\n'
code += r'''
int main(){
  baseline::TestRegistrationAndGapFill(true);current::TestRegistrationAndGapFill(false);
  baseline::TestResolutionAndIndexLifecycle(true);current::TestResolutionAndIndexLifecycle(false);
  current::TestExplicitResolutionAndSealing();current::TestMoves();
  std::puts("PASS: indexed gap registration; immediate/deferred/internal resolution, replacement/removal, sealing and graph moves");
}
'''
a.output.mkdir(parents=True, exist_ok=True)
cpp = a.output.resolve() / 'graph-registration.cpp'
exe = a.output.resolve() / 'graph-registration.exe'
cpp.write_text(code)
subprocess.run([a.compiler, '-std=c++20', '-UNDEBUG', str(cpp), '-o', str(exe)],
               check=True, timeout=45)
subprocess.run([str(exe)], check=True, timeout=10)
