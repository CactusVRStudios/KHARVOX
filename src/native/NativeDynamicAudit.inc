// Bounded diagnostic: prove or refute the assumed separation of mapped dynamic
// camera ranges. Does not alter descriptors, camera bytes or GPU submission.
namespace {
struct DynamicAuditRange {VkBuffer buffer{};VkDeviceSize offset{},size{};VkDeviceMemory memory{};VkDeviceSize absolute{};std::vector<unsigned char> leftBytes;};
static std::array<std::map<std::pair<VkBuffer,VkDeviceSize>,DynamicAuditRange>,2> dynamicAuditRanges;
static uint32_t dynamicAuditEligible{},dynamicAuditSamples{},dynamicAuditSkipped{};
static bool dynamicAuditActive{};
void beginDynamicAudit(){
 static const bool enabled=kharvox::extendedDiagnosticsEnabled()&&kharvox::runtimeFileExists(L"native_audit_dynamic_inputs");
 dynamicAuditActive=enabled&&phase()==Phase::Ready&&dynamicAuditSamples<8&&++dynamicAuditEligible%120==0;
 if(dynamicAuditActive){for(auto& ranges:dynamicAuditRanges)ranges.clear();dynamicAuditSkipped=0;}
}
void observeDynamicAudit(uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* offsets){
 if(!dynamicAuditActive)return;
 const uint32_t eye=inside_frame_root_double?1:0;uint32_t cursor{};
 std::scoped_lock l(swapchains_mutex);
 for(uint32_t i=0;i<count;++i){auto set=descriptor_sets.find(sets[i]);if(set==descriptor_sets.end()){++dynamicAuditSkipped;continue;}
  std::map<uint32_t,DescriptorBinding> ordered(set->second.begin(),set->second.end());
  for(auto&[binding,d]:ordered){
   if(d.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC&&d.type!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)continue;
   if(cursor>=dynamicCount||!offsets){++dynamicAuditSkipped;return;}auto extra=offsets[cursor++];
   if(d.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)continue;
   auto bi=buffers.find(d.buffer);auto bound=buffer_bindings.find(d.buffer);
   if(bi==buffers.end()||bound==buffer_bindings.end()||d.offset>UINT64_MAX-extra){++dynamicAuditSkipped;continue;}
   auto offset=d.offset+extra;if(offset>=bi->second.size){++dynamicAuditSkipped;continue;}
   auto size=std::min(d.range,bi->second.size-offset);
   if(!size||size>65536||dynamicAuditRanges[eye].size()>=4096||!stageMappedBytes(d.buffer,offset,size_t(size))||bound->second.offset>UINT64_MAX-offset){++dynamicAuditSkipped;continue;}
   auto key=std::make_pair(d.buffer,offset);auto& range=dynamicAuditRanges[eye][key];
   if(!range.size||range.size<size)range={d.buffer,offset,size,bound->second.memory,bound->second.offset+offset,{}};
  }
 }
 if(cursor!=dynamicCount)++dynamicAuditSkipped;
}
void finishDynamicAudit(uint32_t eye){
 if(!dynamicAuditActive)return;
 std::scoped_lock l(swapchains_mutex);
 if(!eye){
  size_t total{};for(auto&[key,range]:dynamicAuditRanges[0]){
   if(total+range.size>16*1024*1024){++dynamicAuditSkipped;continue;}
   auto data=stageMappedBytes(range.buffer,range.offset,size_t(range.size));if(!data){++dynamicAuditSkipped;continue;}
   range.leftBytes.resize(size_t(range.size));copyMappedInput(range.leftBytes.data(),data,range.leftBytes.size());total+=range.leftBytes.size();
  }return;
 }
 uint32_t overlapping{},changed{};size_t bytes{},changedBytes{};
 std::ofstream detail(kharvox::logPathA(std::format("native_dynamic_audit_{:02}.tsv",dynamicAuditSamples).c_str()));
 detail<<"frame\tbuffer\toffset\tbytes\tmatchedRightMemoryRange\tchangedAfterDoubleBytes\n";
 for(auto&[key,range]:dynamicAuditRanges[0]){
  if(range.leftBytes.empty())continue;
  bool overlap=false;for(auto&[otherKey,right]:dynamicAuditRanges[1])if(range.memory==right.memory){
   // Subtraction form avoids overflow at the end of VkDeviceSize ranges.
   overlap|=range.absolute<=right.absolute?right.absolute-range.absolute<range.size:range.absolute-right.absolute<right.size;
  }
  auto data=stageMappedBytes(range.buffer,range.offset,size_t(range.size));if(!data){++dynamicAuditSkipped;continue;}
  std::vector<unsigned char> now(range.leftBytes.size());copyMappedInput(now.data(),data,now.size());size_t differences{};for(size_t i=0;i<now.size();++i)differences+=now[i]!=range.leftBytes[i];
  overlapping+=overlap;changed+=differences!=0;bytes+=now.size();changedBytes+=differences;
  detail<<renderingPose.serial<<'\t'<<reinterpret_cast<uintptr_t>(range.buffer)<<'\t'<<range.offset<<'\t'<<range.size<<'\t'<<overlap<<'\t'<<differences<<'\n';
 }
 if(!detail)fail("native dynamic audit evidence write failed");
 kharvoxnative::log::warn(std::format("Native dynamic input audit frame={} sample={}/8 leftRanges={} rightRanges={} checkedBytes={} overlappingLeftRanges={} changedLeftRanges={} changedBytes={} skipped={} (diagnostic CPU reads; exclude frame from timings; changed descriptor bytes do not prove shader use)",renderingPose.serial,++dynamicAuditSamples,dynamicAuditRanges[0].size(),dynamicAuditRanges[1].size(),bytes,overlapping,changed,changedBytes,dynamicAuditSkipped));
 dynamicAuditActive=false;for(auto& ranges:dynamicAuditRanges)ranges.clear();
}
}
