#include "../src/native/NativeStartupControls.h"
#include <map>
#include <iostream>
using namespace kharvox::native;
int main(){
 int failures=0,writes=0,reports=0;auto check=[&](bool b){if(!b)++failures;};
 for(bool sfs:{false,true})for(bool native:{false,true})for(bool disableAa:{false,true}){
  const auto profile=rendererStartupControls(native,false,false,false,false,disableAa,sfs);
  const auto* temporal=nativePresetControl(profile,0x672bc30);
  check(bool(temporal)==(sfs&&!native));
  if(temporal){
   check(std::string(temporal->name)=="r_SSDOTemporalAA"&&std::string(temporal->value)=="0");
   for(bool force:{false,true})check(setProtectedRenderControl(temporal,"1",force,[&](const char* value,bool forwarded){
    return std::string(value)=="0"&&force==forwarded;
   }));
  }
 }
 const uintptr_t debugRvas[]={0x6727ac0,0x66dd2a0,0x66e0760,0x5b5ec50};
 for(bool native:{false,true}) {
  const auto profile=rendererStartupControls(native,false,false,false);
  for(auto rva:debugRvas) check(!nativePresetControl(profile,rva));
 }
 const auto controls=nativeStartupControls(true,true,false,true);
 check(std::string(nativePresetControl(controls,0x66dd200)->value)=="2");
 check(controls.size()==12);check(nativeStartupControls(false,false,false).size()==7);
 const auto aer=rendererStartupControls(false,true,true,true,true);
 check(aer.size()==7&&std::string(aer[0].name)=="r_antialiasing");
 check(!nativePresetControl(aer,0x5d48350)); // AER must not force/protect r_useSMP.
 check(nativePresetControl(aer,0x6728df0)&&!nativePresetControl(aer,0x6fd58a0)&&!nativePresetControl(aer,0x6c0b620));
 for(bool isNative:{false,true})for(bool force:{false,true})for(bool disableAa:{false,true}){
  const auto profile=rendererStartupControls(isNative,false,false,false,false,disableAa);
  const auto* aa=nativePresetControl(profile,0x66dd200);
  const std::string expected=disableAa?"0":"2";
  std::string actual="6";
  // Presets must not select temporal AA or defeat either renderer's debug override.
  check(setProtectedRenderControl(aa,isNative?"6":"0",force,[&](const char* value,bool forwarded){
   check(forwarded==force);actual=value;return true;
  })&&actual==expected);
  check(!setProtectedRenderControl(aa,isNative?"6":"0",force,[&](const char* value,bool forwarded){
   check(forwarded==force&&std::string(value)==expected);return false;
  })); // Never hide the engine's permission refusal.
  for(auto rva:{uintptr_t(0x67288e0),uintptr_t(0x6728ca0),uintptr_t(0x6727970),uintptr_t(0x6728df0),uintptr_t(0x672b5c0),uintptr_t(0x672bae0)}){
   const auto* visual=nativePresetControl(profile,rva);check(visual!=nullptr);
   check(setProtectedRenderControl(visual,"99",force,[&](const char* value,bool forwarded){
    check(forwarded==force&&std::string(value)==(rva==0x6728ca0?"2":"0"));return true;
   }));
  }
  check(setProtectedRenderControl(nullptr,"2",force,[&](const char* value,bool forwarded){
   check(forwarded==force&&std::string(value)=="2");return true;
  }));
 }
 const auto* asyncControl=nativePresetControl(controls,0x6fd58a0);
 check(asyncControl&&std::string(asyncControl->name)=="r_enableAsyncCompute"&&std::string(asyncControl->value)=="0");
 const auto baseline=nativeStartupControls(true,true,false);
 check(baseline.size()==11&&!nativePresetControl(baseline,0x6fd58a0));
 check(!nativePresetControl(controls,0x1234)); // Unrelated effect settings pass through.
 const auto cached=nativeStartupControls(false,false,true);check(std::string(cached.back().value)=="-1,16,8,4,2");
 std::map<uintptr_t,std::string> values;for(auto& c:controls)values[c.rva]="old";
 bool readable=true,allowed=true,apply=true;
 auto read=[&](const StartupControl& c,std::string& v){v=values[c.rva];return readable;};
 auto write=[&](const StartupControl& c){++writes;if(allowed&&apply)values[c.rva]=c.value;return allowed;};
 auto report=[&](const auto&,const auto&,const auto&){++reports;};
 check(!initializeStartupControls(false,controls,read,write,report)&&writes==0);
 readable=false;check(!initializeStartupControls(true,controls,read,write,report)&&writes==0);
 values[asyncControl->rva]="1";
 readable=true;check(initializeStartupControls(true,controls,read,write,report));check(writes==12&&reports==12);
 check(values[asyncControl->rva]=="0");
 check(initializeStartupControls(true,controls,read,write,report)&&writes==12);
 values[controls[0].rva]="6";allowed=false;
 check(!initializeStartupControls(true,controls,read,write,report));
 allowed=true;apply=false;check(!initializeStartupControls(true,controls,read,write,report));
 apply=true;check(initializeStartupControls(true,controls,read,write,report));
 const auto before=writes;
 auto wrongIdentity=[&](const StartupControl& c,std::string& v){return c.rva!=controls.back().rva&&read(c,v);};
 check(!initializeStartupControls(true,controls,wrongIdentity,write,report)&&writes==before);
 std::cout<<"Native startup controls failures="<<failures<<'\n';return failures?1:0;
}
