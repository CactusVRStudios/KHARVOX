static void initializeNativeRenderControls(){
 const bool early=phase()==Phase::Off&&!kharvoxnative::vulkan_state().queue&&
  !renderThread.load()&&!outstanding&&!armed;
 const auto controls=nativeStartupControls(kharvox::rendererDefaults::freshShadows,
  kharvox::rendererDefaults::uncachedShadows,kharvox::runtimeFileExists(L"native_test_cached_shadows"),kharvox::runtimeFileExists(L"native_test_disable_async_compute"),debugDisableAa());
 kharvoxnative::log::info(std::format("Native async-compute startup override optIn={} actual={}",kharvox::runtimeFileExists(L"native_test_disable_async_compute"),shadowAuditValue(0x6fd58a0,"r_enableAsyncCompute")));
 const bool ok=initializeStartupControls(early,controls,[](const StartupControl& c,std::string& value){
  value=shadowAuditValue(c.rva,c.name);kharvoxnative::log::info(std::format("Native render startup read name={} actual={} requested={}",c.name,value,c.value));return value!="unavailable";
 },[](const StartupControl& c){
  const bool accepted=nativeCvarSet&&nativeCvarSet(reinterpret_cast<void*>(nativeCvarModule+c.rva),c.value,false);
  kharvoxnative::log::info(std::format("Native render startup setter name={} requested={} accepted={} force=false",c.name,c.value,accepted));return accepted;
 },[](const StartupControl& c,const std::string& before,const std::string& after){
  kharvoxnative::log::info(std::format("Native render startup name={} before={} after={} beforeGameQueue=true engineSetter=true force=false",c.name,before,after));
 });
 if(!ok)fail("native render startup controls could not be verified");
}
