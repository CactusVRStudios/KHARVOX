#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace kharvox::native {
// Caller serializes with mirror_mutex. Charges stay attached to physical
// allocations, including deliberately retained resources after a map reset.
template<size_t Limit=128,uint64_t Bytes=256ull*1024*1024>
class StorageMirrorBudget {
 struct Entry {uintptr_t image{};uint64_t bytes{};};
 std::array<Entry,Limit> entries_{};
 uint64_t bytes_{};size_t count_{};
public:
 static constexpr size_t maxCount=Limit;
 static constexpr uint64_t maxBytes=Bytes;
 bool reserve(uintptr_t image,uint64_t bytes){
  if(!image||!bytes||count_>=Limit||bytes>Bytes-bytes_)return false;
  Entry* slot{};
  for(auto& entry:entries_){if(entry.image==image)return false;if(!entry.image&&!slot)slot=&entry;}
  if(!slot)return false;
  *slot={image,bytes};bytes_+=bytes;++count_;return true;
 }
 bool release(uintptr_t image){
  if(!image)return false;
  for(auto& entry:entries_)if(entry.image==image){bytes_-=entry.bytes;--count_;entry={};return true;}
  return false;
 }
 uint64_t bytes()const{return bytes_;}
 size_t count()const{return count_;}
};
struct MirrorFramebufferReferences {
 uint32_t count{};
 void add(){++count;}
 void remove(bool hadMirror){if(hadMirror&&count)--count;}
 bool inUse()const{return count!=0;}
};
}
