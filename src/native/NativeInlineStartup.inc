static InlineStartup inlineStartup;
static bool readUseSmp(int& value){
 const auto object=nativeCvarModule+kUseSmpCvarRva;
 uintptr_t name{};char actual[sizeof("r_useSMP")]{};
 return nativeCvarModule&&safe_read_bytes(object+0x50,&name,sizeof(name))&&
  safe_read_bytes(name,actual,sizeof(actual))&&!std::memcmp(actual,"r_useSMP",sizeof(actual))&&
  safe_read_bytes(object+0x30,&value,sizeof(value));
}
static void initializeNativeInlineRenderer(){
 // Called while vkCreateDevice is returning, before any game queue is handed
 // out or Native recording starts. A missing hidden console command must not
 // leave the required startup setting dependent on an external console mod.
 // Use the validated engine setter and its normal permissions exactly once;
 // never patch numeric/string fields or the derived job flag during gameplay.
 const bool beforeRendering=phase()==Phase::Off&&!kharvoxnative::vulkan_state().queue&&
  !renderThread.load()&&!outstanding&&!armed;
 const auto result=inlineStartup.initialize(beforeRendering,readUseSmp,[](const char* value){
  return nativeCvarSet&&nativeCvarSet(reinterpret_cast<void*>(nativeCvarModule+kUseSmpCvarRva),value,false);
 });
 kharvoxnative::log::info(std::format("Native inline startup result={} before={} after={} beforeGameQueue={} engineSetter=true force=false pid={}",
  int(result),inlineStartup.before,inlineStartup.after,beforeRendering,GetCurrentProcessId()));
 if(result!=InlineStartupResult::AlreadyInline&&result!=InlineStartupResult::Applied)
  fail("native inline startup initialization failed; engine setting or early boundary unverified");
}
