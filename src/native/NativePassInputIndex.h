#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace kharvox::native {
// A framebuffer handle alone is not a render target identity: DOOM uses
// different compatible framebuffers for the same ordered attachment views.
// Unknown metadata may match only the exact original handle.
struct PassTargetIdentity {
    std::vector<uintptr_t> attachments;
    uint32_t width{}, height{}, layers{};
    uintptr_t unknownFramebuffer{};
    bool operator<(const PassTargetIdentity& other) const {
        return std::tie(unknownFramebuffer,width,height,layers,attachments) <
               std::tie(other.unknownFramebuffer,other.width,other.height,other.layers,other.attachments);
    }
};

// Rebuilt after real-eye input capture, before the first doubled pass. A
// repeated target's union is computed once, including conflicting layouts.
// Conflicts are reported only if that target is requested, as in the scan.
// No entry survives resetPassInputs or further real-eye capture.
template<class Inputs> class PassInputIndex {
public:
    struct Group { Inputs images; bool conflict{}; size_t passes{}; };
private:
    std::map<PassTargetIdentity,Group> groups;
public:
    size_t sourcePasses{}, sourceInputs{};
    void clear() { groups.clear(); sourcePasses=sourceInputs=0; }
    void add(const PassTargetIdentity& target,const Inputs& inputs) {
        auto& group=groups[target]; ++group.passes; ++sourcePasses;
        for(const auto& [view,layout]:inputs) {
            ++sourceInputs;
            auto [it,added]=group.images.emplace(view,layout);
            if(!added && it->second!=layout)group.conflict=true;
        }
    }
    const Group* find(const PassTargetIdentity& target) const {
        const auto it=groups.find(target);
        return it==groups.end()?nullptr:&it->second;
    }
    size_t size() const { return groups.size(); }
};
}
