#include "../src/hud/HudLayoutPolicy.h"

#include <cmath>
#include <sstream>
#include "../src/hud/OffhandHudPolicy.h"

namespace {

bool near(float left, float right) {
    return std::fabs(left - right) < 0.0001f;
}

}

int main() {
    using namespace kharvox;

    {
        OffhandHudRenderFrame frame;
        if(frame.usable(0,0))return 120;
        frame.valid=true;frame.present=100;frame.level=3;
        frame.origin={10,20,30};frame.axis={1,0,0,0,1,0,0,0,1};
        if(!frame.usable(100,3)||!frame.usable(101,3))return 121;
        if(frame.usable(99,3)||frame.usable(102,3)||frame.usable(100,4))return 122;
        // Present advancing does not replace the captured render origin with
        // newer physics. A new camera publication can update within a Present.
        auto next=frame;next.origin[0]=12;
        frame=next;
        if(!frame.usable(100,3)||!near(frame.origin[0],12))return 123;
        frame.valid=false;
        if(frame.usable(100,3))return 124;
    }

    {
        if(offhandHudSurface(0xbdcf54,512,300,83)!=0||offhandHudSurface(0xbdcf54,512,300,100)!=1)return 100;
        if(offhandHudSurface(0xbdcf54,512,301,100)!=-1||offhandHudSurface(0xbdcf55,512,300,100)!=-1)return 101;
        OffhandHudConfig config;std::istringstream saved("1 1 8 0 8 0 0 0 .1 4 2 6 0 90 0 .2");
        if(!readOffhandHudConfig(saved,config)||!near(config.modes[1].centimeters[0],4)||!near(config.modes[0].scale,.1f))return 102;
        if(!near(config.modes[2].centimeters[0],config.modes[0].centimeters[0])||!near(config.modes[3].scale,config.modes[1].scale))return 107;
        OffhandHudConfig separate;std::istringstream v2("2 1 8 0 8 0 0 0 .1 4 2 6 0 90 0 .2 15 3 2 10 20 30 .3 1 2 3 4 5 6 .4");
        if(!readOffhandHudConfig(v2,separate)||!near(separate.modes[2].centimeters[0],15)||!near(separate.modes[0].centimeters[0],8)||!near(separate.modes[3].scale,.4f))return 108;
        separate.modes[2].centimeters[0]+=1;
        if(!near(separate.modes[0].centimeters[0],8)||!near(separate.modes[1].centimeters[0],4)||!near(separate.modes[3].centimeters[0],1))return 109;
        std::istringstream invalid("1 1 999 0 0 0 0 0 .1");if(readOffhandHudConfig(invalid,config))return 103;
        float hand[9]{1,0,0,0,1,0,0,0,1},panel[9]{},grip[3]{1,2,3},a[3]{},b[3]{};
        offhandHudBasis(hand,config.modes[0],panel);
        for(int i=0;i<9;++i)if(!near(hand[i],panel[i]))return 104;
        offhandHudOrigin(grip,hand,panel,config.modes[0],0,100,a);
        grip[0]+=5;offhandHudOrigin(grip,hand,panel,config.modes[0],0,100,b);
        if(!near(b[0]-a[0],5)||!near(a[1],16)||!near(a[2],11))return 105;
        // Placement center must not move when panel rotation or scale changes.
        auto rotatedConfig=config.modes[0];rotatedConfig.degrees={25,70,40};rotatedConfig.scale=.8f;
        offhandHudBasis(hand,rotatedConfig,panel);
        float fixed[3]{};grip[0]-=5;offhandHudOrigin(grip,hand,panel,rotatedConfig,0,100,fixed);
        for(int i=0;i<3;++i)if(!near(fixed[i],a[i]))return 110;
        for(float width:{10.f,25.f,50.f}){float corner[3]{},sx{},sy{};
            if(!centeredOffhandHud(fixed,panel,width,512.f/300.f,corner,sx,sy))return 111;
            for(int i=0;i<3;++i)if(!near(corner[i]+.5f*(panel[i]*sx+panel[3+i]*sy),fixed[i]))return 112;
        }
        offhandHudBasis(hand,config.modes[1],panel);if(!near(panel[0],0)||!near(panel[1],1))return 106;
    }

    // Owned health/ammo never fall back to the native screen during sequences.
    if(suppressOffhandHudFallback(true,true,false,false,false,true))return 113;
    if(!suppressOffhandHudFallback(true,false,false,false,false,true))return 114;
    if(!suppressOffhandHudFallback(true,true,true,false,false,true))return 115;
    if(!suppressOffhandHudFallback(true,true,false,true,false,true))return 116;
    if(!suppressOffhandHudFallback(true,true,false,false,true,true))return 117;
    if(!suppressOffhandHudFallback(true,true,false,false,false,false))return 118;
    if(suppressOffhandHudFallback(false,false,true,true,true,false))return 119;

    // The physically reviewed Meta/VDXR projection remains the reference.
    if (!near(selectHudLayoutFit(0.83909965f, 0.966f), calibratedHudLayoutFit)) return 1;

    // SteamXR's wider projection must no longer enlarge or spread the HUD.
    if (!near(selectHudLayoutFit(0.947f, 1.329f), calibratedHudLayoutFit)) return 2;

    // Pixel resolution is deliberately absent from the policy: only angular
    // visibility matters, and a wider surface retains the same calibration.
    if (!near(selectHudLayoutFit(1.20f, 1.20f), calibratedHudLayoutFit)) return 3;

    // A narrower common eye area may shrink the layout to prevent clipping.
    if (!near(selectHudLayoutFit(0.70f, 0.90f), 0.70f)) return 4;
    if (!near(selectHudLayoutFit(0.10f, 0.10f), minimumHudLayoutFit)) return 5;

    // Invalid runtime geometry fails stable at the calibrated reference.
    if (!near(selectHudLayoutFit(NAN, 1.0f), calibratedHudLayoutFit)) return 6;

    return 0;
}
