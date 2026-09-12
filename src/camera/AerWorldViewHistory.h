#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace kharvox {
struct AerWorldView {
    std::array<float,12> pose{}; // origin, then three camera basis rows
    std::array<float,2> fov{};
    uint64_t poseId{},present{},level{};
    uintptr_t context{}; // validated main-camera owner; copies may use pooled addresses
    int eye{-1};
    unsigned domain{};
};
enum class AerWorldViewResult { UnknownSource, MissingTarget, Unchanged, Updated, AmbiguousSource, Invalid };
// Recognize only exact copies of recent, observed gameplay cameras. This is
// NOT a heuristic scan for camera-shaped matrices or GPU image provenance.
class AerWorldViewHistory {
    std::array<AerWorldView,128> entries_{};
    size_t next_{},count_{};
    std::mutex mutex_;
    static bool valid(const AerWorldView& s){
        if(!s.poseId||!s.context||s.eye<0||s.eye>1)return false;
        for(float x:s.pose)if(!std::isfinite(x))return false;
        for(float x:s.fov)if(!std::isfinite(x)||x<=0||x>=180)return false;
        for(int a=0;a<3;++a)for(int b=a;b<3;++b){
            float dot=0;for(int i=0;i<3;++i)dot+=s.pose[3+3*a+i]*s.pose[3+3*b+i];
            if(std::abs(dot-(a==b?1.f:0.f))>.001f)return false;
        }
        return true;
    }
    static bool recent(const AerWorldView& s,uint64_t now,uint64_t level){
        return s.poseId&&s.level==level&&s.present<=now&&now-s.present<=3;
    }
public:
    AerWorldViewResult recognize(const float* pose,const float* fov,uint64_t now,uint64_t level,AerWorldView& source){
        if(!pose||!fov)return AerWorldViewResult::Invalid;
        std::lock_guard lock(mutex_);const AerWorldView* found{};
        for(size_t n=0;n<count_;++n){const auto& s=entries_[(next_+entries_.size()-1-n)%entries_.size()];
            if(!recent(s,now,level)||std::memcmp(s.pose.data(),pose,sizeof(s.pose))||
                std::memcmp(s.fov.data(),fov,sizeof(s.fov)))continue;
            if(found&&(found->context!=s.context||found->poseId!=s.poseId||found->eye!=s.eye||found->domain!=s.domain))
                return AerWorldViewResult::AmbiguousSource;
            if(!found)found=&s;
        }
        if(!found)return AerWorldViewResult::UnknownSource;
        source=*found;return AerWorldViewResult::Unchanged;
    }
    void remember(const AerWorldView& sample){
        if(!valid(sample))return;
        std::lock_guard lock(mutex_);
        entries_[next_]=sample;next_=(next_+1)%entries_.size();
        if(count_<entries_.size())++count_;
    }
    AerWorldViewResult align(const float* pose,const float* fov,uint64_t wantedId,int wantedEye,
        uint64_t now,uint64_t level,AerWorldView& source,AerWorldView& target){
        if(!pose||!fov||!wantedId||wantedEye<0||wantedEye>1)return AerWorldViewResult::Invalid;
        std::lock_guard lock(mutex_);
        const AerWorldView* found{};
        for(size_t n=0;n<count_;++n){
            const auto& s=entries_[(next_+entries_.size()-1-n)%entries_.size()];
            if(!recent(s,now,level)||std::memcmp(s.pose.data(),pose,sizeof(s.pose))||
                std::memcmp(s.fov.data(),fov,sizeof(s.fov)))continue;
            if(found&&found->context!=s.context)return AerWorldViewResult::AmbiguousSource;
            if(!found)found=&s;
        }
        if(!found)return AerWorldViewResult::UnknownSource;
        source=*found;
        if(source.poseId==wantedId&&source.eye==wantedEye){target=source;return AerWorldViewResult::Unchanged;}
        for(size_t n=0;n<count_;++n){
            const auto& s=entries_[(next_+entries_.size()-1-n)%entries_.size()];
            if(!recent(s,now,level)||s.present<source.present||s.context!=source.context||
                s.poseId!=wantedId||s.eye!=wantedEye||s.fov!=source.fov)continue;
            target=s;
            return s.pose==source.pose?AerWorldViewResult::Unchanged:AerWorldViewResult::Updated;
        }
        return AerWorldViewResult::MissingTarget;
    }
};
}
