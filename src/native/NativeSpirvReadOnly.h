#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kharvox::native {
struct StorageAccess {bool readOnly{};std::string name;};
struct ShaderStorageAccess {bool valid{};std::map<std::pair<uint32_t,uint32_t>,StorageAccess> bindings;};
inline bool permitsReadOnlySnapshot(const ShaderStorageAccess& shader,std::pair<uint32_t,uint32_t> binding){
 if(!shader.valid)return false;
 auto found=shader.bindings.find(binding);return found==shader.bindings.end()||found->second.readOnly;
}

// Conservative reflection of direct storage-block declarations, not a SPIR-V
// validator. NonWritable on the variable or on EVERY block member is required.
// Unknown/array/group-decorated declarations are never approved for snapshots.
// https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html (NonWritable).
inline ShaderStorageAccess reflectStorageAccess(const uint32_t* words,size_t count){
 ShaderStorageAccess result;
 if(!words||count<5||count>2*1024*1024||words[0]!=0x07230203||!words[3])return result;
 struct Variable {uint32_t type{},storage{};};
 std::map<uint32_t,Variable> variables,pointers;
 std::map<uint32_t,uint32_t> sets,bindings,members;
 std::map<uint32_t,std::string> names;
 std::set<uint32_t> bufferBlocks,nonWritable;
 std::set<std::pair<uint32_t,uint32_t>> memberNonWritable;
 for(size_t cursor=5;cursor<count;){
  const auto op=words[cursor]&0xffffu,n=words[cursor]>>16;
  if(!n||n>count-cursor)return result;
  const auto* w=words+cursor;
  switch(op){
   case 5: // OpName
    if(n<3)return result;
    {const char* p=reinterpret_cast<const char*>(w+2);size_t size=(n-2)*4,i=0;for(;i<size&&p[i];++i){}if(i==size)return result;names[w[1]]=std::string(p,i);}break;
   case 30: if(n<2)return result;members[w[1]]=n-2;break; // OpTypeStruct
   case 32: if(n!=4)return result;pointers[w[1]]={w[3],w[2]};break; // OpTypePointer
   case 59: if(n<4)return result;variables[w[2]]={w[1],w[3]};break; // OpVariable
   case 71: // OpDecorate
    if(n<3)return result;
    if(w[2]==3)bufferBlocks.insert(w[1]);
    if(w[2]==24)nonWritable.insert(w[1]);
    if(w[2]==33||w[2]==34){if(n!=4)return result;(w[2]==33?bindings:sets)[w[1]]=w[3];}break;
   case 72: if(n<4)return result;if(w[3]==24)memberNonWritable.insert({w[1],w[2]});break;
   case 73: case 74: case 75: return result; // Decoration groups: unsupported.
   default: break;
  }
  cursor+=n;
 }
 for(const auto&[id,var]:variables){
  if(!sets.count(id)||!bindings.count(id))continue;
  auto ptr=pointers.find(var.type);if(ptr==pointers.end()||ptr->second.storage!=var.storage)continue;
  const uint32_t type=ptr->second.type;
  if(var.storage!=12&&!(var.storage==2&&bufferBlocks.count(type)))continue;
  bool readOnly=false;
  if(auto member=members.find(type);member!=members.end()&&member->second){
   readOnly=nonWritable.count(id)!=0;
   if(!readOnly){readOnly=true;for(uint32_t i=0;i<member->second;++i)readOnly&=memberNonWritable.count({type,i})!=0;}
  }
  const auto key=std::make_pair(sets[id],bindings[id]);
  auto [it,added]=result.bindings.emplace(key,StorageAccess{readOnly,names[id]});
  if(!added)it->second.readOnly&=readOnly; // Aliases: any writable declaration vetoes.
 }
 result.valid=true;return result;
}
}
