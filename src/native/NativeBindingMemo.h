#pragma once
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace kharvox::native {
// Writers may overlap or re-enter. A stamp is usable only outside every
// metadata mutation. This invalidates CPU validation results, never GPU data.
class BindingMutationClock {
    std::atomic<uint64_t> revision{1};
    std::atomic<uint32_t> writers{};
public:
    void begin(){writers.fetch_add(1,std::memory_order_acq_rel);revision.fetch_add(1,std::memory_order_acq_rel);}
    void end(){revision.fetch_add(1,std::memory_order_release);writers.fetch_sub(1,std::memory_order_release);}
    std::optional<uint64_t> stamp() const {
        const auto value=revision.load(std::memory_order_acquire);
        if(writers.load(std::memory_order_acquire)||value!=revision.load(std::memory_order_acquire))return {};
        return value;
    }
};
struct BindingMutationScope {
    BindingMutationClock& clock;
    explicit BindingMutationScope(BindingMutationClock& value):clock(value){clock.begin();}
    ~BindingMutationScope(){clock.end();}
    BindingMutationScope(const BindingMutationScope&)=delete;
};
template<class Key,class Value,size_t Limit=4096> class BindingMemo {
    struct Entry {uint64_t revision;Value value;};
    std::map<Key,Entry> entries;
    // Map nodes remain stable on insert/assignment. Never copy this pointer
    // cache into another owner; clear it before destroying the backing nodes.
    mutable std::array<const typename std::map<Key,Entry>::value_type*,64> recent{};
public:
    BindingMemo()=default;
    BindingMemo(const BindingMemo&)=delete;
    BindingMemo& operator=(const BindingMemo&)=delete;
    const Value* find(const Key& key,std::optional<uint64_t> revision) const {
        if(!revision)return nullptr;
        const auto found=entries.find(key);
        return found!=entries.end()&&found->second.revision==*revision?&found->second.value:nullptr;
    }
    const Value* findFast(const Key& key,std::optional<uint64_t> revision,size_t bucket,bool& direct) const {
        direct=false;if(!revision)return nullptr;
        auto& cached=recent[bucket%recent.size()];
        if(cached&&cached->first==key&&cached->second.revision==*revision){direct=true;return &cached->second.value;}
        const auto found=entries.find(key);
        if(found==entries.end()||found->second.revision!=*revision)return nullptr;
        cached=&*found;return &found->second.value;
    }
    void put(const Key& key,Value value,std::optional<uint64_t> before,std::optional<uint64_t> after){
        if(!before||before!=after)return;
        auto found=entries.find(key);
        if(found!=entries.end())found->second={*before,std::move(value)};
        else if(entries.size()<Limit)entries.emplace(key,Entry{*before,std::move(value)});
    }
    void clear(){recent.fill(nullptr);entries.clear();}
    size_t size()const{return entries.size();}
};
// Equality and the full mutation stamp are always checked after this hash.
// Collisions only select a slower map lookup, never a different result.
template<class T,size_t N> size_t bindingMemoBucket(const std::array<T,N>& key){
    size_t hash=0;
    for(auto value:key)hash^=size_t(value)+size_t(0x9e3779b9u)+(hash<<6)+(hash>>2);
    return hash;
}
}
