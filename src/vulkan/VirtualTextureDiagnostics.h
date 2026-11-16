#pragma once
#include "VirtualTextureAppend.h"
#include <unordered_map>

namespace kharvox {

struct VtRequestKey {
    std::uintptr_t owner{};
    std::uint64_t token{};
    bool operator==(const VtRequestKey& other) const {
        return owner==other.owner&&token==other.token;
    }
};
struct VtRequestHash {
    std::size_t operator()(const VtRequestKey& key) const {
        return std::hash<std::uint64_t>{}(key.token)
            ^std::hash<std::uintptr_t>{}(key.owner);
    }
};
struct VtAppendStatistics {
    std::uint64_t calls{},requests{},appended{},deferred{},saturatedCalls{};
    std::uint64_t destinationPeak{},observedRetries{},retryWaitMs{},maxRetryWaitMs{};
    std::uint64_t untrackedDeferrals{};
};

// Entries are opaque native request tokens. This measures observed waiting
// from a deferred append to a later successful append, NOT GPU residency,
// upload completion, mip selection, or unique requests across retries.
// The caller serializes diagnostic access; the append itself stays native.
class VtAppendDiagnostics {
    std::unordered_map<VtRequestKey,std::uint64_t,VtRequestHash> pending_;
    VtAppendStatistics statistics_{};
public:
    const VtAppendStatistics& statistics() const { return statistics_; }
    std::size_t pendingCount() const { return pending_.size(); }
    void observe(std::uintptr_t owner,const VtPageList& source,
        std::uint64_t destinationBefore,const VtAppendResult& result,
        std::uint64_t nowMs) {
        if(!result.valid||source.count>vtPageCapacity
            ||destinationBefore>vtPageCapacity
            ||result.appended>vtPageCapacity-destinationBefore
            ||result.appended>source.count
            ||result.deferred!=source.count-result.appended)return;
        auto& s=statistics_;
        ++s.calls;s.requests+=source.count;s.appended+=result.appended;
        s.deferred+=result.deferred;s.saturatedCalls+=result.deferred!=0;
        s.destinationPeak=std::max(s.destinationPeak,destinationBefore+result.appended);
        if(pending_.empty()&&!result.deferred)return;
        // Successful native appends need no page inspection unless a deferred
        // request can be resolved. Most gameplay frames have no pending work.
        for(std::uint64_t index=0;index<result.appended&&!pending_.empty();++index){
            const VtRequestKey key{owner,source.pages[index]};
            auto found=pending_.find(key);
            if(found!=pending_.end()){
                const auto wait=nowMs>=found->second?nowMs-found->second:0;
                ++s.observedRetries;s.retryWaitMs+=wait;
                s.maxRetryWaitMs=std::max(s.maxRetryWaitMs,wait);
                pending_.erase(found);
            }
        }
        // Always inspect the deferred suffix, including tokens also accepted
        // above: another occurrence can start a new pending request.
        for(std::uint64_t index=result.appended;index<source.count;++index){
            const VtRequestKey key{owner,source.pages[index]};
            if(pending_.find(key)==pending_.end()){
                if(pending_.size()<vtPageCapacity)pending_.emplace(key,nowMs);
                else ++s.untrackedDeferrals;
            }
        }
    }
};
} // namespace kharvox
