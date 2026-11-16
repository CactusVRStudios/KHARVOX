#include "../src/hands/WeaponSupportConfig.h"
#include "../src/hud/ForearmHudPolicy.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace {
using namespace kharvox;
using namespace kharvox::hands;
using namespace kharvox::hands::math;

void assign(WeaponSupportFrame& frame,const Mat4& weapon,const Mat4& joint){
    for(int c=0;c<3;++c)for(int r=0;r<3;++r){
        frame.axis[c*3+r]=weapon.m[c*4+r];frame.jointAxis[0][c*3+r]=joint.m[c*4+r];
    }
    for(int i=0;i<3;++i){frame.origin[i]=weapon.m[12+i];frame.jointOrigin[0][i]=joint.m[12+i];}
    frame.jointMask=1;
}

bool verify(SupportHudGeometry geometry,const ForearmHudConfig& config){
    Mat4 bind{};
    if(!inverseSupportSimilarity(geometry.jointBindInverse,bind))return false;
    for(float units:{25.f,39.3701f,50.f})for(float weaponScale:{.5f,1.f,2.f})for(float angle:{0.f,70.f,180.f}){
        const float degrees[]{angle,31,-17};
        auto weapon=multiply(translation(10,20,30),multiply(rotationDegrees(degrees),scale(weaponScale)));
        auto joint=multiply(translation(.4f,-.3f,.2f),bind);
        WeaponSupportFrame frame;assign(frame,weapon,joint);
        float origin[3]{},axis[9]{};
        if(!supportHudFrame(frame,geometry,units,origin,axis))return false;
        // Independent oracle: the existing rendered-hand joint and fixed-size path.
        Mat4 hand{},fixed{};
        if(!weaponJointModel(weapon,joint,geometry.jointBindInverse,hand)
            ||!fixedSizeSupportMatrix(hand,geometry.anchor,geometry.metersPerAssetUnit*units,fixed))return false;
        auto expected=multiply(fixed,geometry.gripToAsset);
        for(int panel=0;panel<3;++panel){
            float center[3]{},pixels[9]{};
            if(!forearmHudTransform(config,panel,false,origin,axis,units,center,pixels))return false;
            const auto& p=config.panels[panel];
            for(int row=0;row<3;++row){
                float value=expected.m[12+row];
                for(int k=0;k<3;++k)value+=expected.m[k*4+row]*p.matrix[k*4+3]*.01f;
                if(std::abs(center[row]-value)>.0003f)return false;
                for(int col=0;col<3;++col){
                    float direction{};
                    for(int k=0;k<3;++k)direction+=expected.m[k*4+row]/units*p.matrix[k*4+col]*(col? -1.f:1.f);
                    if(std::abs(direction-pixels[col*3+row])>.0003f)return false;
                }
            }
            for(int col=0;col<2;++col){float squared{};
                for(int row=0;row<3;++row)squared+=pixels[col*3+row]*pixels[col*3+row];
                if(std::abs(std::sqrt(squared)-1.f)>.0003f)return false;
            }
        }
        frame.jointMask=0;origin[0]=42;
        if(supportHudFrame(frame,geometry,units,origin,axis)||origin[0]!=42)return false;
        geometry.jointSlot=-1;
        float legacy[3]{},legacyAxis[9]{};
        if(!supportHudFrame(frame,geometry,units,origin,axis)
            ||!supportHudFrame(weapon,geometry,units,legacy,legacyAxis))return false;
        for(int i=0;i<3;++i)if(origin[i]!=legacy[i])return false;
        geometry.jointSlot=0;
    }
    return true;
}
}

int main(int argc,char** argv){
    using namespace kharvox;using namespace kharvox::hands;using namespace kharvox::hands::math;
    ForearmHudConfig config;config.enabled=true;
    for(auto& panel:config.panels){panel.widthCm=25;panel.heightCm=25.f*300/512;
        panel.matrix={1,0,0,5,0,1,0,6,0,0,1,7};}
    config.panels[2].heightCm=0;
    SupportHudGeometry geometry;geometry.valid=true;geometry.jointSlot=0;
    geometry.metersPerAssetUnit=.02f;geometry.anchor={22,-2,3};
    geometry.gripToAsset=multiply(translation(2,3,4),scale(50));
    geometry.jointBindInverse=translation(-3,-4,-5);
    if(!verify(geometry,config))return 1;
    // The regression is observable: old bare-weapon path ignores a moving joint.
    WeaponSupportFrame frame;assign(frame,identity(),translation(4,4,5));
    float corrected[3]{},old[3]{},axis[9]{};
    if(!supportHudFrame(frame,geometry,100,corrected,axis)
        ||!supportHudFrame(identity(),geometry,100,old,axis)
        ||std::abs(corrected[0]-old[0]-1.f)>.0001f)return 2;
    if(argc==1){std::cout<<"Joint, size, parent, fallback and fail-closed regressions passed\n";return 0;}
    if(argc!=3)return 3;
    const std::filesystem::path root=argv[1];
    std::ifstream hud(argv[2]);if(!readForearmHudConfig(hud,config))return 4;
    std::map<std::string,std::string> values;
    for(const auto* name:{"hand_models.cfg","hand_models_calibration_default.cfg","hand_models_calibration_saved.cfg"}){
        std::ifstream file(root/name);std::string line;
        while(std::getline(file,line)){auto eq=line.find('=');if(eq==std::string::npos)continue;
            auto key=line.substr(0,eq);auto first=key.find_first_not_of(" \t");if(first==std::string::npos)continue;
            key=key.substr(first,key.find_last_not_of(" \t")-first+1);values[key]=line.substr(eq+1);
        }
    }
    HandCalibration calibration;
    std::istringstream(values["left_position"])>>calibration.position[0]>>calibration.position[1]>>calibration.position[2];
    std::istringstream(values["left_rotation"])>>calibration.rotationDegrees[0]>>calibration.rotationDegrees[1]>>calibration.rotationDegrees[2];
    calibration.scale=std::stof(values.at("left_scale"));
    HandPose zero;zero.valid=true;Mat4 inverse{};
    if(!inverseSupportSimilarity(calibratedHandMatrix(zero,calibration,false),inverse))return 5;
    auto doomToXr=identity();doomToXr.m[0]=0;doomToXr.m[1]=0;doomToXr.m[2]=-1;
    doomToXr.m[4]=-1;doomToXr.m[5]=0;doomToXr.m[6]=0;doomToXr.m[8]=0;doomToXr.m[9]=1;doomToXr.m[10]=0;
    std::array<WeaponSupportProfile,15> profiles{};
    std::ifstream file(root/"support_hands.cfg");auto count=readWeaponSupportProfiles(file,profiles);
    if(count!=10)return 6;
    for(unsigned kind=1;kind<profiles.size();++kind){const auto& profile=profiles[kind];if(!profile.valid)continue;
        geometry.anchor=profile.point;geometry.jointSlot=profile.jointSlot[0];
        geometry.jointBindInverse=profile.jointBindInverse[0];
        geometry.metersPerAssetUnit=profile.metersPerAssetUnit*calibration.scale/profile.referenceHandScales[0];
        geometry.gripToAsset=multiply(profile.fistToAsset[0],multiply(inverse,doomToXr));
        if(geometry.jointSlot!=0||!verify(geometry,config))return 7;
        std::cout<<"weapon="<<kind<<" synthetic joint animation, 27 scale/rotation/world cases: passed\n";
    }
    return 0;
}
