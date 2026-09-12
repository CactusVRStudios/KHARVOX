#pragma once
#include <cstdint>

namespace kharvox {
// Reserved, non-exception exit status understood by the launcher.
inline constexpr uint32_t engineMemoryCapacityExitCode = 0x4b480001;

inline uint64_t enginePoolCapacity(uint32_t flags, uint64_t deviceLocal, uint64_t hostVisible) {
    // DOOM 20240321 allocator: bits 1, 3 and 4 select the host-visible pool.
    return (flags & 0x1a) ? hostVisible : deviceLocal;
}

inline bool oversizedEnginePoolImage(uint32_t bytes, uint32_t flags,
    bool image, uint64_t capacity) {
    // Bit 0 bypasses pooling. Zero/uninitialized capacity is not evidence
    // of an oversized request. Vulkan's requirement already includes padding;
    // alignment of the offset does not require rounding the size up again.
    return image && !(flags & 1) && capacity && uint64_t(bytes) > capacity;
}
}
