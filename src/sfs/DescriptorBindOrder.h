#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace kharvox::sfs {
class DescriptorBindOrder {
    std::vector<uint32_t> indices_;
public:
    void record(uint32_t first, uint32_t count) {
        if (!count) return;
        indices_.erase(std::remove_if(indices_.begin(), indices_.end(),
            [=](uint32_t index) { return index >= first && index - first < count; }),
            indices_.end());
        for (uint32_t offset = 0; offset < count; ++offset) indices_.push_back(first + offset);
    }
    const std::vector<uint32_t>& indices() const { return indices_; }
    void clear() { indices_.clear(); }
};
}
