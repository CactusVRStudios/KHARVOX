#pragma once
#include "../common/AerSourceTracking.h"
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <algorithm>
namespace kharvox {
// Learn only rigid near-weapon attachments across independent camera pairs.
// Apply the parent's correction to the CURRENT child pose, never a cached HUD.
class AerWeaponAttachments {
    struct Parent {uintptr_t model{},asset{};AerSourceKey key{};uint64_t present{};
        std::array<float,12> original{},target{};std::array<float,3> relativeHead{};};
    struct Track {uintptr_t child{},asset{},parent{},parentAsset{};uint64_t level{},pose{};
        unsigned count{};std::array<float,12> local{};std::array<float,3> firstRelative{};float motion{};};
    std::mutex mutex_;std::array<Parent,8> parents_{};size_t nextParent_{};
    std::array<Track,64> tracks_{};size_t nextTrack_{};
    static bool rigid(const float* p){
        for(int i=0;i<12;++i)if(!std::isfinite(p[i]))return false;
        const float scale2=p[3]*p[3]+p[4]*p[4]+p[5]*p[5];
        if(scale2<.039f||scale2>2.251f)return false;
        for(int i=0;i<3;++i)for(int j=i;j<3;++j){float d=0;
            for(int k=0;k<3;++k)d+=p[3+3*i+k]*p[3+3*j+k];
            if(std::abs(d-(i==j?scale2:0.f))>.002f*scale2)return false;}
        return true;
    }
    static void localPose(const float* parent,const float* child,float* local){
        // Inverse of a uniformly scaled orthogonal basis is transpose / scale².
        const float inverseScale2=1.f/(parent[3]*parent[3]+parent[4]*parent[4]+parent[5]*parent[5]);
        for(int i=0;i<3;++i){local[i]=0;for(int j=0;j<3;++j)local[i]+=parent[3+3*i+j]*(child[j]-parent[j])*inverseScale2;}
        for(int r=0;r<3;++r)for(int i=0;i<3;++i){local[3+3*r+i]=0;
            for(int j=0;j<3;++j)local[3+3*r+i]+=parent[3+3*i+j]*child[3+3*r+j]*inverseScale2;}
    }
    static void worldPose(const float* parent,const float* local,float* world){
        for(int i=0;i<3;++i){world[i]=parent[i];for(int j=0;j<3;++j)world[i]+=parent[3+3*j+i]*local[j];}
        for(int r=0;r<3;++r)for(int i=0;i<3;++i){world[3+3*r+i]=0;
            for(int j=0;j<3;++j)world[3+3*r+i]+=parent[3+3*j+i]*local[3+3*r+j];}
    }
public:
    void parent(uintptr_t model,uintptr_t asset,AerSourceKey key,uint64_t present,
        const float* original,const float* target,const float* camera){
        if(!model||!asset||!key.valid()||key.domain||!rigid(original)||!rigid(target))return;
        std::lock_guard lock(mutex_);Parent* p=nullptr;
        for(auto& v:parents_)if(v.model==model){p=&v;break;}
        if(!p){p=&parents_[nextParent_];nextParent_=(nextParent_+1)%parents_.size();}
        p->model=model;p->asset=asset;p->key=key;p->present=present;
        std::memcpy(p->original.data(),original,sizeof(p->original));std::memcpy(p->target.data(),target,sizeof(p->target));
        // Camera-local translation distinguishes a moving controller from a
        // headlocked UI that happens to move through the world with it.
        for(int i=0;i<3;++i){p->relativeHead[i]=0;
            for(int j=0;j<3;++j)p->relativeHead[i]+=camera[3+3*i+j]*(original[j]-camera[j]);}
    }
    bool resolve(uintptr_t child,uintptr_t asset,AerSourceKey key,uint64_t present,
        const float* pose,float* out){
        if(!child||!asset||!key.valid()||key.domain)return false;
        for(int i=0;i<12;++i)if(!std::isfinite(pose[i]))return false;
        std::lock_guard lock(mutex_);bool found=false;std::array<float,12> selected{};
        for(const auto& p:parents_){
            if(!p.model||p.model==child||!(p.key==key)||p.present!=present)continue;
            std::array<float,12> local{};localPose(p.original.data(),pose,local.data());
            float distance=0;for(int i=0;i<3;++i)distance+=local[i]*local[i];
            if(distance>64.f*64.f)continue;
            Track* t=nullptr;
            for(auto& v:tracks_)if(v.child==child&&v.parent==p.model){t=&v;break;}
            if(!t){t=&tracks_[nextTrack_];nextTrack_=(nextTrack_+1)%tracks_.size();*t={};}
            bool same=t->count&&t->asset==asset&&t->parentAsset==p.asset&&t->level==key.level
                &&key.poseId>=t->pose&&key.poseId-t->pose<=4;
            for(int i=0;i<12&&same;++i)same=std::abs(local[i]-t->local[i])<(i<3?.02f:.002f);
            if(!same){*t={};t->child=child;t->parent=p.model;t->asset=asset;t->parentAsset=p.asset;
                t->level=key.level;t->pose=key.poseId;t->local=local;t->firstRelative=p.relativeHead;t->count=1;continue;}
            if(t->pose!=key.poseId){t->pose=key.poseId;if(t->count<6)++t->count;}
            float motion=0;for(int i=0;i<3;++i)motion+=std::pow(p.relativeHead[i]-t->firstRelative[i],2.f);
            t->motion=std::max(t->motion,motion);
            if(t->count<6||t->motion<.25f)continue;
            std::array<float,12> candidate{};worldPose(p.target.data(),local.data(),candidate.data());
            if(found){for(int i=0;i<12;++i)if(std::abs(selected[i]-candidate[i])>(i<3?.002f:.0005f))return false;}
            else {selected=candidate;found=true;}
        }
        if(found)std::memcpy(out,selected.data(),sizeof(selected));return found;
    }
};
}
