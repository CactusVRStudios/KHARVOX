#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
namespace kharvox::native {
// Last completed bind per command buffer, never an arbitrary older binding.
// The caller serializes access and includes every snapshot-policy dependency
// in context. Dynamic offsets are deliberately forwarded fresh by the caller.
template<class Command,class Set,size_t Width=4,size_t Capacity=64>
class SnapshotBatchCache {
public:
 SnapshotBatchCache()=default;
 SnapshotBatchCache(const SnapshotBatchCache&)=delete;
 SnapshotBatchCache& operator=(const SnapshotBatchCache&)=delete;
 struct Key {
  std::array<uint64_t,8> context{}; // revision, pipeline, layout, first, point, eye, mirror generation, frame
  uint32_t count{};
  std::array<Set,Width> source{},effective{};
  bool operator==(const Key&)const=default;
 };
 using Result=std::array<Set,Width>;
 struct Attempt {uint64_t token{};std::optional<Result> hit;};
 static std::optional<Key> key(std::array<uint64_t,8> context,std::span<const Set> source,std::span<const Set> effective){
  if(source.empty()||source.size()>Width||source.size()!=effective.size())return {};
  Key k;k.context=context;k.count=uint32_t(source.size());
  for(size_t i=0;i<source.size();++i){k.source[i]=source[i];k.effective[i]=effective[i];}return k;
 }
 Attempt begin(Command cb,const std::optional<Key>& key){
  auto entry=find(cb);
  if(!entry){if(!key||entries.size()>=Capacity)return {};entry=&entries.emplace(cb,Entry{}).first->second;hot=entry;hotCommand=cb;}
  auto& e=*entry;Attempt a;
  if(key&&e.valid&&e.key==*key)a.hit=e.result; // copy before any re-entrant execution
  a.token=e.token=++serial;e.valid=false;return a;
 }
 bool publish(Command cb,uint64_t token,const Key& key,const Result& result){
  auto entry=find(cb);if(!token||!entry||entry->token!=token)return false;
  auto& e=*entry;e.key=key;e.result=result;e.valid=true;return true;
 }
 void invalidate(Command cb){if(auto e=find(cb)){e->valid=false;e->token=++serial;}}
 void clear(){hot=nullptr;entries.clear();++serial;} // tokens do not repeat across frame/reset boundaries
private:
 struct Entry {Key key{};Result result{};uint64_t token{};bool valid{};};
 std::map<Command,Entry> entries;
 Entry* hot{};Command hotCommand{};
 Entry* find(Command cb){
  if(hot&&hotCommand==cb)return hot;
  auto it=entries.find(cb);if(it==entries.end())return nullptr;
  hotCommand=cb;return hot=&it->second;
 }
 uint64_t serial{};
};
}
