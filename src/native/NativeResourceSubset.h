#pragma once
#include <set>

namespace kharvox::native {
// A lifecycle-maintained selection, not a cache of mutable resource contents.
// The owner holds its metadata lock for every update and iteration.
template<class Key> class ResourceSubset {
    std::set<Key> keys;
public:
    void record(Key key, bool selected) {
        if(selected) keys.insert(key);
        else keys.erase(key); // Reused handles may have different usage.
    }
    void erase(Key key) { keys.erase(key); }
    void clear() { keys.clear(); }
    const std::set<Key>& selected() const { return keys; }
};
}
