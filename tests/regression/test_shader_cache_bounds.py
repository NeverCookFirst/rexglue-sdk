"""Exercise both production shader-cache readers without allocating corrupt counts."""
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


def reader(source):
    prefix_start = source.index("    uint64_t shader_storage_valid_bytes =")
    prefix_end = source.index("    // Load", prefix_start)
    prefix = source[prefix_start:prefix_end]
    fread_start = source.index("      if (!fread(&shader_header", prefix_end)
    loop_start = source.rfind("    while (", prefix_end, fread_start)
    increment = source.index("      shader_storage_valid_bytes +=", fread_start)
    loop_end = source.index("\n", increment)
    loop = source[loop_start:loop_end]
    truncate_start = source.index("rex::filesystem::TruncateStdioFile(shader_storage_file_, shader_storage_valid_bytes)", loop_end)
    condition = source.rfind("    if (shader_storage_size_valid)", loop_end, truncate_start)
    if condition >= 0:
        truncate = block(source, condition)
    else:
        truncate = source[truncate_start:source.index(";", truncate_start) + 1]
    return prefix, loop, truncate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--d3d-source", type=Path)
    parser.add_argument("--vulkan-source", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    sdk = root
    paths = [args.d3d_source or sdk / "src/graphics/d3d12/pipeline_cache.cpp",
             args.vulkan_source or sdk / "src/graphics/vulkan/pipeline_cache.cpp"]
    harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <unistd.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
#define REXGPU_WARN(...) ((void)0)
bool fail_size_query=false;
namespace rex {namespace filesystem {
bool Seek(FILE* file,int64_t offset,int origin){
 if(fail_size_query&&origin==SEEK_END)return false;return fseeko(file,offset,origin)==0;
}
int64_t Tell(FILE* file){return ftello(file);}
bool TruncateStdioFile(FILE* file,uint64_t length){
 return !fflush(file)&&!ftruncate(fileno(file),off_t(length))&&fseeko(file,0,SEEK_END)==0;
}
}}
#pragma pack(push,1)
struct ShaderStoredHeader {uint64_t ucode_data_hash;uint32_t ucode_dword_count:31;uint32_t type:1;};
#pragma pack(pop)
struct FileHeader {uint32_t magic=0x48534558,version=0x19122020;};
static_assert(sizeof(ShaderStoredHeader)==12);
uint32_t resize_calls=0,max_resize=0;
struct CheckedWords:std::vector<uint32_t> {
 void resize(size_t count){
  ++resize_calls;max_resize=std::max(max_resize,uint32_t(count));
  // Old production code trips this assertion, rather than allocating gigabytes.
  assert(count<=4096);std::vector<uint32_t>::resize(count);
 }
};
'''
    for name, path in zip(("D3D", "Vulkan"), paths):
        prefix, loop, truncate = reader(path.read_text())
        harness += "std::vector<std::vector<uint32_t>> Load" + name + r'''(const std::filesystem::path& path){
 FILE* shader_storage_file_=fopen(path.c_str(),"a+b");assert(shader_storage_file_);
 assert(fseeko(shader_storage_file_,0,SEEK_SET)==0);
 FileHeader shader_storage_file_header;
 assert(fread(&shader_storage_file_header,sizeof(shader_storage_file_header),1,shader_storage_file_)==1);
 std::vector<std::vector<uint32_t>> loaded;
 ShaderStoredHeader shader_header;CheckedWords ucode_dwords;
'''
        harness += prefix + loop + r'''
      loaded.push_back(ucode_dwords);
    }
''' + truncate + r'''
 assert(!fclose(shader_storage_file_));return loaded;
}
'''
    harness += r'''
using Payload=std::vector<uint32_t>;
using Reader=std::vector<Payload>(*)(const std::filesystem::path&);
void Write(const std::filesystem::path& path,const std::vector<Payload>& payloads,
           uint32_t corrupt_count=0,bool torn_header=false,bool bad_hash=false){
 FILE* file=fopen(path.c_str(),"wb");assert(file);FileHeader header;
 assert(fwrite(&header,sizeof(header),1,file)==1);
 for(const auto& payload:payloads){
  ShaderStoredHeader record{};record.ucode_dword_count=uint32_t(payload.size());
  record.ucode_data_hash=XXH3_64bits(payload.data(),payload.size()*4);
  if(bad_hash)record.ucode_data_hash^=1;
  assert(fwrite(&record,sizeof(record),1,file)==1);
  if(!payload.empty())assert(fwrite(payload.data(),payload.size()*4,1,file)==1);
 }
 if(corrupt_count||torn_header){
  ShaderStoredHeader record{};record.ucode_dword_count=corrupt_count;
  assert(fwrite(&record,torn_header?5:sizeof(record),1,file)==1);
 }
 assert(!fclose(file));
}
std::vector<char> Bytes(const std::filesystem::path& path){
 std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};
}
int main(int argc,char** argv){
 assert(argc==2);const std::filesystem::path directory=argv[1];
 const auto path=directory/"shader.xsh",expected=directory/"expected.xsh";
 const std::vector<Payload> valid{{1,2,3},Payload(300,0x12345678),{9}};
 for(Reader load:{&LoadD3D,&LoadVulkan}){
  Write(path,valid);const auto original=Bytes(path);resize_calls=max_resize=0;
  assert(load(path)==valid&&resize_calls==3&&max_resize==300);
  assert(Bytes(path)==original&&load(path)==valid);
  for(uint32_t count:{1u,4097u,0x7FFFFFFFu}){
   // The corrupt suffix advertises up to8GiB but has no payload bytes.
   Write(path,valid,count);Write(expected,valid);resize_calls=max_resize=0;
   assert(load(path)==valid&&resize_calls==3&&max_resize==300);
   assert(Bytes(path)==Bytes(expected)&&load(path)==valid);
   Write(path,{},count);Write(expected,{});resize_calls=0;
   assert(load(path).empty()&&resize_calls==0&&Bytes(path)==Bytes(expected));
  }
  Write(path,valid,0,true);Write(expected,valid);resize_calls=0;
  assert(load(path)==valid&&resize_calls==3&&Bytes(path)==Bytes(expected));
  Write(path,{},0,true);Write(expected,{});resize_calls=0;
  assert(load(path).empty()&&resize_calls==0&&Bytes(path)==Bytes(expected));
  Write(path,{{1,2}},0,false,true);Write(expected,{});resize_calls=0;
  assert(load(path).empty()&&resize_calls==1&&Bytes(path)==Bytes(expected));
  // Failure to size a readable file must not destructively truncate its cache.
  Write(path,valid);const auto intact=Bytes(path);fail_size_query=true;resize_calls=0;
  assert(load(path).empty()&&resize_calls==0&&Bytes(path)==intact);
  fail_size_query=false;assert(load(path)==valid);
 }
 std::cout<<"PASS: D3D/Vulkan shader-cache counts bounded before allocation; valid prefix preserved.\n";
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output.resolve() / "shader-cache-bounds.cpp"
    exe = args.output.resolve() / "shader-cache-bounds"
    source.write_text(harness)
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG",
                    "-I" + str(sdk / "thirdparty/xxHash"), str(source), "-o", str(exe)],
                   check=True, timeout=45)
    subprocess.run([str(exe), str(args.output.resolve())], check=True, timeout=10)


if __name__ == "__main__":
    main()
