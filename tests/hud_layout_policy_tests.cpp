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
        if(!near(b[0]-a[0],5)||!near(a[1],9)||!near(a[2],11))return 105;
        offhandHudBasis(hand,config.modes[1],panel);if(!near(panel[0],0)||!near(panel[1],1))return 106;
    }

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
