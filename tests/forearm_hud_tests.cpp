#include "../src/hud/ForearmHudPolicy.h"
#include "../src/hud/OffhandHudPolicy.h"
#include "../src/hands/WeaponSupportPolicy.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <limits>

namespace {
bool near(float a,float b){return std::abs(a-b)<.0002f;}
std::string fixture(){
    std::ostringstream out;out<<"1 1 ";
    for(int i=0;i<3;++i)out<<(i==2?20:25)<<' '<<(i==2?0:25.f*300/512)
        <<" 1 0 0 5 0 1 0 6 0 0 1 7 ";
    return out.str();
}
}
int main(int argc,char** argv){
    using namespace kharvox;
    using namespace kharvox::hands;
    using namespace kharvox::hands::math;
    ForearmHudConfig config;
    std::istringstream input(fixture());
    if(!readForearmHudConfig(input,config)||!forearmHudActive(config,false)
        ||forearmHudActive(config,true)||forearmHudActive(ForearmHudConfig{},false))return 1;
    for(const auto& bad:{std::string("2 1"),std::string("1 1"),fixture()+" extra"}){
        std::istringstream invalid(bad);ForearmHudConfig previous=config;
        if(readForearmHudConfig(invalid,previous)||previous.panels[0].matrix!=config.panels[0].matrix)return 2;
    }
    if(ownedOffhandHudSurface(0x2240888,0xbdcf54,512,300,83)!=0
        ||ownedOffhandHudSurface(0x2240978,0xbdcf54,512,300,100)!=1)return 3;
    if(argc>2)return 4;
    if(argc==2){std::ifstream file(argv[1]);if(!readForearmHudConfig(file,config))return 5;}
    const float identity[9]{1,0,0,0,1,0,0,0,1},zero[3]{};
    for(int panel=0;panel<3;++panel){
        float center[3]{},pixels[9]{};
        if(!forearmHudTransform(config,panel,false,zero,identity,100,center,pixels))return 6;
        // Diagnostic values allow an independent Python/Blender round trip.
        std::cout<<panel;
        std::cout.precision(9);
        for(float value:center)std::cout<<' '<<value;
        for(float value:pixels)std::cout<<' '<<value;
        std::cout<<'\n';
        float native[9]{2,0,0,0,3,0,0,0,4},axis[9]{};
        if(!forearmHudPixelAxis(pixels,native,axis))return 7;
        const auto& pose=config.panels[panel];
        float corner[3]{},extentX{},extentY{};
        const float height=panel==2?pose.widthCm*300/512:pose.heightCm;
        if(!centeredOffhandHud(center,axis,pose.widthCm,pose.widthCm/height,corner,extentX,extentY))return 8;
        // Native (0,0) must be Blender upper-left (-width/2,+height/2).
        for(int world=0;world<3;++world){
            const float expected=center[world]-.5f*pose.widthCm*pose.matrix[world*4]
                +.5f*height*pose.matrix[world*4+1];
            if(!near(corner[world],expected))return 9;
        }
        for(int angle:{0,45,90,180,270})for(float scaleValue:{.5f,1.f,2.f}){
            const float degrees[]{float(angle),float(angle/2),-17};
            auto weapon=multiply(translation(10,20,30),multiply(rotationDegrees(degrees),scale(scaleValue)));
            SupportHudGeometry geometry;geometry.valid=true;geometry.metersPerAssetUnit=.02f;
            geometry.anchor={22,-2,3};geometry.gripToAsset=multiply(translation(2,3,4),scale(50));
            constexpr float units=39.3701f;
            float grip[3]{},hand[9]{},locked[3]{},lockedPixels[9]{};
            if(!supportHudFrame(weapon,geometry,units,grip,hand)
                ||!forearmHudTransform(config,panel,false,grip,hand,units,locked,lockedPixels))return 10;
            Mat4 fixed{};
            if(!fixedSizeSupportMatrix(weapon,geometry.anchor,.02f*units,fixed))return 11;
            const auto finalGrip=multiply(fixed,geometry.gripToAsset);
            for(int world=0;world<3;++world){
                float expected=finalGrip.m[12+world];
                for(int i=0;i<3;++i)expected+=finalGrip.m[i*4+world]*pose.matrix[i*4+3]*.01f;
                if(!near(locked[world],expected))return 12;
                for(int pixel=0;pixel<3;++pixel){float direction{};
                    for(int i=0;i<3;++i)direction+=finalGrip.m[i*4+world]/units*pose.matrix[i*4+pixel]*(pixel==0?1.f:-1.f);
                    if(!near(lockedPixels[pixel*3+world],direction))return 13;
                }
            }
            float freeAgain[3]{},freePixels[9]{};
            if(!forearmHudTransform(config,panel,false,zero,identity,100,freeAgain,freePixels))return 14;
            for(int i=0;i<3;++i)if(!near(freeAgain[i],center[i]))return 15;
        }
        float unchanged[3]{42,43,44},unchangedAxis[9]{};
        if(forearmHudTransform(config,panel,true,zero,identity,100,unchanged,unchangedAxis)||unchanged[0]!=42)return 16;
        if(forearmHudTransform(config,panel,false,zero,identity,0,unchanged,unchangedAxis)||unchanged[0]!=42)return 17;
        const float invalidAxis[9]{};
        if(forearmHudPixelAxis(pixels,invalidAxis,unchangedAxis))return 18;
    }
    return 0;
}
