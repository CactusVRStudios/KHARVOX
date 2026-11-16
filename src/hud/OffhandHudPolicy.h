#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <istream>
namespace kharvox {
// A render publication may outlive Present by one boundary, but must never
// be replaced with a live physics pose or carried into another level.
struct OffhandHudRenderFrame {
    std::array<float,3> origin{};
    std::array<float,9> axis{};
    uint64_t present{},level{};
    bool valid{};
    bool supportHeld{};unsigned supportWeapon{};
    std::array<float,3> bodyOrigin{};
    std::array<float,9> bodyAxis{};
    bool usable(uint64_t now,uint64_t currentLevel) const {
        return valid&&level==currentLevel&&now>=present&&now-present<=1;
    }
};
struct OffhandHudCalibration {
    std::array<float,3> centimeters{8,0,8};
    std::array<float,3> degrees{}; // pitch, yaw, roll
    float scale{0.40f};
};
inline const char* offhandHudName(int surface){return surface==0?"Life":surface==1?"Ammo":"ProgMeter";}
struct OffhandHudBounds {
    std::array<float,3> minimum{},maximum{};
    // Conservative mesh sections, prepared at load/calibration time. Each row
    // is min XYZ / max XYZ in the same Grip-local centimetres as the aggregate.
    std::array<std::array<float,6>,8> sections{};
    unsigned sectionMask{};
};
struct OffhandHudConfig {
    bool enabled{true};std::array<OffhandHudCalibration,6> modes{};
    bool watchReading{};float armourGapCm{};
    // Production mesh envelope in raw Grip-local DOOM coordinates, centimetres.
    std::array<OffhandHudBounds,2> armourBounds{};
};
// This one SWF must finish inside its owner Frame: ProgMeter extraction uses
// that frame's pose and geometry scope. All unrelated SWFs retain native jobs.
inline bool drawProgMeterInline(bool enabled,uintptr_t ownerSwf,uintptr_t drawSwf,
    uintptr_t ownerGui,uintptr_t drawGui,int surface){
    return enabled&&ownerSwf&&ownerSwf==drawSwf&&ownerGui&&ownerGui==drawGui&&surface==0;
}
inline bool suppressOffhandHudFallback(bool managed,bool gameplay,bool cinematic,bool ledge,bool syncAttack,bool tracked){
    return managed&&(!gameplay||cinematic||ledge||syncAttack||!tracked);
}
inline int offhandHudSurface(uintptr_t caller,int width,int height,int scale){
    if(caller!=0xbdcf54||width!=512||height!=300)return -1;
    return scale==83?0:scale==100?1:-1;
}
inline int ownedOffhandHudSurface(uintptr_t ownerVtable,uintptr_t caller,int width,int height,int scale){
    const int surface=offhandHudSurface(caller,width,height,scale);
    // Live SP objects: WeaponInfo uses .083; BottomLeft uses .100.
    // Preserve the existing calibration slots, rather than inferring their
    // identities from manager names. Bottom also uses .100 but is not owned.
    return (surface==0&&ownerVtable==0x2240888)
        ||(surface==1&&ownerVtable==0x2240978)?surface:-1;
}
// Measure the nearest corner without changing the hand-bound transform.
inline bool offhandHudNearestDepth(const float* center,const float* axis,float width,float aspect,
    const float* eye,const float* forward,float& nearest){
    if(!std::isfinite(width)||width<=0||!std::isfinite(aspect)||aspect<=0)return false;
    float depth=0,radius=0,norm=0;
    for(int i=0;i<3;++i){if(!std::isfinite(center[i])||!std::isfinite(eye[i])||!std::isfinite(forward[i]))return false;
        depth+=(center[i]-eye[i])*forward[i];norm+=forward[i]*forward[i];}
    if(std::abs(norm-1.f)>.01f)return false;
    for(int row=0;row<2;++row){float length=0,dot=0;
        for(int i=0;i<3;++i){const float v=axis[row*3+i];if(!std::isfinite(v))return false;length+=v*v;dot+=v*forward[i];}
        if(length<1e-10f)return false;
        radius+=.5f*(row?width/aspect:width)*std::abs(dot)/std::sqrt(length);
    }
    nearest=depth-radius;return std::isfinite(nearest);
}
inline bool offhandHudNearVisible(bool wasVisible,float nearest,float minimumDepth,float margin){
    return std::isfinite(nearest)&&nearest>=minimumDepth+(wasVisible?0.f:margin);
}
inline bool readOffhandHudConfig(std::istream& stream,OffhandHudConfig& out){
    int version{},enabled{};OffhandHudConfig value;
    if(!(stream>>version>>enabled)||(version<1||version>4)||(enabled!=0&&enabled!=1))return false;
    if(version==4){
        int watch{};
        if(!(stream>>watch>>value.armourGapCm)||(watch!=0&&watch!=1)
            ||!std::isfinite(value.armourGapCm)||value.armourGapCm<0||value.armourGapCm>20)return false;
        value.watchReading=watch!=0;
    }
    for(int i=0;i<(version==1?2:version==2?4:6);++i){auto& mode=value.modes[i];
        for(auto& v:mode.centimeters)if(!(stream>>v)||!std::isfinite(v)||std::abs(v)>100)return false;
        for(auto& v:mode.degrees)if(!(stream>>v)||!std::isfinite(v)||std::abs(v)>180)return false;
        if(!(stream>>mode.scale)||!std::isfinite(mode.scale)||mode.scale<.02f||mode.scale>2)return false;
    }
    if(version==1){value.modes[2]=value.modes[0];value.modes[3]=value.modes[1];}
    if(version<3){
        for(int hand=0;hand<2;++hand){value.modes[4+hand]=value.modes[hand];
            value.modes[4+hand].centimeters[1]=std::clamp(value.modes[4+hand].centimeters[1]+14.f,-100.f,100.f);
            value.modes[4+hand].centimeters[2]=std::clamp(value.modes[4+hand].centimeters[2]+8.f,-100.f,100.f);}
    }
    if(version==4)for(auto& bounds:value.armourBounds){
        for(auto& v:bounds.minimum)if(!(stream>>v)||!std::isfinite(v)||std::abs(v)>200)return false;
        for(auto& v:bounds.maximum)if(!(stream>>v)||!std::isfinite(v)||std::abs(v)>200)return false;
        for(int i=0;i<3;++i)if(bounds.minimum[i]>=bounds.maximum[i])return false;
    }
    value.enabled=enabled!=0;out=value;return true;
}
// Rotate the actual native pixel axes, never a guessed controller Euler axis.
// Clearance is perpendicular to that pixel plane, toward the shared centre
// eye. Both eyes therefore use the same side of the armor envelope.
// Invalid input leaves the complete transform unchanged.
inline bool watchOffhandHud(float* center,float* axis,const float* grip,const float* hand,
    const OffhandHudBounds& bounds,const float* centerEye,float gapCm,float unitsPerMeter,
    bool rotateReading=true,float panelWidth=0,float panelAspect=512.f/300.f){
    if(!std::isfinite(gapCm)||gapCm<0||!std::isfinite(unitsPerMeter)||unitsPerMeter<=0)return false;
    for(int i=0;i<9;++i)if(!std::isfinite(axis[i])||!std::isfinite(hand[i]))return false;
    for(int i=0;i<3;++i)if(!std::isfinite(center[i])||!std::isfinite(grip[i])
        ||!std::isfinite(centerEye[i])||!std::isfinite(bounds.minimum[i])
        ||!std::isfinite(bounds.maximum[i])||bounds.minimum[i]>=bounds.maximum[i])return false;
    float normal[3]{axis[1]*axis[5]-axis[2]*axis[4],
        axis[2]*axis[3]-axis[0]*axis[5],axis[0]*axis[4]-axis[1]*axis[3]};
    float length{};for(float v:normal)length+=v*v;
    if(length<1e-12f||!std::isfinite(length))return false;
    length=std::sqrt(length);float facing{};
    for(int i=0;i<3;++i){normal[i]/=length;facing+=normal[i]*(centerEye[i]-center[i]);}
    if(facing<0)for(float& v:normal)v=-v;
    const bool local=bounds.sectionMask&&panelWidth>0;
    float pixelLengths[2]{};
    if(local){
        if(!std::isfinite(panelWidth)||!std::isfinite(panelAspect)||panelAspect<=0)return false;
        for(int row=0;row<2;++row){
            for(int i=0;i<3;++i)pixelLengths[row]+=axis[row*3+i]*axis[row*3+i];
            pixelLengths[row]=std::sqrt(pixelLengths[row]);
            if(pixelLengths[row]<1e-6f)return false;
        }
    }
    float maximum=-1e30f;
    for(int section=0;section<(local?8:1);++section){
      if(local&&!(bounds.sectionMask&(1u<<section)))continue;
      float depth=-1e30f,lo[2]{1e30f,1e30f},hi[2]{-1e30f,-1e30f};
      for(int corner=0;corner<8;++corner){
        float relative[3]{},projected{};
        for(int i=0;i<3;++i){float world=grip[i]-center[i];
            for(int j=0;j<3;++j){
                const float value=local?bounds.sections[section][j+((corner&(1<<j))?3:0)]
                    :((corner&(1<<j))?bounds.maximum[j]:bounds.minimum[j]);
                if(!std::isfinite(value))return false;
                world+=hand[j*3+i]*value*unitsPerMeter*.01f;
            }
            relative[i]=world;
            projected+=world*normal[i];}
        depth=std::max(depth,projected);
        if(local)for(int row=0;row<2;++row){float tangent{};
            for(int i=0;i<3;++i)tangent+=relative[i]*axis[row*3+i]/pixelLengths[row];
            lo[row]=std::min(lo[row],tangent);hi[row]=std::max(hi[row],tangent);
        }
      }
      if(local){
          const float half[2]{panelWidth*.5f,panelWidth/panelAspect*.5f};
          if(lo[0]>half[0]||hi[0]<-half[0]||lo[1]>half[1]||hi[1]<-half[1])continue;
      }
      maximum=std::max(maximum,depth);
    }
    const float lift=std::max(0.f,maximum+gapCm*unitsPerMeter*.01f);
    float adjusted[3]{};
    for(int i=0;i<3;++i){adjusted[i]=center[i]+normal[i]*lift;if(!std::isfinite(adjusted[i]))return false;}
    for(int i=0;i<3;++i)center[i]=adjusted[i];
    if(rotateReading)for(int i=0;i<6;++i)axis[i]=-axis[i];
    return true;
}
// Use the original head-facing native GUI as the reading-side reference,
// rather than assuming which direction its local pixel normal points.
inline float offhandHudPixelFacing(const float* axis,const float* forward){
    const float normal[3]{axis[1]*axis[5]-axis[2]*axis[4],
        axis[2]*axis[3]-axis[0]*axis[5],axis[0]*axis[4]-axis[1]*axis[3]};
    float dot{},length{};for(int i=0;i<3;++i){dot+=normal[i]*forward[i];length+=normal[i]*normal[i];}
    return length>1e-12f?dot/std::sqrt(length):0.f;
}
// Preserve the original pixel axes and the wrist-local reading side. The
// native headlocked GUI supplies the sign before it is attached to the hand;
// moving the viewer around the wrist must not move the HUD through the glove
// or mirror its artwork. Only the necessary normal translation is applied.
inline bool clearOriginalOffhandHud(float* center,float* axis,const float* grip,
    const float* hand,const OffhandHudBounds& bounds,float nativeFacing,
    float gapCm,float unitsPerMeter,float panelWidth=0,float panelAspect=512.f/300.f){
    if(!std::isfinite(nativeFacing)||std::abs(nativeFacing)<.01f)return false;
    float normal[3]{axis[1]*axis[5]-axis[2]*axis[4],
        axis[2]*axis[3]-axis[0]*axis[5],axis[0]*axis[4]-axis[1]*axis[3]};
    float squared{};for(float value:normal)squared+=value*value;
    if(!std::isfinite(squared)||squared<1e-12f)return false;
    const float direction=(nativeFacing>0?-1.f:1.f)/std::sqrt(squared);
    float readingSide[3]{};
    for(int i=0;i<3;++i)readingSide[i]=center[i]+normal[i]*direction*unitsPerMeter;
    return watchOffhandHud(center,axis,grip,hand,bounds,readingSide,gapCm,
        unitsPerMeter,false,panelWidth,panelAspect);
}
inline bool faceOriginalOffhandHud(float* axis,const float* center,const float* eye,float reference){
    float towardEye[3]{};
    for(int i=0;i<9;++i)if(!std::isfinite(axis[i]))return false;
    for(int i=0;i<3;++i){towardEye[i]=eye[i]-center[i];if(!std::isfinite(towardEye[i]))return false;}
    if(!std::isfinite(reference)||std::abs(reference)<.01f
        ||offhandHudPixelFacing(axis,towardEye)*reference<=0)return false;
    // Reverse the horizontal pixel row and the unused normal row together:
    // center, pixel-up, row lengths and determinant are preserved.
    for(int i=0;i<3;++i){axis[i]=-axis[i];axis[6+i]=-axis[6+i];}
    return true;
}
inline void offhandHudBasis(const float* hand,const OffhandHudCalibration& c,float* out){
    std::array<float,9> local{1,0,0,0,1,0,0,0,1};
    // Intrinsic pitch (forward/up), yaw (forward/left), roll (left/up).
    const int pairs[3][2]{{0,2},{0,1},{1,2}};
    for(int r=0;r<3;++r){const float a=c.degrees[r]*.01745329251994329577f;
        const float cs=std::cos(a),sn=std::sin(a);const int x=pairs[r][0],y=pairs[r][1];
        for(int j=0;j<3;++j){const float u=local[x*3+j],v=local[y*3+j];local[x*3+j]=cs*u+sn*v;local[y*3+j]=-sn*u+cs*v;}}
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){out[i*3+j]=0;for(int k=0;k<3;++k)out[i*3+j]+=local[i*3+k]*hand[k*3+j];}
}
inline void offhandHudOrigin(const float* grip,const float* hand,const float* panel,
    const OffhandHudCalibration& c,int surface,float unitsPerMeter,float* out){
    for(int i=0;i<3;++i){out[i]=grip[i];
        for(int j=0;j<3;++j)out[i]+=hand[j*3+i]*c.centimeters[j]*unitsPerMeter*.01f;
        out[i]+=hand[3+i]*(surface==0?1.f:surface==1?-1.f:0.f)*14.f*unitsPerMeter*.01f;}
}
// Final GUI uses local X/Y pixels multiplied by its physical extents. Center
// the complete canvas after those extents are known, independent of rotation.
inline bool centeredOffhandHud(const float* center,const float* axis,float width,
    float aspect,float* origin,float& extentX,float& extentY){
    float lengths[2]{};
    for(int row=0;row<2;++row){for(int j=0;j<3;++j){const float v=axis[row*3+j];if(!std::isfinite(v))return false;lengths[row]+=v*v;}lengths[row]=std::sqrt(lengths[row]);}
    if(!std::isfinite(width)||width<=0||!std::isfinite(aspect)||aspect<=0||lengths[0]<.00001f||lengths[1]<.00001f)return false;
    extentX=width/lengths[0];extentY=width/aspect/lengths[1];
    for(int i=0;i<3;++i){if(!std::isfinite(center[i]))return false;origin[i]=center[i]-.5f*(axis[i]*extentX+axis[3+i]*extentY);}
    return true;
}

}
