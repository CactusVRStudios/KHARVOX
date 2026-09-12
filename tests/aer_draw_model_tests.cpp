#include "../src/weapon/AerDrawModel.h"
#include "../src/weapon/AerWeaponSource.h"
#include "../src/camera/AerWorldViewHistory.h"
#include "../src/common/AerCinematicProjection.h"
#include <cstdlib>
#include <limits>
void check(bool ok){if(!ok)std::abort();}
bool near(float a,float b){return std::abs(a-b)<.0001f;}
int main(){
    using namespace kharvox;
    check(cinematicOverrideFov(true,false,120,115,108,110)==std::array<float,2>{108,110});
    check(cinematicOverrideFov(false,false,120,115,108,110)==std::array<float,2>{120,115});
    check(cinematicOverrideFov(true,true,120,115,108,110)==std::array<float,2>{120,115});
    struct V {float x,y,z;};struct P {V position;};
    std::array<P,2> eyes{};eyes[0].position={2.f-.032f,3,4};eyes[1].position={2.f+.032f,3,4};
    anchorCinematicEyePair(eyes,V{7,8,9});
    check(near(eyes[0].position.x,7-.032f)&&near(eyes[1].position.x,7+.032f));
    check(near(eyes[0].position.y,8)&&near(eyes[1].position.z,9));
    check(aerUseSourceQualification(true,false,false,true,false));
    check(aerUseSourceQualification(true,true,false,true,false));
    check(aerUseSourceQualification(true,true,true,false,true));
    check(!aerUseSourceQualification(false,true,false,false,true));
    check(aerUseSourceQualification(true,false,true,true,false));
    check(aerUseSourceQualification(true,false,true,false,false));
    check(!aerUseSourceQualification(true,false,false,true,true));
    check(!aerUseSourceQualification(true,false,false,false,false));
    check(!aerUseSourceQualification(false,false,false,true,false));
    // Cinematic producer recognition requires the rendered FOV, not the native
    // 90-degree startup overwrite. Subsequent eye caches use producer identity.
    AerWorldViewHistory cinematicViews;AerWorldView cv;
    cv.pose={0,0,0,1,0,0,0,1,0,0,0,1};cv.fov={108,110};
    cv.poseId=988;cv.present=992;cv.level=3;cv.context=0xA30CFF;cv.eye=0;cv.domain=1;
    cinematicViews.remember(cv);AerWorldView recognized;
    float nativeFov[2]{90,58.7155f};
    check(cinematicViews.recognize(cv.pose.data(),nativeFov,993,3,recognized)==AerWorldViewResult::UnknownSource);
    check(cinematicViews.recognize(cv.pose.data(),cv.fov.data(),993,3,recognized)==AerWorldViewResult::Unchanged);
    AerSourceWindow cinematicWindow;cinematicWindow.observe({recognized.poseId,recognized.level,recognized.eye,recognized.domain});
    const auto completed=cinematicWindow.take();
    check(completed.valid()&&completed.key.eye==0&&completed.key.poseId==988);
    check(aerSourceCompletesPair(completed.key,true,{988,3,1,1}));
    check(!aerSourceCompletesPair(completed.key,true,{990,3,1,1}));
    check(!aerSourceCompletesPair(completed.key,true,{988,3,1,0}));
    // VEGA switches from a generic cinematic (domain 1) to an observed
    // scripted gameplay camera (domain 2). Old-domain/old-pose eyes cannot pair.
    cv.domain=2;cv.poseId=1001;cv.present=1002;cv.context=0xE3F96A;
    cinematicViews.remember(cv);
    check(cinematicViews.recognize(cv.pose.data(),cv.fov.data(),1003,3,recognized)==AerWorldViewResult::Unchanged);
    check(recognized.domain==2&&recognized.poseId==1001);
    cinematicWindow.observe({recognized.poseId,recognized.level,recognized.eye,recognized.domain});
    const auto scripted=cinematicWindow.take();
    check(scripted.valid()&&scripted.key.domain==2);
    check(aerSourceCompletesPair(scripted.key,true,{1001,3,1,2}));
    check(!aerSourceCompletesPair(scripted.key,true,{1001,3,1,1}));
    check(!aerSourceCompletesPair(scripted.key,true,{999,3,1,2}));
    check(!aerSourceCompletesPair(scripted.key,true,{1001,4,1,2}));
    check(aerDrawSourceEligible(0,0)&&aerDrawSourceEligible(0x10,0)&&aerDrawSourceEligible(0x18,0));
    check(!aerDrawSourceEligible(1,0)&&!aerDrawSourceEligible(2,0)&&!aerDrawSourceEligible(4,0));
    check(!aerDrawSourceEligible(0,0x80));
    AerWeaponSourceTransforms history;AerWeaponFrame frame;
    frame.camera.key={101,3,1};frame.input.epoch=8;frame.input.generation=2;
    float origin[3]{10,20,30},axis[9]{1,0,0,0,1,0,0,0,1};
    check(!history.hold(frame,123,1,origin,axis));
    // A later locomotion/turn source has translated and rotated the same prop.
    frame.camera.key={103,3,1};float next[3]{15,25,30},turn[9]{0,1,0,-1,0,0,0,0,1};
    check(!history.hold(frame,123,1,next,turn));
    float target[12]{};uint64_t matched{};
    check(history.forDraw({103,3,1},origin,axis,target,target+3,matched)==2&&matched==101);
    for(int i=0;i<3;++i)check(target[i]==next[i]);
    for(int i=0;i<9;++i)check(target[3+i]==turn[i]);
    check(history.forDraw({103,3,0},origin,axis,target,target+3,matched)==2);
    check(history.forDraw({103,3,0},next,turn,target,target+3,matched)==1);
    float rounded[3]{10.001f,20,30};float roundedAxis[9];
    std::memcpy(roundedAxis,axis,sizeof(axis));roundedAxis[1]=.000001f;
    check(history.forDraw({103,3,1},rounded,roundedAxis,target,target+3,matched)==2);
    check(target[0]==15&&matched==101);
    // r287 capture: translation is within the bound, but the matrix axis
    // round trip differs by 0.000373364 and used to reject the model.
    roundedAxis[1]=.000373364f;
    check(history.forDraw({103,3,1},rounded,roundedAxis,target,target+3,matched)==2);
    check(target[0]==15&&matched==101);
    check(history.forDraw({103,3,0},rounded,roundedAxis,target,target+3,matched)==2);
    rounded[0]=10.01f;
    check(history.forDraw({103,3,1},rounded,roundedAxis,target,target+3,matched)==0);
    roundedAxis[1]=std::numeric_limits<float>::quiet_NaN();
    check(history.forDraw({103,3,1},rounded,roundedAxis,target,target+3,matched)==0);
    rounded[0]=10.001f;roundedAxis[1]=.001f;
    check(history.forDraw({103,3,1},rounded,roundedAxis,target,target+3,matched)==0);
    check(history.forDraw({105,3,0},next,turn,target,target+3,matched)==3); // target not built
    check(history.forDraw({103,4,0},next,turn,target,target+3,matched)==0); // level
    check(history.forDraw({103,3,0,1},next,turn,target,target+3,matched)==0); // authored cinematic
    float unknown[3]{900,800,700};
    check(history.forDraw({103,3,0},unknown,turn,target,target+3,matched)==0);
    // Never guess between entities that shared an earlier placement.
    frame.camera.key={101,3,1};check(!history.hold(frame,456,1,origin,axis));
    frame.camera.key={103,3,1};check(!history.hold(frame,456,1,unknown,turn));
    check(history.forDraw({103,3,0},origin,axis,target,target+3,matched)==4);
    roundedAxis[1]=.000001f;
    check(history.forDraw({103,3,0},rounded,roundedAxis,target,target+3,matched)==4);
    // Cached model matrix may be stale even when the exposed origin is current.
    float oldMatrix[16]{},matrix[16]{},scale[3]{2,3,4};
    check(aerDrawModelMatrix(origin,axis,scale,oldMatrix));
    check(aerDrawModelMatrix(next,turn,scale,matrix));
    const float point[4]{1,2,3,1};float result[3]{};
    for(int r=0;r<3;++r)for(int c=0;c<4;++c)result[r]+=matrix[r*4+c]*point[c];
    check(near(result[0],9)&&near(result[1],27)&&near(result[2],42));
    check(oldMatrix[3]==10&&matrix[3]==15&&matrix[15]==1);
    scale[0]=std::numeric_limits<float>::quiet_NaN();check(!aerDrawModelMatrix(next,turn,scale,matrix));
    // Equal pose ID and eye separation is insufficient at camera-mode changes.
    check(!aerSourceCompletesPair({103,3,0,1},true,{103,3,1,0}));
    check(aerSourceCompletesPair({103,3,0,1},true,{103,3,1,1}));
    AerSourceWindow window;window.observe({103,3,1,0});window.observe({103,3,1,1});
    check(window.take().ambiguous);
    AerWorldViewHistory views;AerWorldView v;
    v.pose={0,0,0,1,0,0,0,1,0,0,0,1};v.fov={100,90};v.poseId=103;v.present=50;v.context=99;v.eye=0;v.level=3;
    views.remember(v);v.domain=1;views.remember(v);AerWorldView source;
    check(views.recognize(v.pose.data(),v.fov.data(),50,3,source)==AerWorldViewResult::AmbiguousSource);
    // Both cinematic eyes retain their first anchor despite a mode transition.
    AerInputHistory<std::array<float,3>> anchors;std::array<float,3> anchor{1,2,3},held{};
    anchors.remember({103,3,0,1},anchor);anchors.remember({105,3,0,1},{8,9,10});
    check(anchors.find({103,3,0,1},held)&&held==anchor);
    // Right-eye draw precedes the next engine placement: move the sampled
    // mount with the recorded controller, without requiring a future hold().
    AerWeaponSourceTransforms pending;AerWeaponFrame old;
    old.camera.key={201,3,1};old.camera.bodyAxis={1,0,0,0,1,0,0,0,1};
    old.input.valid=true;old.input.epoch=9;old.input.generation=2;old.input.grip={10,0,0};
    float mounted[3]{12,0,0},scaled[9]{2,0,0,0,3,0,0,0,4};
    check(!pending.hold(old,77,1,mounted,scaled));
    check(!pending.hold(old,78,0,mounted,scaled)); // root and prop can coincide
    auto current=old;current.camera.key.poseId=203;current.camera.bodyOrigin={100,20,0};
    current.camera.bodyAxis={0,1,0,-1,0,0,0,0,1};
    check(pending.forDraw(current.camera.key,mounted,scaled,target,target+3,matched,&current)==6);
    check(near(target[0],100)&&near(target[1],32)&&near(target[2],0));
    check(near(target[3],0)&&near(target[4],2)&&near(target[6],-3)&&near(target[11],4));
    // r289: an established model/asset identity survives a small subsequent
    // copy difference, including coincident root and prop identities.
    AerWeaponSourceTransforms bound;
    check(!bound.hold(old,77,1,mounted,scaled));
    check(!bound.hold(old,78,0,mounted,scaled));
    bool recovered=false;
    check(bound.forDraw(current.camera.key,mounted,scaled,target,target+3,matched,&current,500,600,&recovered)==6&&!recovered);
    float copyOrigin[3]{12.01f,0,0},copyAxis[9];std::memcpy(copyAxis,scaled,sizeof(scaled));copyAxis[1]=.001f;
    check(bound.forDraw(current.camera.key,copyOrigin,copyAxis,target,target+3,matched,&current,500,600,&recovered)==2&&recovered);
    check(near(target[0],100)&&near(target[1],32));
    check(bound.forDraw(current.camera.key,copyOrigin,copyAxis,target,target+3,matched,&current,501,600,&recovered)==0&&!recovered);
    check(bound.forDraw(current.camera.key,copyOrigin,copyAxis,target,target+3,matched,&current,500,601,&recovered)==0&&!recovered);
    auto reset=current;reset.input.epoch++;
    check(bound.forDraw(reset.camera.key,copyOrigin,copyAxis,target,target+3,matched,&reset,500,600,&recovered)==0);
    reset=current;reset.input.generation++;
    check(bound.forDraw(reset.camera.key,copyOrigin,copyAxis,target,target+3,matched,&reset,500,600,&recovered)==0);
    copyOrigin[0]=13;
    check(bound.forDraw(current.camera.key,copyOrigin,copyAxis,target,target+3,matched,&current,500,600,&recovered)==0);
    copyOrigin[0]=12.01f;
    auto laterBound=current;laterBound.camera.key.poseId=205;
    check(bound.forDraw(laterBound.camera.key,copyOrigin,copyAxis,target,target+3,matched,&laterBound,500,600,&recovered)==6&&recovered);
    laterBound.camera.key.poseId=211;
    check(bound.forDraw(laterBound.camera.key,copyOrigin,copyAxis,target,target+3,matched,&laterBound,500,600,&recovered)==0&&!recovered);
    // Native animation must advance even after an early draw. Only the draw
    // snapshot is held for the left eye; it never writes back into the engine.
    float lateAnimation[12]{999,999,999,1,0,0,0,1,0,0,0,1};
    check(!pending.hold(current,77,1,lateAnimation,lateAnimation+3));
    check(lateAnimation[0]==999);
    check(!pending.hold(current,78,0,lateAnimation,lateAnimation+3));
    current.camera.key.eye=0;
    float left[12]{};
    check(pending.forDraw(current.camera.key,lateAnimation,lateAnimation+3,left,left+3,matched,&current)==2);
    for(int i=0;i<12;++i)check(near(left[i],target[i]));
    // A body-basis update and its inverse local yaw leave world placement fixed.
    current.camera.key.poseId=205;current.camera.bodyYawDelta=90;current.camera.bodyOrigin={0,0,0};
    check(rebaseAerDrawPlacement(old,current,mounted,scaled,target,target+3));
    for(int i=0;i<3;++i)check(near(target[i],mounted[i]));
    for(int i=0;i<9;++i)check(near(target[i+3],scaled[i]));
    current=old;current.input.orientation={0,std::sqrt(.5f),0,std::sqrt(.5f)};
    check(rebaseAerDrawPlacement(old,current,mounted,scaled,target,target+3));
    check(near(target[0],10)&&near(target[1],2)); // pivot offset rotates around grip
    current.input.epoch++;check(!rebaseAerDrawPlacement(old,current,mounted,scaled,target,target+3));
    current=old;current.input.generation++;check(!rebaseAerDrawPlacement(old,current,mounted,scaled,target,target+3));
    current=old;current.camera.key.level++;check(!rebaseAerDrawPlacement(old,current,mounted,scaled,target,target+3));
    current=old;current.input.valid=false;check(!rebaseAerDrawPlacement(old,current,mounted,scaled,target,target+3));
    AerWeaponSourceHistory recorded;recorded.remember(201,3,old.input);old.camera.present=50;recorded.camera(old.camera);
    AerWeaponFrame retrieved;
    check(recorded.frame(old.camera.key,51,retrieved));
    check(!recorded.frame(old.camera.key,49,retrieved));
    check(!recorded.frame(old.camera.key,54,retrieved));
    check(!recorded.frame({203,3,1},51,retrieved));
    // Repeated equal model poses must use the nearest predecessor's mount,
    // not the oldest ring entry or a controller sample from the future.
    AerWeaponSourceTransforms repeated;old.camera.key={301,3,1};old.input.grip={10,0,0};
    check(!repeated.hold(old,88,1,mounted,scaled));
    auto closer=old;closer.camera.key.poseId=303;closer.input.grip={11,0,0};
    check(!repeated.hold(closer,88,1,mounted,scaled));
    auto later=old;later.camera.key.poseId=307;later.input.grip={50,0,0};
    check(!repeated.hold(later,88,1,mounted,scaled));
    current=old;current.camera.key.poseId=305;current.input.grip={20,0,0};
    check(repeated.forDraw(current.camera.key,mounted,scaled,target,target+3,matched,&current)==6);
    check(matched==303&&near(target[0],21));
    current.input.epoch++;
    check(repeated.forDraw(current.camera.key,mounted,scaled,target,target+3,matched,&current)==0);
    AerWeaponSourceTransforms ambiguous;
    check(!ambiguous.hold(old,88,1,mounted,scaled));
    check(!ambiguous.hold(closer,89,1,mounted,scaled));
    current.input.epoch=old.input.epoch;
    check(ambiguous.forDraw(current.camera.key,mounted,scaled,target,target+3,matched,&current)==4);
    // Repeated early draws cannot perpetuate yesterday's wall/mount animation.
    AerWeaponSourceTransforms animation;auto anim=old;anim.camera.key={401,3,1};
    float native[3]{12,0,0};check(!animation.hold(anim,90,1,native,axis));
    for(unsigned i=1;i<20;++i){
        const float previous=native[0];anim.camera.key.poseId+=2;
        check(animation.forDraw(anim.camera.key,native,axis,target,target+3,matched,&anim)==6);
        check(near(target[0],previous));
        native[0]=i%2?30.f:12.f; // wall deflection then release
        check(!animation.hold(anim,90,1,native,axis));
        auto other=anim;other.camera.key.eye=0;
        check(animation.forDraw(other.camera.key,native,axis,left,left+3,matched,&other)==2);
        check(near(left[0],previous));
    }
    anim.camera.key.poseId+=2;anim.input.epoch++;
    check(animation.forDraw(anim.camera.key,native,axis,target,target+3,matched,&anim)==0);
    native[0]=17;check(!animation.hold(anim,90,1,native,axis));
    check(animation.forDraw(anim.camera.key,native,axis,target,target+3,matched,&anim)==1);
    check(target[0]==17); // reused entity after weapon switch gets its new mount
}
