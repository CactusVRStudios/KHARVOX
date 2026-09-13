#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>

namespace kharvox {
inline constexpr uint64_t vtPageCapacity = 8192;
struct VtPageList {
    uint64_t count;
    uint8_t reserved[0x28];
    uint64_t pages[vtPageCapacity];
};
static_assert(offsetof(VtPageList, pages) == 0x30);
struct VtAppendResult { bool valid; uint64_t appended; uint64_t deferred; };
inline VtAppendResult appendVirtualTexturePages(VtPageList* destination, const VtPageList* source) {
    if (!destination || !source || destination == source ||
        destination->count > vtPageCapacity || source->count > vtPageCapacity)
        return {false, 0, 0};
    const auto count = std::min(source->count, vtPageCapacity - destination->count);
    std::memcpy(destination->pages + destination->count, source->pages, count * sizeof(uint64_t));
    destination->count += count;
    // DOOM's following residency pass carries unfulfilled source requests to
    // its other source buffer. Never shorten or consume that source here.
    return {true, count, source->count - count};
}
}
