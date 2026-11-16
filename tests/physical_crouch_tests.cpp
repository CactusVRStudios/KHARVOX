#include "../src/camera/BodyStanceHeight.h"
#include "../src/openxr/PhysicalCrouchPolicy.h"
#include <cstdlib>
#include <limits>

namespace {
void check(bool value){if(!value)std::abort();}
void skippedFeedbackPreservesCommands(){
    kharvox::PhysicalCrouchCommand full,gated;
    std::uint32_t random=0x18474191;
    std::int64_t time{};
    auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    // Compare every combination of native stance, tracking/UI eligibility,
    // request, manual button and jump against the previous unconditional
    // feedback behavior, from thousands of reachable command states.
    for(unsigned step=0;step<4096;++step){
        for(unsigned bits=0;bits<64;++bits){
            auto a=full,b=gated;
            const bool requested=bits&1,gameplay=bits&2,manual=bits&4,
                jump=bits&8,valid=bits&16,crouched=bits&32;
            const bool feedback=b.needsStanceFeedback(requested);
            check(a.update(time,requested,gameplay,manual,jump,valid,crouched)
                ==b.update(time,requested,gameplay,manual,jump,feedback&&valid,feedback&&crouched));
            check(a.ownsCrouch()==b.ownsCrouch());
            // Observe a later return-to-standing, catching hidden latch/pulse
            // state differences rather than checking only the first output.
            check(a.update(time+50000000,false,true,false,false,true,true)
                ==b.update(time+50000000,false,true,false,false,true,true));
            check(a.ownsCrouch()==b.ownsCrouch());
        }
        const auto bits=next();
        const bool request=bits&1,feedback=gated.needsStanceFeedback(request);
        check(full.update(time,request,bits&2,bits&4,bits&8,bits&16,bits&32)
            ==gated.update(time,request,bits&2,bits&4,bits&8,feedback&&(bits&16),feedback&&(bits&32)));
        check(full.ownsCrouch()==gated.ownsCrouch());
        time+=(1+next()%500)*1000000LL;
    }
}
}

int main() {
    skippedFeedbackPreservesCommands();
    kharvox::BodyStanceHeight posture;
    std::uint64_t frame{};
    auto sample=[&](float h,bool enabled=true,bool gameplay=true,bool tracked=true){
        return posture.updatePhysicalCrouch(h,enabled,gameplay,tracked,++frame);
    };
    check(!sample(1.80f));
    check(!sample(1.46f));check(!sample(1.44f));
    check(!posture.updatePhysicalCrouch(1.44f,true,true,true,frame)); // second eye
    check(sample(1.44f));
    for(int i=0;i<8;++i)check(sample(i%2?1.48f:1.55f)); // hysteresis
    check(sample(1.61f));check(!sample(1.61f));
    check(!sample(1.44f));check(!sample(1.80f)); // isolated tracking spike
    check(!sample(1.44f));check(sample(1.44f));
    check(!sample(1.44f,true,false)); // menus/cinematics release request
    check(!sample(1.10f,true,false)); // never recalibrate while bent in a menu
    float physicalStanding{};
    check(posture.physicalStandingHeight(physicalStanding)&&std::abs(physicalStanding-1.80f)<.0001f);
    check(sample(1.44f)); // resume crouched without a false stand toggle
    check(sample(1.80f));check(!sample(1.80f));
    check(!sample(1.44f));check(sample(1.44f));
    check(!sample(1.44f,true,true,false));
    check(!sample(std::numeric_limits<float>::quiet_NaN()));
    check(sample(1.44f)); // recovery retains the latched request
    check(sample(1.80f));check(!sample(1.80f));
    check(!sample(1.44f,false));
    check(!sample(1.90f)); // enabling starts a new standing calibration
    check(!sample(1.54f));check(sample(1.54f));
    posture.resetPhysicalCrouch(); // LOCAL reference-space recenter
    check(!sample(-.10f));check(!sample(-.46f));check(sample(-.46f));

    // Native world height may change during stairs/jumps/lifts without
    // changing the headset's LOCAL y. The detectors share no world input.
    kharvox::BodyStanceHeight native;
    native.update(87,87,true,false,0,0);
    float measured{},standing{};
    for(unsigned i=1;i<=5;++i){
        native.update(87,87,false,false,i,float(i)*10);
        check(native.stanceHeights(measured,standing)&&measured==87&&standing==87);
    }
    // Engine crouch and HMD descent must not lower the camera twice.
    native.update(55,87,false,false,6,50);
    native.update(55,87,false,false,7,50);
    native.update(55,87,false,false,8,50);
    native.update(55,87,false,false,9,50);
    check(native.stanceHeights(measured,standing)&&measured==55&&standing==87);
    check(native.cameraHeight(true,false)==87);
    check(native.cameraHeight(false,false)==87); // stand blocked by ceiling
    check(native.cameraHeight(false,true)==55); // manual crouch owns camera
    check(native.cameraHeight(false,false)==55);
    native.update(87,55,false,false,10,50);
    native.update(87,55,false,false,11,50);
    check(native.cameraHeight(false,false)==87);

    constexpr std::int64_t ms=1000000;
    kharvox::PhysicalCrouchCommand command;
    check(command.update(0,true,true,false,false,true,false));
    check(command.ownsCrouch());
    check(command.update(50*ms,true,true,false,false,true,false));
    check(!command.update(150*ms,true,true,false,false,true,true));
    check(!command.update(500*ms,true,true,false,false,true,true));
    check(command.update(600*ms,false,true,false,false,true,true)); // stand toggle
    check(!command.ownsCrouch());
    check(!command.update(1100*ms,false,true,false,false,true,true)); // blocked stand: no retry toggles
    command={};
    check(!command.update(0,true,true,false,false,true,true)); // already manually crouched
    check(!command.update(500*ms,false,true,false,false,true,true));
    check(!command.ownsCrouch());
    check(!command.update(600*ms,true,true,false,false,false,false)); // unknown stance
    check(command.update(700*ms,true,true,false,false,true,false));
    check(!command.update(800*ms,true,false,false,false,true,true)); // no menu Cancel
    check(command.update(1200*ms,false,true,false,false,true,true)); // stand after menu
    command={};
    check(command.update(0,true,true,false,false,true,false));
    check(!command.update(500*ms,true,true,true,false,true,true)); // manual takes over
    check(!command.ownsCrouch());
    check(!command.update(1000*ms,true,true,false,false,true,false));
    check(!command.update(1500*ms,false,true,false,false,true,false));
    check(command.update(2000*ms,true,true,false,false,true,false));
    check(!command.update(2100*ms,true,true,false,true,true,true)); // jump takes over
    check(!command.update(2600*ms,true,true,false,false,true,false));
}
