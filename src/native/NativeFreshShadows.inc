// Preserve shared SMAA and the debug AA override and shared visual settings across presets.
// The validated bool setter owns allocation, locking, parsing and permissions.
static bool debugDisableAa(){
 char value[8]{};
 return GetEnvironmentVariableA("KHARVOX_DISABLE_AA",value,sizeof(value))==1&&value[0]=='1';
}
static bool(*realFreshShadowSet)(void*,const char*,bool){};
static uintptr_t freshShadowModule{};
static std::string shadowAuditValue(uintptr_t rva,const char* expected);
static bool hookFreshShadowSet(void* object,const char* value,bool force){
 const auto rva=reinterpret_cast<uintptr_t>(object)-freshShadowModule;
 static const auto controls=rendererStartupControls(requested(),kharvox::rendererDefaults::freshShadows,
  kharvox::rendererDefaults::uncachedShadows,kharvox::runtimeFileExists(L"native_test_cached_shadows"),kharvox::runtimeFileExists(L"native_test_disable_async_compute"),debugDisableAa(),kharvox::sfs::vrEnabled());
 const auto* selected=nativePresetControl(controls,rva);
 if(selected){
  const auto& control=*selected;
  uintptr_t name{};char actual[64]{};const auto length=std::strlen(control.name)+1;
  if(length>sizeof(actual)||!safe_read_bytes(reinterpret_cast<uintptr_t>(object)+0x50,&name,sizeof(name))||
   !safe_read_bytes(name,actual,length)||std::memcmp(actual,control.name,length))fail("native render CVar identity changed");
  const auto* requestedValue=value;
  if(requested()&&(!std::strcmp(control.name,"r_shadowMaxStaleFrames")||
     !std::strcmp(control.name,"r_motionBlurQuality")||!std::strcmp(control.name,"r_antialiasing")))
   beginQualityChange();
  static std::atomic_uint updates{};const auto count=++updates;
  if(count<=12||count%120==0||!std::strcmp(control.name,"r_enableAsyncCompute"))kharvoxnative::log::info(std::format("Native render preset hold name={} requested={} value={} originalForcePreserved={} update={} engineSetterOwnsStorage=true",control.name,requestedValue?requestedValue:"<null>",control.value,force,count));
 }
 // The engine setter may free an incoming string that aliases CVar storage.
 const std::string requestedText=selected&&value?value:"<null>";
 const bool accepted=setProtectedRenderControl(selected,value,force,[&](const char* effective,bool originalForce){
  return realFreshShadowSet(object,effective,originalForce);
 });
 if(selected){
  const auto actual=shadowAuditValue(rva,selected->name);
  kharvoxnative::log::info(std::format("[RENDER-CVAR] renderer={} name={} requested={} enforced={} accepted={} actual={} force={} verified={}",
   requested()?"Native":"AER",selected->name,requestedText,selected->value,accepted,actual,force,accepted&&actual==selected->value));
 }
 return accepted;
}
