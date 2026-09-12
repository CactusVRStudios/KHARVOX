#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace kharvox::native {
struct StartupControl { uintptr_t rva; const char* name; const char* value; };
// Verified DOOM CVar registrations: constructor RVA 0x293870.
inline std::vector<StartupControl> visualStartupControls(bool disableAa=false){
 return {{0x66dd200,"r_antialiasing",disableAa?"0":"2"},
  {0x67288e0,"r_filmGrainRatio","0"},
  {0x6728ca0,"r_sharpening","2"},
  {0x6727970,"r_motionblur","0"}, // Pass gate at RVA 0x18735D2; quality 0 still selects a kernel.
  {0x6728df0,"r_motionBlurQuality","0"},
  {0x672b5c0,"r_SSR","0"}, // Verified pass enable switch; quality 0 alone keeps SSR active.
  {0x672bae0,"r_SSRQuality","0"}}; // Lowest SSR quality, not proven disabled.
}
inline std::vector<StartupControl> nativeStartupControls(bool fresh,bool uncached,bool cached,bool disableAsync=false,bool disableAa=false){
 auto controls=visualStartupControls(disableAa);
 // The engine refused this experimental override in r166 before rendering.
 // Keep it explicit; never force writes past engine permissions.
 if(disableAsync)controls.push_back({0x6fd58a0,"r_enableAsyncCompute","0"});
 if(uncached||cached){controls.push_back({0x6c0b620,"r_shadowStaticMode","1"});
  controls.push_back({0x6eacb90,"r_shadowStaticMipTriThresholds",cached?"-1,16,8,4,2":"-1,-1,-1,-1,-1"});}
 if(fresh){controls.push_back({0x6c0b830,"r_shadowMaxStaleFrames","0,0,0,0,0"});
  controls.push_back({0x6eac480,"r_shadowParallelMaxStaleFrames","0,0,0,0"});}
 return controls;
}
// Shared visual settings, never Native shadow/queue controls in AER.
inline std::vector<StartupControl> rendererStartupControls(bool native,bool fresh,bool uncached,bool cached,bool disableAsync=false,bool disableAa=false){
 if(native)return nativeStartupControls(fresh,uncached,cached,disableAsync,disableAa);
 return visualStartupControls(disableAa);
}
template<class Set>
bool setProtectedRenderControl(const StartupControl* control,const char* requested,bool force,const Set& setter){
 return setter(control?control->value:requested,force);
}
inline const StartupControl* nativePresetControl(const std::vector<StartupControl>& controls,uintptr_t rva){
 for(const auto& control:controls)if(control.rva==rva)return &control;
 return nullptr;
}
template<class Read,class Write,class Report>
bool initializeStartupControls(bool beforeRendering,const std::vector<StartupControl>& controls,
 const Read& read,const Write& write,const Report& report){
 if(!beforeRendering)return false;
 std::vector<std::string> before(controls.size());
 // Validate every object before changing any setting.
 for(size_t i=0;i<controls.size();++i)if(!read(controls[i],before[i]))return false;
 for(size_t i=0;i<controls.size();++i){
  const auto& c=controls[i];std::string after;
  if(before[i]!=c.value&&!write(c))return false;
  if(!read(c,after)||after!=c.value)return false;
  report(c,before[i],after);
 }
 return true;
}
}
