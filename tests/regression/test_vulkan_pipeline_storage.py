"""Run production Vulkan cache load/truncation blocks against real temporary files."""
import argparse
from pathlib import Path
import subprocess


def block(source, start):
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    production = (root / "src/graphics/vulkan/pipeline_cache.cpp").read_text()
    declaration_start = production.index("  std::vector<PipelineStoredDescription> pipeline_stored_descriptions;")
    declarations = production[declaration_start:production.index("  // <Shader hash", declaration_start)]
    header_start = production.index("  const uint32_t pipeline_storage_magic =")
    load_start = production.index("  if (fread(&pipeline_storage_file_header", header_start)
    header_and_load = production[header_start:load_start] + block(production, load_start)
    truncate_start = production.index("  rex::filesystem::TruncateStdioFile(\n      pipeline_storage_file_,")
    truncate_end = production.index("  shader_storage_cache_root_ =", truncate_start)
    truncation = production[truncate_start:truncate_end]
    harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <utility>
#include <vector>
#include <unistd.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
#define REXGPU_INFO(...) ((void)0)
struct PipelineDescription {
 static constexpr uint32_t kVersion=1;
 uint64_t vertex_shader_hash,vertex_shader_modification,pixel_shader_hash,pixel_shader_modification;
};
struct PipelineStoredDescription {uint64_t description_hash;PipelineDescription description;};
struct SpirvShaderTranslator {struct Modification {static constexpr uint32_t kVersion=1;};};
bool support_all=false;
bool ArePipelineRequirementsMet(const PipelineDescription& description){
 return support_all||!(description.vertex_shader_modification&1);
}
namespace rex {
uint32_t byte_swap(uint32_t value){return __builtin_bswap32(value);}
namespace filesystem {
bool Seek(FILE* file,int64_t offset,int origin){return fseeko(file,offset,origin)==0;}
int64_t Tell(FILE* file){return ftello(file);}
bool TruncateStdioFile(FILE* file,uint64_t length){
 return !fflush(file)&&!ftruncate(fileno(file),off_t(length))&&Seek(file,0,SEEK_END);
}
}
}
std::vector<uint64_t> Load(const std::filesystem::path& path,bool all_features){
 support_all=all_features;FILE* pipeline_storage_file_=fopen(path.c_str(),"a+b");assert(pipeline_storage_file_);
 assert(rex::filesystem::Seek(pipeline_storage_file_,0,SEEK_SET));
 const bool edram_fragment_shader_interlock=false;
 std::set<std::pair<uint64_t,uint64_t>> shader_translations_needed;
'''
    harness += declarations + header_and_load + truncation + r'''
 std::vector<uint64_t> kept;
 for(const auto& record:pipeline_stored_descriptions)kept.push_back(record.description.vertex_shader_hash);
 assert(!fclose(pipeline_storage_file_));return kept;
}
struct Header {uint32_t magic=0x53504558,api=0,version=rex::byte_swap(1);};
PipelineStoredDescription Record(uint64_t id,bool unsupported=false,bool corrupt=false){
 PipelineStoredDescription record{};record.description.vertex_shader_hash=id;
 record.description.vertex_shader_modification=unsupported?1:0;
 record.description_hash=XXH3_64bits(&record.description,sizeof(record.description));
 if(corrupt)record.description_hash^=1;return record;
}
void Write(const std::filesystem::path& path,const std::vector<PipelineStoredDescription>& records,bool torn=false){
 FILE* file=fopen(path.c_str(),"wb");assert(file);Header header;
 assert(fwrite(&header,sizeof(header),1,file)==1);
 for(const auto& record:records)assert(fwrite(&record,sizeof(record),1,file)==1);
 if(torn){auto record=Record(99);assert(fwrite(&record,7,1,file)==1);}
 assert(!fclose(file));
}
std::vector<char> Bytes(const std::filesystem::path& path){
 std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};
}
void AssertDisk(const std::filesystem::path& actual,const std::filesystem::path& expected,
                const std::vector<PipelineStoredDescription>& records){
 Write(expected,records);assert(Bytes(actual)==Bytes(expected));
}
int main(int argc,char** argv){
 assert(argc==2);const std::filesystem::path directory=argv[1];
 const auto file=directory/"cache.xpso",expected=directory/"expected.xpso";
 {
  const auto a=Record(1,true),b=Record(2),c=Record(3);
  Write(file,{a,b,c});assert((Load(file,false)==std::vector<uint64_t>{2,3}));
  AssertDisk(file,expected,{a,b,c});
  assert((Load(file,false)==std::vector<uint64_t>{2,3}));AssertDisk(file,expected,{a,b,c});
  assert((Load(file,true)==std::vector<uint64_t>{1,2,3}));
 }
 {
  const auto a=Record(4,true),b=Record(5,true);
  Write(file,{a,b});assert(Load(file,false).empty());AssertDisk(file,expected,{a,b});
  assert(Load(file,false).empty());AssertDisk(file,expected,{a,b});
  assert((Load(file,true)==std::vector<uint64_t>{4,5}));
 }
 {
  const auto a=Record(6),b=Record(7,true),c=Record(8,false,true),d=Record(9);
  Write(file,{a,b,c,d});assert((Load(file,false)==std::vector<uint64_t>{6}));
  AssertDisk(file,expected,{a,b});assert((Load(file,false)==std::vector<uint64_t>{6}));
  assert((Load(file,true)==std::vector<uint64_t>{6,7}));
 }
 {
  const auto a=Record(10,true),b=Record(11);
  Write(file,{a,b},true);assert((Load(file,false)==std::vector<uint64_t>{11}));
  AssertDisk(file,expected,{a,b});assert((Load(file,true)==std::vector<uint64_t>{10,11}));
 }
 {
  Write(file,{Record(12,false,true),Record(13)});assert(Load(file,false).empty());
  AssertDisk(file,expected,{});assert(Load(file,true).empty());
 }
 {
  const auto unsupported=Record(14,true),appended=Record(15);
  Write(file,{unsupported});assert(Load(file,false).empty());
  FILE* stream=fopen(file.c_str(),"ab");assert(stream);
  assert(fwrite(&appended,sizeof(appended),1,stream)==1);assert(!fclose(stream));
  assert((Load(file,false)==std::vector<uint64_t>{15}));
  AssertDisk(file,expected,{unsupported,appended});
 }
 std::cout<<"PASS: Vulkan pipeline filtering preserves valid disk records across repeated loads and append.\n";
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output.resolve() / "vulkan-pipeline-storage.cpp"
    exe = args.output.resolve() / "vulkan-pipeline-storage"
    source.write_text(harness)
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG",
                    "-I" + str(root / "thirdparty/xxHash"),
                    str(source), "-o", str(exe)], check=True, timeout=45)
    subprocess.run([str(exe), str(args.output.resolve())], check=True, timeout=10)


if __name__ == "__main__":
    main()
