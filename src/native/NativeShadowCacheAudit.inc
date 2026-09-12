// Read-only confirmation of the separate cache/freshness/visibility controls.
// The launcher requests values before loading; never infer their actual values
// from the arguments, because presets can overwrite CVars later.
static std::string shadowAuditValue(uintptr_t rva,const char* expected){
 const auto object=nativeCvarModule+rva;uintptr_t name{},text{};
 char actual[64]{},value[64]{};const auto length=std::strlen(expected)+1;
 if(!nativeCvarModule||length>sizeof(actual)||
    !safe_read_bytes(object+0x50,&name,sizeof(name))||
    !safe_read_bytes(name,actual,length)||std::memcmp(actual,expected,length)||
    !safe_read_bytes(object+0x10,&text,sizeof(text)))return "unavailable";
 // Bounded byte reads do not require unrelated bytes past the terminator.
 for(size_t i=0;i<sizeof(value);++i){
  if(!safe_read_bytes(text+i,value+i,1))return "unavailable";
  if(!value[i])return value;
 }
 return "unavailable";
}
static void auditShadowConfiguration(bool gameplay){
 static int previous=-1;const bool transition=previous!=int(gameplay);previous=int(gameplay);
 static uint32_t frames{};if(!transition&&++frames>4&&frames%120)return;
 kharvoxnative::log::info(std::format("Native render controls frame={} gameplay={} transition={} AA={} motionBlur={} asyncCompute={} readOnly=true",renderingPose.serial,gameplay,transition,shadowAuditValue(0x66dd200,"r_antialiasing"),shadowAuditValue(0x6728df0,"r_motionBlurQuality"),shadowAuditValue(0x6fd58a0,"r_enableAsyncCompute")));
 kharvoxnative::log::info(std::format(
  "Native shadow controls frame={} staticMode={} staticMipThresholds={} stale={} parallelStale={} skipLightGpuCulling={} readOnly=true (actual CVar text; cache thresholds are separate from stale ages)",
  renderingPose.serial,shadowAuditValue(0x6c0b620,"r_shadowStaticMode"),
  shadowAuditValue(0x6eacb90,"r_shadowStaticMipTriThresholds"),
  shadowAuditValue(0x6c0b830,"r_shadowMaxStaleFrames"),
  shadowAuditValue(0x6eac480,"r_shadowParallelMaxStaleFrames"),
  shadowAuditValue(0x66db180,"r_skipLightGPUCulling")));
}
