#pragma once
#include <array>
#include <cstdint>
#include <mutex>
namespace kharvox {
// The per-eye cinematic FOV writer now agrees with the observed camera, so
// immersive images can use the same completed-source identity as gameplay.
// Scripted gameplay-camera sequences publish observed domain-2 transforms too.
// Their completed images must not be assigned to the next scheduled eye.
inline bool aerUseSourceQualification(bool pairPath,bool immersive,bool scripted,bool gameplay,bool cutscene){
    return pairPath&&(immersive||scripted||(gameplay&&!cutscene));
}
struct AerSourceKey {
    uint64_t poseId{},level{};int eye{-1};unsigned domain{}; // gameplay, cinematic, scripted
    bool valid()const{return poseId&&eye>=0&&eye<2;}
    bool operator==(const AerSourceKey& b)const{return poseId==b.poseId&&level==b.level&&eye==b.eye&&domain==b.domain;}
};
struct AerSourceObservation {AerSourceKey key{};unsigned count{};bool ambiguous{};
    bool valid()const{return count&&!ambiguous&&key.valid();}};
// Completed producer observations between Present entries. Multiple different
// sources are ambiguous, never resolved by taking whichever one happened last.
class AerSourceWindow {
    std::mutex mutex_;AerSourceObservation pending_{};
public:
    void observe(AerSourceKey key){
        if(!key.valid())return;
        std::lock_guard lock(mutex_);
        if(!pending_.count)pending_.key=key;
        else if(!(pending_.key==key))pending_.ambiguous=true;
        ++pending_.count;
    }
    AerSourceObservation take(){std::lock_guard lock(mutex_);auto out=pending_;pending_={};return out;}
};
// XR-thread-only bounded input history, indexed by identity rather than timing.
template<class T,size_t Capacity=128> class AerInputHistory {
    struct Entry{AerSourceKey key{};T value{};};
    std::array<Entry,Capacity> entries_{};size_t next_{};
public:
    void remember(AerSourceKey key,const T& value){if(key.valid()){entries_[next_]={key,value};next_=(next_+1)%Capacity;}}
    bool find(AerSourceKey key,T& value)const{
        if(!key.valid())return false;
        for(size_t n=0;n<Capacity;++n){const auto& e=entries_[(next_+Capacity-1-n)%Capacity];
            if(e.key==key){value=e.value;return true;}}
        return false;
    }
};
inline bool aerSourceCompletesPair(AerSourceKey source,bool otherValid,AerSourceKey other,uint64_t publishedPoseId=0){
    return source.valid()&&otherValid&&other.valid()&&source.eye!=other.eye
        &&source.poseId==other.poseId&&source.level==other.level&&source.domain==other.domain&&source.poseId>publishedPoseId;
}
}
