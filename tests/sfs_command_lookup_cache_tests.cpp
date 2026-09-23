#include "../src/sfs/CommandLookupCache.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

using kharvox::sfs::CommandLookupCache;

struct Payload { int value{}; };
using Map = std::unordered_map<int, Payload>;

static bool throwsOutOfRange(const int* owner, Map& map, int key) {
    try { CommandLookupCache::find(owner, map, key); } catch (const std::out_of_range&) { return true; }
    return false;
}

int main() {
    int ownerA{}, ownerB{};

    // Same reference map.at() would give, repeatedly and for different keys.
    Map map;
    map[1].value = 10; map[2].value = 20;
    assert(&CommandLookupCache::find(&ownerA, map, 1) == &map.at(1));
    assert(&CommandLookupCache::find(&ownerA, map, 1) == &map.at(1));
    assert(&CommandLookupCache::find(&ownerA, map, 2) == &map.at(2));
    assert(&CommandLookupCache::find(&ownerA, map, 1) == &map.at(1));

    // Rehashing must not invalidate what was cached (references are stable).
    auto* before = &CommandLookupCache::find(&ownerA, map, 1);
    for (int i = 100; i < 5000; ++i) map[i].value = i;
    assert(&CommandLookupCache::find(&ownerA, map, 1) == before);
    assert(before == &map.at(1));

    // Two owners with the same key must not share an entry.
    Map other;
    other[1].value = 99;
    assert(CommandLookupCache::find(&ownerA, map, 1).value == 10);
    assert(CommandLookupCache::find(&ownerB, other, 1).value == 99);
    assert(CommandLookupCache::find(&ownerA, map, 1).value == 10);

    // Erase + invalidate: an absent key throws exactly like map.at().
    map.erase(1);
    CommandLookupCache::invalidate();
    assert(throwsOutOfRange(&ownerA, map, 1));
    // A recycled key must resolve to the NEW element, not the freed one.
    map[1].value = 11;
    assert(&CommandLookupCache::find(&ownerA, map, 1) == &map.at(1));
    assert(CommandLookupCache::find(&ownerA, map, 1).value == 11);

    // Never-present key throws and does not poison later lookups.
    assert(throwsOutOfRange(&ownerA, map, 424242));
    assert(CommandLookupCache::find(&ownerA, map, 2).value == 20);

    // The real contract, under threads: readers hold the lock shared while
    // calling find(); the writer erases and re-creates the element under the
    // exclusive lock and invalidates before releasing it. A reader must never
    // be handed a stale (erased) element.
    Map shared;
    shared[7].value = 0;
    std::shared_mutex lock;
    std::atomic<bool> stop{false};
    std::atomic<long> mismatches{0}, lookups{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) readers.emplace_back([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::shared_lock<std::shared_mutex> guard(lock);
            try {
                auto& found = CommandLookupCache::find(&ownerA, shared, 7);
                if (&found != &shared.at(7)) mismatches.fetch_add(1);
                lookups.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::out_of_range&) {
                // Legitimate only if the key really is absent right now.
                if (shared.count(7)) mismatches.fetch_add(1);
            }
        }
    });
    for (int i = 0; i < 20000; ++i) {
        std::unique_lock<std::shared_mutex> guard(lock);
        shared.erase(7);
        CommandLookupCache::invalidate();
        if (i % 3) { shared[7].value = i; }
        else { shared[7].value = -i; shared.erase(7); CommandLookupCache::invalidate(); shared[7].value = i; }
    }
    stop.store(true);
    for (auto& r : readers) r.join();
    assert(mismatches.load() == 0);
    assert(lookups.load() > 0);

    std::puts("sfs command lookup cache tests passed");
    return 0;
}
