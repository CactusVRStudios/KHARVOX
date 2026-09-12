// r_skipLightGPUCulling=1 was verified in run24; the user confirmed both
// eyes stable while turning the head and approaching the affected grates.
// This bypasses light visibility rejection, not the rendering of shadows.
static bool(*nativeCvarSet)(void*,const char*,bool){};
static uintptr_t nativeCvarModule{};
static LightCullingScope lightCullingScope;
static bool readLightCulling(LightCullingValue& value){
 constexpr uintptr_t rva=0x66db180;
 const auto object=nativeCvarModule+rva;
 uintptr_t name{},text{};char actual[sizeof("r_skipLightGPUCulling")]{};
 if(!nativeCvarModule||!safe_read_bytes(object+0x50,&name,sizeof(name))||
    !safe_read_bytes(name,actual,sizeof(actual))||std::memcmp(actual,"r_skipLightGPUCulling",sizeof(actual))||
    !safe_read_bytes(object+0x30,&value.integer,sizeof(value.integer))||
    !safe_read_bytes(object+0x10,&text,sizeof(text)))return false;
 char current[64]{};
 if(!safe_read_bytes(text,current,sizeof(current))||!std::memchr(current,0,sizeof(current)))return false;
 value.text=current;
 return true;
}
static void updateLightCulling(bool nativeGameplay){
 const bool wasHeld=lightCullingScope.held;
 if(!nativeCvarSet||!lightCullingScope.update(nativeGameplay,readLightCulling,[](const char* value){
    // force=false: keep DOOM's permission checks, allocation and locking.
    nativeCvarSet(reinterpret_cast<void*>(nativeCvarModule+0x66db180),value,false);
 }))fail("native light GPU culling override or restoration failed");
 if(wasHeld!=lightCullingScope.held){
  LightCullingValue actual;if(!readLightCulling(actual))fail("native light CVar verification unavailable");
  kharvoxnative::log::info(std::format("Native light GPU culling scope active={} name=r_skipLightGPUCulling value={} restoreValue={} engineSetter=true force=false",lightCullingScope.held,actual.integer,lightCullingScope.restore.integer));
 }
}
