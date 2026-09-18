#pragma once
#include "../common/AerSourceTracking.h"
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <memory>

namespace kharvox {
// The r287 trace still has unrecognized copied models whose matrix axes
// differ from the observed prop by up to 0.000374 per component. Accommodate
// that small rotation round trip; position, source identity and ambiguity
// checks remain independent. This is not permission to follow a new model.
inline bool aerWeaponPoseNear(const float* a,const float* aa,const float* b,const float* ba){
    for(int i=0;i<3;++i)if(!std::isfinite(a[i])||!std::isfinite(b[i])||std::abs(a[i]-b[i])>.002f)return false;
    for(int i=0;i<9;++i)if(!std::isfinite(aa[i])||!std::isfinite(ba[i])||std::abs(aa[i]-ba[i])>.0005f)return false;
    return true;
}
struct AerWeaponInput {
    std::array<float,3> grip{},baseline{};
    std::array<float,4> orientation{0,0,0,1};
    unsigned generation{};uint64_t epoch{};bool valid{};
    uint64_t sampleQpc{}; // Controller publication time, not sensor exposure time.
};
struct AerWeaponCamera {
    AerSourceKey key{};uint64_t present{};
    std::array<float,3> bodyOrigin{};
    std::array<float,9> bodyAxis{},headAxis{};
    float bodyYawDelta{};
    std::array<float,3> renderOrigin{};
    bool renderOriginValid{};
};
// Match the existing queued draw-source lifetime across root, prop and draw.
inline constexpr uint64_t aerWeaponSourcePresentAge=3;
struct AerWeaponFrame {AerWeaponCamera camera{};AerWeaponInput input{};};
inline bool alignAerWeaponDrawCamera(AerWeaponFrame& frame,const float* origin){
    if(!origin||!frame.camera.renderOriginValid)return false;
    for(int i=0;i<3;++i)if(!std::isfinite(origin[i])
        ||!std::isfinite(frame.camera.renderOrigin[i])
        ||std::abs(origin[i]-frame.camera.renderOrigin[i])>64.f)return false;
    for(int i=0;i<3;++i){frame.camera.bodyOrigin[i]+=origin[i]-frame.camera.renderOrigin[i];frame.camera.renderOrigin[i]=origin[i];}
    return true;
}
enum class AerWeaponResolveFailure { None, NoCamera, Level, Future, Stale, MissingInput, InvalidInput, Epoch, Generation };
struct AerWeaponResolveDiagnostic {
    AerWeaponResolveFailure failure{};
    uint64_t now{},cameraPresent{},poseId{};
};
inline bool aerWeaponPropSourceMatches(const AerWeaponFrame& frame,uint64_t rootPresent,
    uint64_t now,uint64_t level,uint64_t epoch,unsigned generation){
    return frame.camera.key.valid()&&frame.input.valid&&rootPresent==now
        &&frame.camera.present<=now&&now-frame.camera.present<=aerWeaponSourcePresentAge
        &&frame.camera.key.level==level&&frame.input.epoch==epoch&&frame.input.generation==generation;
}

// Inputs are latched once per camera pose ID, independently of the XR output
// phase. The camera publisher supplies the frame actually built by DOOM.
class AerWeaponSourceHistory {
    std::mutex mutex_;
    AerInputHistory<AerWeaponInput> inputs_;
    AerWeaponCamera camera_{};
    AerInputHistory<AerWeaponCamera> cameras_;
    bool resolveLocked(uint64_t now,uint64_t level,uint64_t epoch,unsigned generation,
        AerWeaponFrame& out,AerWeaponResolveDiagnostic& diagnostic){
        diagnostic={AerWeaponResolveFailure::None,now,camera_.present,camera_.key.poseId};
        auto fail=[&](AerWeaponResolveFailure reason){diagnostic.failure=reason;return false;};
        using F=AerWeaponResolveFailure;
        if(!camera_.key.valid())return fail(F::NoCamera);
        if(camera_.key.level!=level)return fail(F::Level);
        if(camera_.present>now)return fail(F::Future);
        if(now-camera_.present>aerWeaponSourcePresentAge)return fail(F::Stale);
        AerWeaponInput input;
        if(!inputs_.find({camera_.key.poseId,level,0},input))return fail(F::MissingInput);
        if(!input.valid)return fail(F::InvalidInput);
        if(input.epoch!=epoch)return fail(F::Epoch);
        if(input.generation!=generation)return fail(F::Generation);
        out={camera_,input};return true;
    }
public:
    AerWeaponInput remember(uint64_t poseId,uint64_t level,const AerWeaponInput& input){
        std::lock_guard lock(mutex_);AerWeaponInput existing;
        const AerSourceKey key{poseId,level,0};
        if(!inputs_.find(key,existing)||existing.epoch!=input.epoch||existing.generation!=input.generation
            ||(!existing.valid&&input.valid)){
            inputs_.remember(key,input);return input;
        }
        return existing;
    }
    void camera(const AerWeaponCamera& camera){std::lock_guard lock(mutex_);camera_=camera;cameras_.remember(camera.key,camera);}
    bool frame(AerSourceKey key,uint64_t now,AerWeaponFrame& out){
        std::lock_guard lock(mutex_);AerWeaponCamera camera;AerWeaponInput input;
        if(!cameras_.find(key,camera)||camera.present>now||now-camera.present>aerWeaponSourcePresentAge
            ||!inputs_.find({key.poseId,key.level,0},input)||!input.valid)return false;
        out={camera,input};return true;
    }
    bool resolve(uint64_t now,uint64_t level,uint64_t epoch,unsigned generation,AerWeaponFrame& out){
        std::lock_guard lock(mutex_);
        AerWeaponResolveDiagnostic diagnostic;
        return resolveLocked(now,level,epoch,generation,out,diagnostic);
    }
    template<class Clock>
    bool resolveCurrent(Clock clock,uint64_t level,uint64_t epoch,unsigned generation,
        AerWeaponFrame& out,AerWeaponResolveDiagnostic& diagnostic){
        std::lock_guard lock(mutex_);
        // A camera can be published while a worker waits for this mutex.
        // Sample Present AFTER locking, otherwise that valid camera appears
        // to come from the future and the root falls back to native placement.
        return resolveLocked(clock(),level,epoch,generation,out,diagnostic);
    }
};

// Same basis change as the head camera: express a stored controller in the
// body yaw that DOOM actually used, without adding a new tracking sample.
inline AerWeaponInput rebaseAerWeaponInput(AerWeaponInput input,float degrees){
    const float a=degrees*0.01745329251994329577f,c=std::cos(a),s=std::sin(a);
    for(auto* v:{&input.grip,&input.baseline}){
        const float f=(*v)[0],l=(*v)[1];(*v)[0]=c*f+s*l;(*v)[1]=-s*f+c*l;
    }
    const float h=-a*.5f,qc=std::cos(h),qs=std::sin(h);
    const auto q=input.orientation;
    input.orientation={qc*q[0]+qs*q[2],qc*q[1]+qs*q[3],qc*q[2]-qs*q[0],qc*q[3]-qs*q[1]};
    return input;
}

inline bool aerControllerWorldFrame(const AerWeaponFrame& frame,float* origin,float* axis){
    if(!frame.input.valid)return false;
    const auto input=rebaseAerWeaponInput(frame.input,frame.camera.bodyYawDelta);
    const auto& q=input.orientation;float n=0;for(float x:q)n+=x*x;
    if(!std::isfinite(n)||std::abs(n-1.f)>.001f)return false;
    for(int i=0;i<3;++i){origin[i]=frame.camera.bodyOrigin[i];
        for(int j=0;j<3;++j)origin[i]+=frame.camera.bodyAxis[j*3+i]*input.grip[j];}
    for(int row=0;row<3;++row){
        float doom[3]{};doom[row]=1;const float v[3]{-doom[1],doom[2],-doom[0]};
        const float t[3]{2*(q[1]*v[2]-q[2]*v[1]),2*(q[2]*v[0]-q[0]*v[2]),2*(q[0]*v[1]-q[1]*v[0])};
        const float r[3]{v[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],
            v[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],v[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0]};
        const float local[3]{-r[2],-r[0],r[1]};
        for(int i=0;i<3;++i){axis[row*3+i]=0;for(int j=0;j<3;++j)axis[row*3+i]+=frame.camera.bodyAxis[j*3+i]*local[j];}
    }
    for(int i=0;i<3;++i){if(!std::isfinite(origin[i]))return false;
        for(int j=i;j<3;++j){float dot=0;for(int k=0;k<3;++k)dot+=axis[i*3+k]*axis[j*3+k];
            if(!std::isfinite(dot)||std::abs(dot-(i==j?1.f:0.f))>.001f)return false;}}
    return true;
}
// Preserve the already sampled model/animation relative to its controller,
// using the target camera's existing input. No velocity estimate or future pose.
inline bool rebaseAerDrawPlacement(const AerWeaponFrame& from,const AerWeaponFrame& to,
    const float* origin,const float* axis,float* outOrigin,float* outAxis){
    if(from.input.epoch!=to.input.epoch||from.input.generation!=to.input.generation
        ||from.camera.key.level!=to.camera.key.level||from.camera.key.domain!=to.camera.key.domain)return false;
    float a[3]{},b[3]{},ra[9]{},rb[9]{};
    if(!aerControllerWorldFrame(from,a,ra)||!aerControllerWorldFrame(to,b,rb))return false;
    auto rotate=[&](const float* v,float* out){float local[3]{};
        for(int i=0;i<3;++i)for(int j=0;j<3;++j)local[i]+=ra[i*3+j]*v[j];
        for(int i=0;i<3;++i){out[i]=0;for(int j=0;j<3;++j)out[i]+=rb[j*3+i]*local[j];}};
    float relative[3]{origin[0]-a[0],origin[1]-a[1],origin[2]-a[2]};rotate(relative,outOrigin);
    for(int i=0;i<3;++i)outOrigin[i]+=b[i];
    for(int i=0;i<3;++i)rotate(axis+i*3,outAxis+i*3);
    return true;
}

// Final root and prop transforms share the camera's identity. Multiple pairs
// can be in flight; a newer schedule must not evict a still-running older pair.
class AerWeaponSourceTransforms {
    struct Entry {
        AerSourceKey key{};uintptr_t entity{};uint64_t epoch{};unsigned generation{},role{};
        std::array<float,3> origin{};std::array<float,9> axis{};
        AerWeaponFrame frame{};
    };
    std::mutex mutex_;std::array<Entry,256> entries_{};size_t next_{};
    // Draw-only pair snapshots must never become native animation history.
    std::array<Entry,256> draws_{};size_t nextDraw_{};
    struct Binding {
        uintptr_t model{},asset{};AerSourceKey confirmed{};
        std::array<float,3> origin{};std::array<float,9> axis{};
        std::array<Entry,8> identities{};size_t count{};
    };
    std::unique_ptr<std::array<Binding,64>> bindings_=std::make_unique<std::array<Binding,64>>();size_t nextBinding_{};
public:
    // Draw-time models are copied render objects, not idHands entity pointers.
    // Prefer an exact observed pose, otherwise allow only bounded roundoff,
    // then resolve the actual draw camera. Conflicting identities never write.
    int forDraw(AerSourceKey wanted,const float* origin,const float* axis,
        float* targetOrigin,float* targetAxis,uint64_t& sourceId,const AerWeaponFrame* drawFrame=nullptr,
        uintptr_t model=0,uintptr_t asset=0,bool* recovered=nullptr,bool followDrawBody=false){
        if(recovered)*recovered=false;
        if(!wanted.valid()||wanted.domain)return 0;
        std::lock_guard lock(mutex_);const Entry* selected{};Entry selectedValue{};bool recognized=false;
        std::array<const Entry*,256> identities{};size_t identityCount{};
        auto eligible=[&](const Entry& e){return e.key.valid()&&e.key.level==wanted.level&&e.key.domain==wanted.domain
            &&(!drawFrame||(e.epoch==drawFrame->input.epoch&&e.generation==drawFrame->input.generation))
            &&e.key.poseId<=wanted.poseId+6&&wanted.poseId<=e.key.poseId+6;};
        auto exact=[&](const Entry& e){return !std::memcmp(e.origin.data(),origin,sizeof(e.origin))
            &&!std::memcmp(e.axis.data(),axis,sizeof(e.axis));};
        bool hasExact=false;
        for(const auto& e:entries_)if(eligible(e)&&exact(e)){hasExact=true;break;}
        bool hasPoseMatch=hasExact;
        if(!hasPoseMatch)for(const auto& e:entries_)if(eligible(e)&&aerWeaponPoseNear(e.origin.data(),e.axis.data(),origin,axis)){hasPoseMatch=true;break;}
        const Binding* binding=nullptr;
        if(!hasPoseMatch&&model&&asset&&drawFrame){
            for(const auto& b:*bindings_){
                if(b.model!=model||b.asset!=asset||!b.count||b.confirmed.level!=wanted.level
                    ||b.confirmed.domain!=wanted.domain||b.confirmed.poseId>wanted.poseId
                    ||wanted.poseId-b.confirmed.poseId>6)continue;
                bool close=true;
                for(int i=0;i<3;++i)close&=std::isfinite(origin[i])&&std::abs(origin[i]-b.origin[i])<=.03f;
                for(int i=0;i<9;++i)close&=std::isfinite(axis[i])&&std::abs(axis[i]-b.axis[i])<=.01f;
                for(size_t i=0;i<b.count;++i)close&=b.identities[i].epoch==drawFrame->input.epoch
                    &&b.identities[i].generation==drawFrame->input.generation;
                if(close)binding=&b;
                break;
            }
        }
        auto boundIdentity=[&](const Entry& e){
            if(!binding)return false;
            for(size_t i=0;i<binding->count;++i){const auto& b=binding->identities[i];
                if(e.entity==b.entity&&e.role==b.role&&e.epoch==b.epoch&&e.generation==b.generation
                    &&e.key.poseId==b.key.poseId)return true;}
            return false;
        };
        for(const auto& e:entries_){
            if(!eligible(e)||(binding?!boundIdentity(e):(hasExact?!exact(e):!aerWeaponPoseNear(e.origin.data(),e.axis.data(),origin,axis))))continue;
            recognized=true;
            bool duplicate=false;
            for(size_t i=0;i<identityCount;++i){const auto& old=*identities[i];
                if(old.entity==e.entity&&old.epoch==e.epoch&&old.generation==e.generation&&old.role==e.role){
                    // Prefer the closest recorded predecessor, independent of ring order.
                    if(e.key.poseId<=wanted.poseId&&(old.key.poseId>wanted.poseId||e.key.poseId>old.key.poseId))identities[i]=&e;
                    duplicate=true;break;}}
            if(duplicate)continue;
            identities[identityCount++]=&e;
            const Entry* target{};
            for(const auto& t:draws_){
                if(t.key.poseId==wanted.poseId&&t.key.level==wanted.level&&t.key.domain==wanted.domain
                    &&t.entity==e.entity&&t.epoch==e.epoch&&t.generation==e.generation&&t.role==e.role){target=&t;break;}
            }
            for(const auto& t:entries_){
                if(target)break;
                if(t.key.poseId!=wanted.poseId||t.key.level!=wanted.level||t.key.domain!=wanted.domain
                    ||t.entity!=e.entity||t.epoch!=e.epoch||t.generation!=e.generation||t.role!=e.role)continue;
                target=&t;
            }
            if(target){
                auto candidate=*target;
                // SFS root/prop snapshots can be recorded at different body
                // anchors for one tracking ID. Compare them in the actual
                // draw frame, not before movement compensation.
                if(followDrawBody&&drawFrame){
                    if(!rebaseAerDrawPlacement(candidate.frame,*drawFrame,target->origin.data(),target->axis.data(),
                        candidate.origin.data(),candidate.axis.data()))return 3;
                    candidate.frame=*drawFrame;
                }
                if(selected){
                    const bool same=followDrawBody
                        ?aerWeaponPoseNear(selected->origin.data(),selected->axis.data(),candidate.origin.data(),candidate.axis.data())
                        :selected->origin==candidate.origin&&selected->axis==candidate.axis;
                    if(!same)return 4;
                }
                selectedValue=candidate;selected=&selectedValue;sourceId=e.key.poseId;
            }
        }
        auto rememberBinding=[&]{
            if(binding){if(recovered)*recovered=true;return;} // never extend fallback lifetime
            if(!model||!asset||!drawFrame||!identityCount||identityCount>8)return;
            Binding* slot=nullptr;
            for(auto& b:*bindings_)if(b.model==model){slot=&b;break;}
            if(!slot){slot=&(*bindings_)[nextBinding_];nextBinding_=(nextBinding_+1)%bindings_->size();}
            *slot={};slot->model=model;slot->asset=asset;slot->confirmed=wanted;slot->count=identityCount;
            std::memcpy(slot->origin.data(),origin,sizeof(slot->origin));std::memcpy(slot->axis.data(),axis,sizeof(slot->axis));
            for(size_t i=0;i<identityCount;++i)slot->identities[i]=*identities[i];
        };
        std::array<Entry,8> derived{};size_t derivedCount{};
        if(!selected&&drawFrame&&drawFrame->camera.key==wanted){
            // Root and prop can share an exact pose. Hold every recognized
            // role only when all produce the same target; never choose one.
            if(identityCount>derived.size())return 4;
            for(size_t i=0;i<identityCount;++i){
                const Entry* latest=identities[i];
                for(const auto& observed:entries_){
                    if(observed.entity==latest->entity&&observed.role==latest->role
                        &&observed.epoch==latest->epoch&&observed.generation==latest->generation
                        &&observed.key.level==wanted.level&&observed.key.domain==wanted.domain
                        &&observed.key.poseId<=wanted.poseId&&observed.key.poseId>latest->key.poseId)latest=&observed;
                }
                const auto& e=*latest;
                if(e.key.poseId>wanted.poseId)return 3;
                Entry candidate=e;candidate.key=wanted;candidate.frame=*drawFrame;
                if(!rebaseAerDrawPlacement(e.frame,*drawFrame,e.origin.data(),e.axis.data(),candidate.origin.data(),candidate.axis.data()))return 3;
                if(derivedCount){
                    const bool same=followDrawBody
                        ?aerWeaponPoseNear(derived[0].origin.data(),derived[0].axis.data(),candidate.origin.data(),candidate.axis.data())
                        :derived[0].origin==candidate.origin&&derived[0].axis==candidate.axis;
                    if(!same)return 4;
                }
                derived[derivedCount++]=candidate;sourceId=e.key.poseId;
            }
            if(derivedCount){std::memcpy(targetOrigin,derived[0].origin.data(),sizeof(derived[0].origin));
                std::memcpy(targetAxis,derived[0].axis.data(),sizeof(derived[0].axis));
                for(size_t i=0;i<derivedCount;++i){draws_[nextDraw_]=derived[i];nextDraw_=(nextDraw_+1)%draws_.size();}rememberBinding();return 6;}
        }
        if(!selected)return recognized?3:0;
        auto result=*selected;
        // Selected SFS candidates already share the actual draw-camera frame.
        std::memcpy(targetOrigin,result.origin.data(),sizeof(result.origin));
        std::memcpy(targetAxis,result.axis.data(),sizeof(result.axis));
        for(size_t i=0;i<identityCount;++i){
            auto snapshot=*identities[i];snapshot.key=wanted;snapshot.origin=result.origin;snapshot.axis=result.axis;snapshot.frame=result.frame;
            bool held=false;
            for(const auto& d:draws_)if(d.key.poseId==wanted.poseId&&d.key.level==wanted.level
                &&d.key.domain==wanted.domain&&d.entity==snapshot.entity&&d.role==snapshot.role
                &&d.epoch==snapshot.epoch&&d.generation==snapshot.generation){held=true;break;}
            if(!held){draws_[nextDraw_]=snapshot;nextDraw_=(nextDraw_+1)%draws_.size();}
        }
        rememberBinding();
        return std::memcmp(targetOrigin,origin,sizeof(result.origin))
            ||std::memcmp(targetAxis,axis,sizeof(result.axis))?2:1;
    }
    bool hold(const AerWeaponFrame& frame,uintptr_t entity,unsigned role,float* origin,float* axis,AerWeaponFrame* heldFrame=nullptr){
        if(heldFrame)*heldFrame=frame;
        if(!frame.camera.key.valid()||!entity||!origin||!axis)return false;
        std::lock_guard lock(mutex_);
        for(auto& e:entries_){
            if(e.key.poseId!=frame.camera.key.poseId||e.key.level!=frame.camera.key.level
                ||e.key.domain!=frame.camera.key.domain
                ||e.entity!=entity||e.epoch!=frame.input.epoch||e.generation!=frame.input.generation||e.role!=role)continue;
            // The first eye can be evaluated repeatedly before the second.
            // Keep one immutable model pose for this pair, including repeat
            // evaluations of the first eye. A new pose ID, entity, role or
            // calibration/weapon epoch still creates a fresh snapshot.
            // A repeated camera publication may have moved within this ID.
            // Children must inherit the camera belonging to the held model,
            // otherwise draw-time rebasing subtracts the wrong body position.
            if(heldFrame)*heldFrame=e.frame;
            std::memcpy(origin,e.origin.data(),sizeof(e.origin));std::memcpy(axis,e.axis.data(),sizeof(e.axis));return true;
        }
        auto& e=entries_[next_];next_=(next_+1)%entries_.size();
        e.key=frame.camera.key;e.entity=entity;e.epoch=frame.input.epoch;e.generation=frame.input.generation;e.role=role;
        e.frame=frame;
        std::memcpy(e.origin.data(),origin,sizeof(e.origin));std::memcpy(e.axis.data(),axis,sizeof(e.axis));return false;
    }
};
}
