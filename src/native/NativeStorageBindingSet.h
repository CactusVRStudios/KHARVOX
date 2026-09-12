#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
namespace kharvox::native {
// CPU bookkeeping only. Descriptor rebinds replace these small sorted sets
// thousands of times per frame. Retain their capacity until the owning
// command buffer is freed; clear still invalidates every previous binding.
class StorageBindingSet {
 std::vector<uint32_t> values;
public:
 void clear(){values.clear();}
 // Memo values are already sorted/unique and owned independently. Return
 // whether content changed; an identical bind must not rewrite the vector.
 bool replaceSorted(const std::vector<uint32_t>& source){
  if(values==source)return false;
  values.assign(source.begin(),source.end());return true;
 }
 void insert(uint32_t value){
  const auto at=std::lower_bound(values.begin(),values.end(),value);
  if(at==values.end()||*at!=value)values.insert(at,value);
 }
 bool empty()const{return values.empty();}
 size_t size()const{return values.size();}
 size_t capacity()const{return values.capacity();}
 auto begin()const{return values.begin();}
 auto end()const{return values.end();}
};
// Empty memo hits must clear stale bindings without creating new map nodes.
// Other set indices and command buffers are not affected by a partial bind.
template<class Commands,class Command>
bool restoreStorageBindings(Commands& commands,Command command,uint32_t set,const std::vector<uint32_t>& source){
 if(source.empty()){
  const auto cb=commands.find(command);if(cb==commands.end())return false;
  const auto target=cb->second.find(set);if(target==cb->second.end())return false;
  return target->second.replaceSorted(source);
 }
 return commands[command][set].replaceSorted(source);
}
}
