#include "../src/weapon/AerWeaponSource.h"
#include <cstdlib>
#include <thread>
void check(bool ok){if(!ok)std::abort();}
bool near(float a,float b){return std::abs(a-b)<.0001f;}
int main(){
    using namespace kharvox;
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
