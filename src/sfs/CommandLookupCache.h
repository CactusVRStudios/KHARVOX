#pragma once
#include <atomic>
#include <cstdint>

namespace kharvox::sfs {
// Per-thread memo for "which element of this map belongs to command buffer X".
// The recording hooks look the same command buffer up one to three times per
// command, and a thread records into one command buffer for long stretches, so
// a one-entry cache removes nearly every hash lookup.
//
// unordered_map references survive rehashing, so the only thing that can make
// a cached entry stale is erasing it. Every erase or clear of the map MUST call
// invalidate() while it still holds the lock that excludes concurrent lookups,
// and find() MUST be called under that same lock (readers hold it shared). The
// lock hand-off then guarantees a reader never sees an epoch older than the
// last erase.
class CommandLookupCache {
    inline static std::atomic<uint64_t> epoch_{1};
public:
    static void invalidate(){epoch_.fetch_add(1,std::memory_order_release);}
    // Same contract as map.at(key), including throwing when the key is absent.
    template<class Owner,class Map,class Key>
    static typename Map::mapped_type& find(const Owner* owner,Map& map,const Key& key){
        using Value=typename Map::mapped_type;
        struct Entry{const void* owner{};Key key{};Value* value{};uint64_t epoch{};};
        thread_local Entry entry;
        const auto current=epoch_.load(std::memory_order_acquire);
        if(entry.value&&entry.owner==owner&&entry.epoch==current&&entry.key==key)return *entry.value;
        Value& found=map.at(key);
        entry=Entry{owner,key,&found,current};
        return found;
    }
};
}
