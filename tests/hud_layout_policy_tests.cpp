#include "../src/hud/HudLayoutPolicy.h"
#include "../src/hud/WeaponWheelHudPolicy.h"

#include <cmath>
#include <sstream>
#include <fstream>
#include "../src/hud/OffhandHudPolicy.h"
#include "../src/hands/WeaponSupportPolicy.h"

namespace {

bool near(float left, float right) {
    return std::fabs(left - right) < 0.0001f;
}

}

int main(int argc,char** argv) {
    if(argc>2)return 190;
    if(argc==2){
        std::ifstream input(argv[1]);kharvox::OffhandHudConfig config;
        if(!input||!kharvox::readOffhandHudConfig(input,config)||!config.watchReading
            ||!near(config.armourGapCm,3.5f))return 191;
        for(const auto& bounds:config.armourBounds)for(int i=0;i<3;++i)
            if(bounds.minimum[i]>=bounds.maximum[i])return 192;
    }
    using namespace kharvox;
    {
        // Original reading: no reflection or watch rotation, independent of
        // viewer/eye. Exercise both physical hands, all panel calibrations,
        // wrist orientations and the existing native support-wrist mapping.
        constexpr float modes[6][7]{
            {-9.5f,-5,24.5f,-65,90,15,.4f},{-10,-14,0,-75,-65,20,.4f},
            {-4.5f,20.5f,17.5f,-65,60,15,.4f},{-13,12.5f,11,-55,-100,0,.4f},
            {-11.5f,7.5f,18,-130,95,-50,.4f},{-18,-1,6,-85,-65,30,.4f}};
        const OffhandHudBounds bounds{{-16,-12,-18},{10,20,37}};
        constexpr float units=39.3701f;
        const float identity[9]{1,0,0,0,1,0,0,0,1};
        for(int mode=0;mode<6;++mode)for(int angle:{0,45,90,180,270})
            for(bool locked:{false,true})for(float reference:{-1.f,1.f}){
            OffhandHudCalibration values;
            for(int i=0;i<3;++i){values.centimeters[i]=modes[mode][i];values.degrees[i]=modes[mode][3+i];}
            OffhandHudCalibration wrist;wrist.degrees={float(angle),float(angle/2),float(-angle/3)};
            float hand[9]{},grip[3]{10,20,30};offhandHudBasis(identity,wrist,hand);
            if(locked){
                hands::SupportHudGeometry geometry;geometry.valid=true;
                geometry.metersPerAssetUnit=.02f;geometry.anchor={22,-2,3};
                geometry.gripToAsset=hands::math::scale(50);
                const float degrees[]{float(angle),float(angle/2),float(-angle/3)};
                auto weapon=hands::math::multiply(hands::math::translation(10,20,30),
                    hands::math::rotationDegrees(degrees));
                if(!hands::supportHudFrame(weapon,geometry,units,grip,hand))return 212;
            }
            float axis[9]{},center[3]{};
            offhandHudBasis(hand,values,axis);
            offhandHudOrigin(grip,hand,axis,values,mode/2,units,center);
            const std::array<float,9> beforeAxis{axis[0],axis[1],axis[2],axis[3],axis[4],axis[5],axis[6],axis[7],axis[8]};
            const std::array<float,3> before{center[0],center[1],center[2]};
            if(!clearOriginalOffhandHud(center,axis,grip,hand,bounds,reference,2,units,25))return 213;
            for(int i=0;i<9;++i)if(axis[i]!=beforeAxis[i])return 214;
            float normal[3]{axis[1]*axis[5]-axis[2]*axis[4],axis[2]*axis[3]-axis[0]*axis[5],axis[0]*axis[4]-axis[1]*axis[3]};
            for(float& v:normal)v*=reference>0?-1.f:1.f;
            for(int row=0;row<2;++row){float tangent{};
                for(int i=0;i<3;++i)tangent+=(center[i]-before[i])*axis[row*3+i];
                if(std::abs(tangent)>.0001f)return 215;
            }
            for(int corner=0;corner<8;++corner){float distance{};
                for(int i=0;i<3;++i){float point=grip[i];
                    for(int j=0;j<3;++j)point+=hand[j*3+i]*((corner&(1<<j))?bounds.maximum[j]:bounds.minimum[j])*units*.01f;
                    distance+=(center[i]-point)*normal[i];}
                if(distance<2.f*units*.01f-.0001f)return 216;
            }
            const std::array<float,3> once{center[0],center[1],center[2]};
            if(!clearOriginalOffhandHud(center,axis,grip,hand,bounds,reference,2,units,25))return 217;
            for(int i=0;i<3;++i)if(!near(center[i],once[i]))return 218;
            for(float eyeOffset:{-1.28f,1.28f}){
                const float eye[3]{eyeOffset,0,0},forward[3]{0,0,1};float nearest{};
                if(!offhandHudNearestDepth(center,axis,25,512.f/300.f,eye,forward,nearest))return 219;
                for(int i=0;i<3;++i)if(!near(center[i],once[i]))return 220;
            }
        }
        // Invalid native reference cannot leave a partial transform.
        float center[3]{},axis[9]{1,0,0,0,1,0,0,0,1},grip[3]{};
        if(clearOriginalOffhandHud(center,axis,grip,identity,bounds,0,2,units)
            ||center[2]!=0||axis[0]!=1)return 221;
        // Far-away forearm sections do not affect this panel's clearance.
        OffhandHudBounds sectioned=bounds;sectioned.sectionMask=3;
        sectioned.sections[0]={-4,-4,-1,4,4,1};
        sectioned.sections[1]={80,-4,-1,100,4,37};
        if(!clearOriginalOffhandHud(center,axis,grip,identity,sectioned,-1,2,100,25)
            ||!near(center[2],3))return 222;
    }
    {
        // A remote part of the forearm must not lift a panel by 30 cm when
        // the armor directly under its footprint needs only 3 cm clearance.
        OffhandHudBounds bounds{{-100,-100,-2},{100,100,30}};
        bounds.sectionMask=3;
        bounds.sections[0]={-4,-4,-1,4,4,1};
        bounds.sections[1]={80,-4,-1,100,4,30};
        const float hand[9]{1,0,0,0,1,0,0,0,1},grip[3]{},eye[3]{0,0,100};
        float axis[9]{1,0,0,0,1,0,0,0,1},center[3]{};
        if(!watchOffhandHud(center,axis,grip,hand,bounds,eye,2,100,false,25)
            ||!near(center[2],3))return 200;
        center[0]=90;center[2]=0;
        if(!watchOffhandHud(center,axis,grip,hand,bounds,eye,2,100,false,25)
            ||!near(center[2],32))return 201;
        center[0]=40;center[2]=0;
        if(!watchOffhandHud(center,axis,grip,hand,bounds,eye,2,100,false,25)
            ||!near(center[2],0))return 202;
        // Missing section metadata retains the compatible conservative path.
        bounds.sectionMask=0;center[0]=0;
        if(!watchOffhandHud(center,axis,grip,hand,bounds,eye,2,100,false,25)
            ||!near(center[2],32))return 203;
    }
    {
        const float native[9]{0,-2,0,0,0,-3,4,0,0},forward[3]{1,0,0},center[3]{};
        const float reference=offhandHudPixelFacing(native,forward);
        if(!near(reference,1))return 204;
        for(float sign:{-1.f,1.f})for(float eyeX:{-3.2f,3.2f}){
            float axis[9]{2,0,0,0,3,0,0,0,4};const float eye[3]{eyeX,0,sign*100};
            const bool changed=faceOriginalOffhandHud(axis,center,eye,reference);
            if(changed!=(sign>0)||!near(axis[4],3)||!near(axis[0]*axis[4]*axis[8],24))return 205;
            if(offhandHudPixelFacing(axis,eye)*reference>=0)return 206;
            if(faceOriginalOffhandHud(axis,center,eye,reference))return 207;
            float origin[3]{},x{},y{};
            if(!centeredOffhandHud(center,axis,25,512.f/300.f,origin,x,y))return 208;
            for(int i=0;i<3;++i)if(!near(origin[i]+.5f*(axis[i]*x+axis[3+i]*y),center[i]))return 209;
        }
        float axis[9]{2,0,0,0,3,0,0,0,4};const float eye[3]{0,0,100};
        if(faceOriginalOffhandHud(axis,center,eye,0)||!near(axis[0],2))return 210;
        if(faceOriginalOffhandHud(axis,center,eye,-reference)||!near(axis[0],2))return 211;
    }
    {
        if(!ownedWeaponWheel(0x22456f8)||!ownedWeaponWheel(0x2245608)||ownedWeaponWheel(0x2240978))return 170;
        const float eye[3]{5,6,7},head[9]{0,1,0,-1,0,0,0,0,1};float center[3]{},out[9]{};
        if(!weaponWheelPose(eye,head,100,center)||!near(center[0],5)||!near(center[1],306)||!near(center[2],7))return 171;
        const float native[9]{2,0,0,0,0,-3,0,4,0};
        if(!flatWeaponWheelAxis(native,head,out))return 172;
        for(int i=0;i<9;++i)if(!near(native[i],out[i]))return 173;
        for(int width:{512,1024,2048,4096}){
            float origin[3]{},x=0,y=0;
            if(!centeredOffhandHud(center,out,300,float(width)/float(width*9/16),origin,x,y))return 174;
            for(int i=0;i<3;++i)if(!near(origin[i]+.5f*(out[i]*x+out[3+i]*y),center[i]))return 175;
            if(!near(x*std::abs(out[0]),300))return 176;
        }
    }

    {
        // Values read from live campaign objects, not inferred from type names.
        if(ownedOffhandHudSurface(0x2240888,0xbdcf54,512,300,83)!=0)return 125;
        if(ownedOffhandHudSurface(0x2240978,0xbdcf54,512,300,100)!=1)return 126;
        for(auto owner:{uintptr_t(0),uintptr_t(0x22425d0),uintptr_t(0x2240a68),uintptr_t(0x22412e0)})
            for(int scale:{83,100})if(ownedOffhandHudSurface(owner,0xbdcf54,512,300,scale)!=-1)return 127;
        if(ownedOffhandHudSurface(0x2240978,0xbdcf54,512,300,83)!=-1)return 128;
        const float eye[3]{},forward[3]{1,0,0};
        const float axis[9]{0,1,0,0,0,1,1,0,0};float center[3]{5,2,3};
        float nearest{};
        if(!offhandHudNearestDepth(center,axis,20,2,eye,forward,nearest)||!near(nearest,5)
            ||!near(center[0],5)||!near(center[1],2)||!near(center[2],3))return 129;
        center[0]=40;
        if(!offhandHudNearestDepth(center,axis,20,2,eye,forward,nearest)||!near(nearest,40))return 130;
        const float tilted[9]{1,1,0,-1,1,0,0,0,1};center[0]=5;
        if(!offhandHudNearestDepth(center,tilted,20,2,eye,forward,nearest)
            ||!near(nearest,5-15/std::sqrt(2.f))||!near(center[0],5))return 131;
        bool visible=true;
        for(float depth:{9.f,10.1f,11.9f}){
            visible=offhandHudNearVisible(visible,depth,10,2);if(visible)return 132;
        }
        visible=offhandHudNearVisible(visible,12,10,2);if(!visible)return 133;
        if(!offhandHudNearVisible(visible,10.1f,10,2))return 134;
        const float zero[9]{};
        if(offhandHudNearestDepth(center,zero,20,2,eye,forward,nearest))return 135;
    }

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

    // Legacy calibration migration preserves both accepted panels and seeds
    // an independent ProgMeter pose for each handedness.
    {
        OffhandHudConfig c;
        std::istringstream old("2 1 1 2 3 4 5 6 .4 7 8 9 10 11 12 .5 13 14 15 16 17 18 .6 19 20 21 22 23 24 .7");
        if(!readOffhandHudConfig(old,c)||!near(c.modes[4].centimeters[1],16)||!near(c.modes[5].centimeters[1],22)
            ||!near(c.modes[4].degrees[1],5)||!near(c.modes[5].degrees[1],11)||!near(c.modes[3].scale,.7f))return 140;
        std::ostringstream saved;saved<<"3 1 ";
        c.modes[4].centimeters[0]=31;c.modes[5].centimeters[0]=-42;
        for(const auto& mode:c.modes){for(float x:mode.centimeters)saved<<x<<' ';for(float x:mode.degrees)saved<<x<<' ';saved<<mode.scale<<' ';}
        OffhandHudConfig restored;std::istringstream input(saved.str());
        if(!readOffhandHudConfig(input,restored)||!near(restored.modes[4].centimeters[0],31)||!near(restored.modes[5].centimeters[0],-42))return 141;
        std::istringstream truncated("3 1 0 0 0 0 0 0 .4");
        if(readOffhandHudConfig(truncated,restored)||!near(restored.modes[5].centimeters[0],-42))return 142;
        float hand[9]{1,0,0,0,1,0,0,0,1},grip[3]{},origin[3]{};
        offhandHudOrigin(grip,hand,hand,c.modes[4],2,100,origin);
        if(!near(origin[1],16))return 143;
    }

    {
        // Regression: original reading must retain armor clearance. Turning
        // the watch rotation off must not leave the panel inside Praetor.
        std::ostringstream encoded;encoded<<"4 1 0 2 ";
        for(int i=0;i<6;++i)encoded<<"1 2 3 4 5 6 .4 ";
        encoded<<"-10 -20 -4 10 20 8 -11 -21 -5 11 21 9";
        OffhandHudConfig config;std::istringstream input(encoded.str());
        if(!readOffhandHudConfig(input,config)||config.watchReading||!near(config.armourGapCm,2))return 195;
        for(int handed=0;handed<2;++handed)for(float sign:{-1.f,1.f}){
            float center[3]{},axis[9]{2,0,0,0,3,0,0,0,4};
            const float grip[3]{},hand[9]{1,0,0,0,1,0,0,0,1},eye[3]{0,0,sign*100};
            if(!watchOffhandHud(center,axis,grip,hand,config.armourBounds[handed],eye,
                config.armourGapCm,39.3701f,config.watchReading))return 196;
            const float expected=(sign>0?config.armourBounds[handed].maximum[2]+2.f
                :config.armourBounds[handed].minimum[2]-2.f)*.393701f;
            if(!near(center[2],expected)||!near(center[0],0)||!near(center[1],0))return 197;
            for(int i=0;i<9;++i)if(!near(axis[i],i==0?2.f:i==4?3.f:i==8?4.f:0.f))return 198;
        }
    }

    // Deferred submission loses the thread-local extraction scope. Decline
    // jobs only for the exact owned SWF/GUI pair with an active hand surface.
    if(!drawProgMeterInline(true,10,10,20,20,0))return 144;
    if(drawProgMeterInline(false,10,10,20,20,0))return 145;
    if(drawProgMeterInline(true,0,0,20,20,0))return 146;
    if(drawProgMeterInline(true,10,11,20,20,0))return 147;
    if(drawProgMeterInline(true,10,10,20,21,0))return 148;
    if(drawProgMeterInline(true,10,10,0,0,0))return 149;
    if(drawProgMeterInline(true,10,10,20,20,-1))return 150;
    if(drawProgMeterInline(true,10,10,20,20,1))return 151;

    // Owned health/ammo never fall back to the native screen during sequences.
    if(suppressOffhandHudFallback(true,true,false,false,false,true))return 113;
    if(!suppressOffhandHudFallback(true,false,false,false,false,true))return 114;
    if(!suppressOffhandHudFallback(true,true,true,false,false,true))return 115;
    if(!suppressOffhandHudFallback(true,true,false,true,false,true))return 116;
    if(!suppressOffhandHudFallback(true,true,false,false,true,true))return 117;
    if(!suppressOffhandHudFallback(true,true,false,false,false,false))return 118;
    if(suppressOffhandHudFallback(false,false,true,true,true,false))return 119;

    {
        const OffhandHudBounds bounds{{-10,-20,-4},{10,20,8}};
        float center[3]{5,6,0},axis[9]{2,0,0,0,3,0,0,0,4};
        const float grip[3]{},hand[9]{1,0,0,0,1,0,0,0,1},eye[3]{0,0,100};
        if(!watchOffhandHud(center,axis,grip,hand,bounds,eye,3.5f,100)
            ||!near(center[0],5)||!near(center[1],6)||!near(center[2],11.5f)
            ||!near(axis[0],-2)||!near(axis[4],-3)||!near(axis[8],4))return 180;
        float origin[3]{},sx{},sy{};
        if(!centeredOffhandHud(center,axis,25,512.f/300.f,origin,sx,sy))return 181;
        for(int i=0;i<3;++i)if(!near(origin[i]+.5f*(axis[i]*sx+axis[3+i]*sy),center[i]))return 182;
        if(!watchOffhandHud(center,axis,grip,hand,bounds,eye,3.5f,100,false)
            ||!near(center[2],11.5f)||!near(axis[0],-2))return 183;
        center[2]=40;
        if(!watchOffhandHud(center,axis,grip,hand,bounds,eye,3.5f,100,false)||!near(center[2],40))return 184;
        const float below[3]{0,0,-100};center[2]=0;
        if(!watchOffhandHud(center,axis,grip,hand,bounds,below,3.5f,100,false)||!near(center[2],-7.5f))return 185;
        const OffhandHudBounds invalid{};const float before=center[2];
        if(watchOffhandHud(center,axis,grip,hand,invalid,eye,3.5f,100)
            ||!near(center[2],before)||!near(axis[0],-2))return 186;
        std::ostringstream encoded;encoded<<"4 1 1 3.5 ";
        for(int i=0;i<6;++i)encoded<<"1 2 3 4 5 6 .4 ";
        encoded<<"-10 -20 -4 10 20 8 -11 -21 -5 11 21 9";
        OffhandHudConfig restored;std::istringstream input(encoded.str());
        if(!readOffhandHudConfig(input,restored)||!restored.watchReading
            ||!near(restored.armourGapCm,3.5f)||!near(restored.armourBounds[1].maximum[2],9))return 187;
        std::istringstream truncated(encoded.str().substr(0,encoded.str().size()-2));
        if(readOffhandHudConfig(truncated,restored)||!restored.watchReading)return 188;
        std::istringstream bad("4 1 1 -1");if(readOffhandHudConfig(bad,restored))return 189;
    }

    {
        const OffhandHudBounds bounds{{-8,-20,-5},{9,22,11}};
        for(float sign:{-1.f,1.f}){
            const float grip[3]{10,20,30},hand[9]{0,1,0,-1,0,0,0,0,1};
            float center[3]{11,21,31},axis[9]{0,2,2,3,0,0,0,0,4};
            const float eye[3]{11,21+sign*100,31-sign*100};
            if(!watchOffhandHud(center,axis,grip,hand,bounds,eye,3.5f,39.3701f))return 193;
            float n[3]{0,sign/std::sqrt(2.f),-sign/std::sqrt(2.f)};
            for(int corner=0;corner<8;++corner){
                float distance{};
                for(int i=0;i<3;++i){float p=grip[i];
                    for(int j=0;j<3;++j)p+=hand[j*3+i]*((corner&(1<<j))?bounds.maximum[j]:bounds.minimum[j])*.393701f;
                    distance+=(center[i]-p)*n[i];}
                if(distance<3.5f*.393701f-.0001f)return 194;
            }
        }
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
