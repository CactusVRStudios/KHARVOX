#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <smmintrin.h>

namespace kharvox::native {
inline bool streamingMappedCopyAvailable() {
    static const bool supported = [] { int info[4]{}; __cpuid(info, 1); return (info[2] & (1 << 19)) != 0; }();
    return supported;
}

// CPU-written coherent Vulkan mappings may be write-combining memory. Ordinary
// memcpy can issue very slow PCIe reads there. MOVNTDQA reads complete streaming
// lines; on cached memory it still preserves the same bytes. Never overread the
// supplied span. The fences order prior engine writes, streaming loads and the
// destination stores before command submission; they do not wait for the GPU.
inline void copyMappedInput(void* destination, const void* source, size_t bytes,
                            bool stream = streamingMappedCopyAvailable()) {
    if (!stream) { std::memcpy(destination, source, bytes); return; }
    auto* dst = static_cast<unsigned char*>(destination);
    auto* src = static_cast<const unsigned char*>(source);
    _mm_mfence();
    while (bytes && (reinterpret_cast<uintptr_t>(src) & 63)) {
        *dst++ = *src++; --bytes;
    }
    while (bytes >= 64) {
        auto* block = reinterpret_cast<__m128i*>(const_cast<unsigned char*>(src));
        const auto a = _mm_stream_load_si128(block);
        const auto b = _mm_stream_load_si128(block + 1);
        const auto c = _mm_stream_load_si128(block + 2);
        const auto d = _mm_stream_load_si128(block + 3);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 16), b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 32), c);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 48), d);
        src += 64; dst += 64; bytes -= 64;
    }
    while (bytes--) *dst++ = *src++;
    _mm_mfence();
}
}
