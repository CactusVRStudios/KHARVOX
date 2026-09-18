#include "../src/weapon/AerWeaponSource.h"
#include <cstdlib>
#include <thread>
void check(bool ok){if(!ok)std::abort();}
bool near(float a,float b){return std::abs(a-b)<.0001f;}
int main(){
    using namespace kharvox;
    {
        // A physics tick can lead the view by one walking step or jump over
        // a pipe while the camera eases vertically. The VR grip relative to
        // that rendered camera must not move with the unrelated physics tick.
        AerWeaponFrame source{};source.input.valid=true;source.camera.key={3000,7,0};
        source.camera.bodyAxis={1,0,0,0,1,0,0,0,1};source.input.grip={12,4,-8};
        float axis[9]{},grip[3]{};
        for(int n=0;n<12;++n){
            const float camera[3]{float(n*4),100,float(87+n)};
            const float physics[3]{camera[0]+(n%3==1?4.f:0.f),100,camera[2]+(n%3==1?16.f:0.f)};
            bindAerWeaponBodyOrigin(source.camera,true,camera,physics);
            check(aerControllerWorldFrame(source,grip,axis));
            for(int i=0;i<3;++i)check(near(grip[i]-camera[i],source.input.grip[i]));
            source.camera.renderOrigin={camera[0]+2,camera[1]-1,camera[2]+3};
            source.camera.renderOriginValid=true;
            auto draw=source;const float actualView[3]{camera[0]+6,camera[1]-1,camera[2]+3};
            check(alignAerWeaponDrawCamera(draw,actualView));
            check(aerControllerWorldFrame(draw,grip,axis));
            check(near(grip[0]-actualView[0],10)); // grip 12 minus room-scale 2
            bindAerWeaponBodyOrigin(source.camera,false,camera,physics);
            check(source.camera.bodyOrigin[0]==physics[0]&&source.camera.bodyOrigin[2]==physics[2]);
        }
    }
    {
        AerWeaponDrawFrames draws;
        AerWeaponFrame f{};f.camera.key={2000,9,0};f.input.valid=true;
        f.camera.renderOrigin={20,30,40};f.camera.bodyOrigin={20,30,35};
        f.input.sampleQpc=100;check(draws.latch(f));
        auto republished=f;republished.camera.bodyOrigin[2]=38;republished.input.sampleQpc=200;
        check(!draws.latch(republished));
        check(republished.camera.bodyOrigin[2]==35&&republished.input.sampleQpc==100);
        // Fresh tracking at 120 Hz is never held for a two-eye/60 Hz pair,
        // even when engine animation/body data only changes every other frame.
        for(unsigned i=1;i<=120;++i){
            auto next=f;next.camera.key.poseId+=i;next.input.sampleQpc+=i;
            next.camera.bodyOrigin[0]+=float(i/2);
            check(draws.latch(next));check(next.input.sampleQpc==100+i);
        }
        auto reset=f;reset.input.epoch=1;check(draws.latch(reset));
        reset.input.generation=1;check(draws.latch(reset));
        reset.camera.renderOrigin[0]+=1;check(draws.latch(reset));
        float viewAxis[9]{1,0,0,0,1,0,0,0,1};
        check(draws.latch(reset,viewAxis));check(!draws.latch(reset,viewAxis));
        viewAxis[0]=0;viewAxis[1]=1;viewAxis[3]=-1;viewAxis[4]=0;
        check(draws.latch(reset,viewAxis)); // same position, different real view
    }
    AerWeaponSourceHistory h;AerWeaponInput input;
    input.valid=true;input.generation=1;input.epoch=7;input.grip={10,20,30};
    input.sampleQpc=1234;
    h.remember(771,2,input);
    auto newer=input;newer.grip={100,200,300};
    newer.sampleQpc=5678;
    check(h.remember(771,2,newer).sampleQpc==1234);
    check(h.remember(771,2,newer).grip==input.grip); // second eye keeps pair's input
    h.remember(773,2,newer); // programming next pair cannot change source 771
    AerWeaponCamera camera;camera.key={771,2,1};camera.present=40;
    camera.bodyAxis={1,0,0,0,1,0,0,0,1};camera.headAxis=camera.bodyAxis;
    h.camera(camera);AerWeaponFrame frame;
    check(h.resolve(40,2,7,1,frame)&&frame.input.grip==input.grip&&frame.camera.key.eye==1);
    camera.key.eye=0;camera.present=41;h.camera(camera);
    check(h.resolve(41,2,7,1,frame)&&frame.input.grip==input.grip&&frame.camera.key.eye==0);
    check(aerWeaponPropSourceMatches(frame,41,41,2,7,1));
    check(!aerWeaponPropSourceMatches(frame,41,42,2,7,1)); // prior root cannot label next prop
    check(!aerWeaponPropSourceMatches(frame,41,41,3,7,1));
    check(!aerWeaponPropSourceMatches(frame,41,41,2,8,1));
    check(!aerWeaponPropSourceMatches(frame,41,41,2,7,2));
    // r282 tester failures: cameraPresent 1141 versus root Present 1143.
    // Both delayed root and its same-Present prop must retain the source.
    check(h.resolve(43,2,7,1,frame));
    check(aerWeaponPropSourceMatches(frame,43,43,2,7,1));
    check(h.resolve(44,2,7,1,frame));
    check(aerWeaponPropSourceMatches(frame,44,44,2,7,1));
    check(!aerWeaponPropSourceMatches(frame,44,45,2,7,1));
    check(!h.resolve(40,2,7,1,frame)); // future camera
    // A worker sampled 40 before waiting for the history lock; the camera
    // was published at 41 meanwhile. Sampling inside the lock accepts it.
    AerWeaponResolveDiagnostic diagnostic;
    check(h.resolveCurrent([]{return 41u;},2,7,1,frame,diagnostic));
    check(diagnostic.failure==AerWeaponResolveFailure::None);
    check(!h.resolveCurrent([]{return 45u;},2,7,1,frame,diagnostic));
    check(diagnostic.failure==AerWeaponResolveFailure::Stale);
    check(!h.resolve(45,2,7,1,frame)); // stale camera
    check(!h.resolve(41,3,7,1,frame)); // different level
    check(!h.resolve(41,2,8,1,frame)); // weapon/reset epoch
    check(!h.resolve(41,2,7,2,frame)); // calibration
    camera.key.poseId=775;h.camera(camera);check(!h.resolve(41,2,7,1,frame)); // missing input
    auto invalid=input;invalid.valid=false;h.remember(775,2,invalid);
    check(!h.resolve(41,2,7,1,frame));
    // A placeholder registered while tracking was disabled must not poison
    // this pair forever. The first valid sample repairs it, then stays frozen
    // across both eyes even if the latest global tracking sample disappears.
    check(h.remember(775,2,input).valid);
    check(h.resolve(41,2,7,1,frame)&&frame.input.grip==input.grip);
    check(h.remember(775,2,invalid).valid);
    check(h.remember(775,2,newer).grip==input.grip);
    camera.key.eye=1;h.camera(camera);
    check(h.resolve(41,2,7,1,frame)&&frame.input.grip==input.grip);
    check(!h.resolve(41,2,8,1,frame)); // actual reset still rejects it
    check(!h.resolve(41,3,7,1,frame)); // level transition still rejects it

    {
        // Uneven ground changes the animated grip after the root used the
        // preceding update's joint. Completed prop must stay on the controller.
        AerWeaponFrame f{};f.input.valid=true;f.input.grip={10,20,30};
        f.camera.bodyAxis={0,1,0,-1,0,0,0,0,1};f.camera.bodyOrigin={100,200,300};
        float grip[3]{},controllerAxis[9]{};check(aerControllerWorldFrame(f,grip,controllerAxis));
        float rootAxis[9]{0,2,0,-2,0,0,0,0,2}; // rotated and scaled mount
        float adjustment[3]{1,2,3},oldJoint[3]{5,6,7},root[3]{};
        for(int i=0;i<3;++i){root[i]=grip[i];for(int j=0;j<3;++j)root[i]-=rootAxis[j*3+i]*(oldJoint[j]+adjustment[j]);}
        for(float step:{0.f,5.f,-3.f,8.f,0.f}){
            float joint[3]{5+step,6,7+step},prop[3]{},delta[3]{};
            for(int i=0;i<3;++i){prop[i]=root[i];for(int j=0;j<3;++j)prop[i]+=rootAxis[j*3+i]*joint[j];}
            check(correctAerPropGrip(f,root,rootAxis,joint,adjustment,prop,delta));
            for(int i=0;i<3;++i){float expected=grip[i];for(int j=0;j<3;++j)expected-=rootAxis[j*3+i]*adjustment[j];check(near(prop[i],expected));}
        }
        float invalidJoint[3]{1000,0,0},unchanged[3]{1,2,3};
        check(!correctAerPropGrip(f,root,rootAxis,invalidJoint,adjustment,unchanged,nullptr));
        check(unchanged[0]==1&&unchanged[1]==2&&unchanged[2]==3);
        f.input.valid=false;
        check(!correctAerPropGrip(f,root,rootAxis,oldJoint,adjustment,unchanged,nullptr));
    }
    // Two workers, delayed eyes, different root/prop objects and future pairs.
    AerWeaponSourceTransforms transforms;
    frame.camera.key={771,2,1};frame.input=input;
    float origin[3]{1,2,3},axis[9]{1,0,0,0,1,0,0,0,1};
    std::thread a([&]{check(!transforms.hold(frame,42,0,origin,axis));});a.join();
    // Repeated first-eye work must not move the pair after an earlier draw.
    float repeated[3]{40,50,60};
    float repeatedAxis[9]{0,1,0,-1,0,0,0,0,1};
    check(transforms.hold(frame,42,0,repeated,repeatedAxis));
    check(repeated[0]==1&&repeated[1]==2&&repeated[2]==3);
    for(int i=0;i<9;++i)check(repeatedAxis[i]==axis[i]);
    auto next=frame;next.camera.key.poseId=773;
    float futureOrigin[3]{9,8,7};check(!transforms.hold(next,42,0,futureOrigin,axis));
    check(futureOrigin[0]==9); // wall/animation movement advances next pair
    frame.camera.key.eye=0;origin[0]=100;
    std::thread b([&]{check(transforms.hold(frame,42,0,origin,axis));});b.join();
    check(origin[0]==1&&origin[1]==2&&origin[2]==3);
    origin[0]=200;check(transforms.hold(frame,42,0,origin,axis));
    check(origin[0]==1); // repeated second eye follows the same rule
    float draw[12]{};uint64_t matched{};
    check(transforms.forDraw(frame.camera.key,origin,axis,draw,draw+3,matched,&frame)==1);
    check(draw[0]==1&&matched==771);
    {
        AerWeaponSourceTransforms walking;
        AerWeaponFrame first{};first.camera.key={900,4,0};first.input.valid=true;
        first.camera.bodyAxis={1,0,0,0,1,0,0,0,1};first.camera.headAxis=first.camera.bodyAxis;
        first.camera.renderOriginValid=true;
        float root[3]{12,2,3},out[12]{};uint64_t source{};
        check(!walking.hold(first,99,0,root,axis));
        check(walking.forDraw(first.camera.key,root,axis,out,out+3,source,&first)==1);
        auto moved=first;float cameraOrigin[3]{10,0,0};
        check(alignAerWeaponDrawCamera(moved,cameraOrigin));
        check(walking.forDraw(first.camera.key,root,axis,out,out+3,source,&moved,0,0,nullptr,true)==2);
        check(near(out[0],22)&&near(out[1],2));
        // Repeated draw must not accumulate translation through the cache.
        check(walking.forDraw(first.camera.key,root,axis,out,out+3,source,&moved,0,0,nullptr,true)==2);
        check(near(out[0],22));
        cameraOrigin[0]=20;check(alignAerWeaponDrawCamera(moved,cameraOrigin));
        check(walking.forDraw(first.camera.key,root,axis,out,out+3,source,&moved,0,0,nullptr,true)==2);
        check(near(out[0],32));
        cameraOrigin[0]=200;check(!alignAerWeaponDrawCamera(moved,cameraOrigin));
        check(near(moved.camera.bodyOrigin[0],20));
    }
    {
        // Root and prop share the native copied pose, but their target
        // snapshots straddle a body movement for the same tracking ID.
        auto movementCase=[&](float animationDifference){
            AerWeaponSourceTransforms moving;
            AerWeaponFrame old{};old.camera.key={1000,4,0};old.input.valid=true;
            old.camera.bodyAxis={1,0,0,0,1,0,0,0,1};old.camera.headAxis=old.camera.bodyAxis;
            float native[3]{12,2,3},rootPose[3]{12,2,3},propPose[3]{22+animationDifference,2,3};
            check(!moving.hold(old,99,0,native,axis));
            check(!moving.hold(old,99,1,native,axis));
            auto root=old;root.camera.key.poseId=1001;
            auto prop=root;prop.camera.bodyOrigin[0]=10;
            check(!moving.hold(root,99,0,rootPose,axis));
            check(!moving.hold(prop,99,1,propPose,axis));
            float out[12]{};uint64_t source{};
            check(moving.forDraw(root.camera.key,native,axis,out,out+3,source,&prop)==4);
            for(int repeat=0;repeat<3;++repeat){
                const auto status=moving.forDraw(root.camera.key,native,axis,out,out+3,source,&prop,0,0,nullptr,true);
                if(animationDifference>0)check(status==4); // genuine ambiguity still rejected
                else {check(status==2);check(near(out[0],22)&&near(out[1],2));}
            }
        };
        movementCase(0);
        movementCase(3);
    }
    {
        // Repeated root evaluation can observe a newer body camera for the
        // same tracking ID. Its held transform and metadata must stay paired
        // when a child prop is first recorded after that movement.
        AerWeaponSourceTransforms moving;
        AerWeaponFrame first{};first.camera.key={1100,4,0};first.input.valid=true;
        first.camera.bodyAxis={1,0,0,0,1,0,0,0,1};
        float root[3]{12,2,3};AerWeaponFrame held{};
        check(!moving.hold(first,99,0,root,axis,&held));
        auto current=first;current.camera.bodyOrigin[0]=10;
        root[0]=22;
        check(moving.hold(current,99,0,root,axis,&held));
        check(near(root[0],12)&&near(held.camera.bodyOrigin[0],0));
        check(!moving.hold(held,100,1,root,axis));
        for(int repeat=0;repeat<3;++repeat){
            float out[12]{};uint64_t source{};
            const auto status=moving.forDraw(current.camera.key,root,axis,out,out+3,source,&current,0,0,nullptr,true);
            check(status==2&&near(out[0],22)&&near(out[1],2));
        }
    }
    origin[0]=50;check(!transforms.hold(frame,42,1,origin,axis)); // prop role distinct
    check(!transforms.hold(frame,43,0,origin,axis)); // object identity
    frame.camera.key.level=3;check(!transforms.hold(frame,42,0,origin,axis));
    frame.camera.key.level=2;frame.input.epoch=8;check(!transforms.hold(frame,42,0,origin,axis));
    frame.input.epoch=7;frame.input.generation=2;check(!transforms.hold(frame,42,0,origin,axis));

    // A 90-degree newer body yaw changes local values, not world position.
    input.grip={10,0,3};input.baseline={5,0,1};
    const auto rebased=rebaseAerWeaponInput(input,90);
    check(near(rebased.grip[0],0)&&near(rebased.grip[1],-10)&&near(rebased.grip[2],3));
    check(near(-rebased.grip[1],input.grip[0])&&near(rebased.grip[0],input.grip[1]));
    check(near(rebased.orientation[1],-std::sqrt(.5f))&&near(rebased.orientation[3],std::sqrt(.5f)));
    const auto back=rebaseAerWeaponInput(rebased,-90);
    for(int i=0;i<3;++i)check(near(back.grip[i],input.grip[i])&&near(back.baseline[i],input.baseline[i]));
    for(int i=0;i<4;++i)check(near(back.orientation[i],input.orientation[i]));
}
