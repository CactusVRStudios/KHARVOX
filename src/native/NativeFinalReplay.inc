// KHARVOX completion pass adapter. The historical 0x186E6BA address is
// chained unwind code, NOT a callable function. Replay Vulkan recording at
// the actual swapchain render-pass boundary instead of re-entering that code.
#include <functional>
#include <tuple>
#include "NativeReplayRecording.h"
#include "NativePushReplay.h"
#include "NativePassInputIndex.h"
#include "NativeSnapshotAccessClasses.h"
#include "NativeImagePlan.h"
#include "NativeSnapshotBatch.h"
#include "NativeEyeBindings.h"
#include "NativeDepthSeedPolicy.h"
namespace kharvox::native {
using ReplayCommand=std::function<void()>;
using ImageInputs=std::map<VkImageView,VkImageLayout>;
struct PassInputs {VkFramebuffer framebuffer{};VkRenderPass renderPass{};ImageInputs images;ImageInputVisits<VkDescriptorSet> visited;VkRect2D renderArea{};};
struct DescriptorReplayPayload {
 VkPipelineBindPoint point{};VkPipelineLayout layout{};uint32_t first{};
 std::vector<VkDescriptorSet> sets;
 std::vector<uint32_t> offsets;
};
static std::vector<PassInputs> realPassInputs;
static size_t doubledPassCursor{};
static PassInputIndex<ImageInputs> realPassInputIndex;
static bool passInputIndexDirty{true};
static std::set<VkImage> pendingSceneDepthClears;
// Caller holds framebuffer_depth_view_mutex. No metadata pointers escape.
static void passTargetIdentity(VkFramebuffer fb,PassTargetIdentity& target){
 target.attachments.clear();target.width=target.height=target.layers=0;target.unknownFramebuffer=0;
 const auto it=framebuffer_infos.find(fb);
 if(it==framebuffer_infos.end()){target.unknownFramebuffer=reinterpret_cast<uintptr_t>(fb);return;}
 const auto& info=it->second;target.width=info.width;target.height=info.height;target.layers=info.layers;
 for(auto view:info.attachments)target.attachments.push_back(reinterpret_cast<uintptr_t>(view));
}
static const PassInputIndex<ImageInputs>::Group* indexedPassInputs(VkFramebuffer fb){
 // One lock per target instead of one lock per candidate pair. Keep identity
 // resolution and construction under the same metadata lock. Resource
 // destruction remains forbidden while the native frame is outstanding.
 std::scoped_lock l(framebuffer_depth_view_mutex);
 BindScratchLease<PassTargetIdentity> lease;auto& target=lease.get();
 if(passInputIndexDirty){
  realPassInputIndex.clear();
  for(const auto& source:realPassInputs){passTargetIdentity(source.framebuffer,target);realPassInputIndex.add(target,source.images);}
  passInputIndexDirty=false;
  if(cpu::current.active){++cpu::current.passIndexBuilds;cpu::current.passIndexSources+=realPassInputIndex.sourcePasses;cpu::current.passIndexInputs+=realPassInputIndex.sourceInputs;cpu::current.passIndexTargets=realPassInputIndex.size();}
 }
 passTargetIdentity(fb,target);
 if(cpu::current.active){++cpu::current.passIndexLookups;cpu::current.passScanCandidatesAvoided+=realPassInputs.size();}
 return realPassInputIndex.find(target);
}
struct ReplayState {
 std::map<uint64_t,ReplayCommand> state;
 NativePushReplay pushState,initialPushState;
 std::vector<ReplayCommand> initial,commands;
 std::map<uint32_t,uint32_t> descriptorRanges;
 std::map<uint32_t,std::shared_ptr<DescriptorReplayPayload>> descriptorPayloads;
 VkRenderPassBeginInfo begin{};std::vector<VkClearValue> clears;
 VkRenderPassBeginInfo activeBegin{};
 VkPipeline pipeline{};
 bool final{},replaying{},recording{},inRenderPass{};
 size_t realPassIndex{SIZE_MAX};
 ImageInputs finalInputs;
 ImageInputVisits<VkDescriptorSet> finalInputVisits;
 std::map<uint32_t,VkDescriptorSet> boundSets;
 std::map<uint64_t,std::pair<VkBuffer,VkDeviceSize>> vertexValues;
};
static std::recursive_mutex replayMutex;
static std::map<VkCommandBuffer,ReplayState> replayStates;
static std::set<VkCommandBuffer> rightEyeBindingCommands;
static SnapshotBatchCache<VkCommandBuffer,VkDescriptorSet> snapshotBatchCache;
static PFN_vkBeginCommandBuffer nextBeginCommand{};
static PFN_vkCmdPushConstants nextPush{};
static bool compiledImagePlans(){static const bool enabled=kharvox::rendererDefaults::compiledImagePlans;return enabled;}
using ImagePlanKey=std::vector<uintptr_t>;
static ImagePlanCache<ImagePlanKey> imagePlanCache;
static uint64_t imagePlanHits{},imagePlanBuilds{},imageInputMerges{},imageInputSkips{},imagePlanLiveVisits{};
static void resetImagePlans(){imagePlanCache.clear();imagePlanHits=imagePlanBuilds=imageInputMerges=imageInputSkips=imagePlanLiveVisits=0;}
static void reportImagePlans(){
 if(!compiledImagePlans()||!cpu::current.active)return;
 static uint64_t count{};if(++count<=8||count%120==0)
  kharvoxnative::log::info(std::format("Native compiled image plans frame={} pid={} hits={} builds={} inputMerges={} inputSkips={} liveLayoutVisits={} fullStereo={} copiesUnchanged=true earlySceneSubmit=false",renderingPose.serial,GetCurrentProcessId(),imagePlanHits,imagePlanBuilds,imageInputMerges,imageInputSkips,imagePlanLiveVisits,!leftEyeOnlyDiagnostic()));
}
#include "NativePassLayouts.inc"
#include "NativeSourceLayouts.inc"
#include "NativeStageReadback.inc"
#include "NativeShaderAccess.inc"
#include "NativeSharedShadows.inc"
#include "NativeUniformSnapshots.inc"
#include "NativeShadowHistory.inc"
#include "NativeGpuTiming.inc"
void resetPassInputs(){std::scoped_lock l(replayMutex);resetImagePlans();rightEyeBindingCommands.clear();pendingSceneDepthClears.clear();realPassInputs.clear();realPassInputIndex.clear();passInputIndexDirty=true;doubledPassCursor=0;beginShadowHistory();beginGpuTimingFrame();}
void prepareLeftDiagnosticTargets(){
 if(!mirrorPreparationEnabled())fail("mirror allocation attempted in input-only diagnostic");
 // Build targets from the passes actually observed on the left. The normal
 // backend discovers them during right replay, which this mode never enters.
 std::scoped_lock l(replayMutex);std::set<VkFramebuffer> targets;
 for(const auto& pass:realPassInputs)targets.insert(pass.framebuffer);
 std::scoped_lock ml(mirror_mutex);
 for(auto target:targets)if(!ensure_mirror_framebuffer(target))fail("left preparation mirror unavailable");
}
void finishLeftDiagnosticPreparation(){
 if(!leftEyeOnlyDiagnostic()||!stereoPreparationEnabled())fail("unexpected diagnostic preparation boundary");
 if(mirrorPreparationEnabled())update_mirror_convergence(true);
 if(!mirrorPreparationEnabled()&&(cpu::current.seededBuffers||cpu::current.seededBytes||preservedAttachmentInputs))fail("mirror preparation executed in input-only diagnostic");
 diagnosticPreparationCompleted=true;diagnosticPreparationStable=!mirrorPreparationEnabled()||mirrors_converged();
 static uint64_t count{};
 ++count;
 if(replayPreparationEnabled()&&!inputPlanCaptureEnabled()&&(count<=12||count%120==0)){
  std::scoped_lock lock(replayMutex);size_t inputs=realPassInputs.size(),states=0,pushes=0,commands=0,payloads=0;
  for(const auto& [cb,r]:replayStates){inputs+=r.finalInputs.size();states+=r.state.size();pushes+=r.pushState.size();commands+=r.initial.size()+r.commands.size();payloads+=r.descriptorPayloads.size();}
  if(inputs)fail("image input plans retained in command-capture diagnostic");
  kharvoxnative::log::info(std::format("Native command capture audit frame={} inputPlanEntries={} stateEntries={} pushEntries={} commandEntries={} descriptorPayloads={} snapshotsEnabled=true pid={}",renderingPose.serial,inputs,states,pushes,commands,payloads,GetCurrentProcessId()));
 }
 if(!replayPreparationEnabled()&&(count<=12||count%120==0)){
  // This audit deliberately runs only at coarse reporting boundaries. Source
  // pipeline/pass/recording state must remain; deferred replay payloads must not.
  std::scoped_lock lock(replayMutex);size_t entries=realPassInputs.size();
  for(const auto& [cb,r]:replayStates)entries+=r.state.size()+r.pushState.size()+r.initial.size()+r.commands.size()+r.descriptorRanges.size()+r.descriptorPayloads.size()+r.clears.size()+r.finalInputs.size()+r.boundSets.size();
  if(entries)fail("CPU replay payload or input plan retained in snapshots-only diagnostic");
  kharvoxnative::log::info(std::format("Native replay capture audit frame={} retainedEntries={} activePipelineTracking=true snapshotsEnabled=true pid={}",renderingPose.serial,entries,GetCurrentProcessId()));
 }
 if(count<=12||count%120==0)kharvoxnative::log::info(std::format(
  "Native left preparation frame={} mirrorPrep={} replayCapture={} inputPlans={} prepStable={} observedPasses={} snapshots={} inputBytes={} seededBuffers={} seededBytes={} preservedAttachments={} rightRootCalls={} finalReplayCalls={} pid={} (exclude unstable warmup; no right scene commands)",
  renderingPose.serial,mirrorPreparationEnabled(),replayPreparationEnabled(),inputPlanCaptureEnabled(),diagnosticPreparationStable,realPassInputs.size(),usedUniformSnapshots.size(),cpu::current.copiedBytes,cpu::current.seededBuffers,cpu::current.seededBytes,preservedAttachmentInputs,frame_root_double_attempts.load(),final_pass_doubles.load(),GetCurrentProcessId()));
}
// Caller holds framebuffer_depth_view_mutex. Require the complete image,
// including stencil, rather than inferring initialization from framebuffer size.
static bool fullSceneDepthClear(const PassDefinition& pass,size_t index,VkImageView view,const VkRect2D& area){
 auto vi=image_view_infos.find(view);if(vi==image_view_infos.end())return false;
 auto ii=created_image_infos.find(vi->second.image);if(ii==created_image_infos.end())return false;
 const auto& image=ii->second;const auto& attachment=pass.attachments[index];
 const bool full=area.offset.x==0&&area.offset.y==0&&area.extent.width==image.extent.width&&area.extent.height==image.extent.height&&image.mips==1&&image.layers==1&&vi->second.range.baseMipLevel==0&&vi->second.range.baseArrayLayer==0;
 return sceneDepthClearsItself(attachment.format==VK_FORMAT_D24_UNORM_S8_UINT,full,
     attachment.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR,attachment.stencilLoadOp==VK_ATTACHMENT_LOAD_OP_CLEAR);
}
void seedDepthTargets(VkCommandBuffer cb){
 std::scoped_lock l(replayMutex);std::set<VkFramebuffer> targets;
 for(const auto& pass:realPassInputs)targets.insert(pass.framebuffer);
 std::scoped_lock ml(mirror_mutex);
 // Preserve CPU-observed FIRST use order, not the sorted framebuffer handles.
 std::set<VkImage> seen,cleared;
 {std::scoped_lock fl(framebuffer_depth_view_mutex);
  for(const auto& input:realPassInputs){
   const auto& pass=passDefinition(input.renderPass);auto fb=framebuffer_infos.find(input.framebuffer);
   if(fb==framebuffer_infos.end()||fb->second.attachments.size()!=pass.attachments.size())fail("depth seed producer metadata missing");
   for(size_t i=0;i<pass.attachments.size();++i){
    if(pass.active[i]==VK_IMAGE_LAYOUT_UNDEFINED)continue;
    const auto view=fb->second.attachments[i];auto vi=image_view_infos.find(view);if(vi==image_view_infos.end())continue;
    if(seen.insert(vi->second.image).second&&fullSceneDepthClear(pass,i,view,input.renderArea))cleared.insert(vi->second.image);
   }
  }
 }
 for(auto target:targets){
  std::vector<VkImageView> views;
  {std::scoped_lock fl(framebuffer_depth_view_mutex);auto f=framebuffer_infos.find(target);if(f==framebuffer_infos.end())fail("missing framebuffer while planning depth seed");
   // Pure depth passes include partial/cached shadow atlas updates. Their
   // untouched tiles must retain this frame's real contents in the mirror.
   bool depthOnly=!f->second.attachments.empty();
   for(auto view:f->second.attachments){auto v=image_view_infos.find(view);depthOnly&=v!=image_view_infos.end()&&v->second.format>=VK_FORMAT_D16_UNORM&&v->second.format<=VK_FORMAT_D32_SFLOAT_S8_UINT;}
   if(depthOnly)views=f->second.attachments;
  }
  if(views.empty())continue;
  if(!ensure_mirror_framebuffer(target))fail("depth seed framebuffer mirror unavailable");
  for(auto view:views){
   VkImage source{};{std::scoped_lock fl(framebuffer_depth_view_mutex);source=image_view_infos.at(view).image;}
   if(cleared.count(source)){
    pendingSceneDepthClears.insert(source);
    static uint32_t count{};if(++count<=8)kharvoxnative::log::info(std::format("Native depth seed omitted frame={} source={} lateCommand={} fullSceneClear=true",renderingPose.serial,reinterpret_cast<void*>(source),reinterpret_cast<void*>(cb)));
    continue;
   }
   seedMirrorImage(cb,view,false,"seed-depth-target");observeShadowHistorySeed(view);observeSharedShadowAtlas(view);
  }
 }
}
static void verifySceneDepthClear(const VkRenderPassBeginInfo& begin,VkCommandBuffer cb){
 if(pendingSceneDepthClears.empty())return;
 const auto& pass=passDefinition(begin.renderPass);std::scoped_lock fl(framebuffer_depth_view_mutex);
 auto fb=framebuffer_infos.find(begin.framebuffer);if(fb==framebuffer_infos.end())fail("right depth clear framebuffer missing");
 for(size_t i=0;i<pass.attachments.size();++i){
  if(pass.active[i]==VK_IMAGE_LAYOUT_UNDEFINED)continue;
  const auto view=fb->second.attachments[i];auto vi=image_view_infos.find(view);if(vi==image_view_infos.end()||!pendingSceneDepthClears.count(vi->second.image))continue;
  if(!fullSceneDepthClear(pass,i,view,begin.renderArea))fail("right scene depth first use does not clear the complete image");
  pendingSceneDepthClears.erase(vi->second.image);
  static uint32_t count{};if(++count<=8)kharvoxnative::log::info(std::format("Native right depth initializer frame={} source={} producerCommand={} fullClearVerified=true lateSeed=false",renderingPose.serial,reinterpret_cast<void*>(vi->second.image),reinterpret_cast<void*>(cb)));
 }
}
template<class Inputs> static void captureSetInput(Inputs& into,VkDescriptorSet set){
 auto found=descriptor_image_sets.find(set);if(found==descriptor_image_sets.end())return;
 for(auto&[binding,rec]:found->second){
  if(!rec.image_view)continue;
  auto [it,added]=into.emplace(rec.image_view,rec.layout);
  if(!added&&it->second!=rec.layout)fail("one render pass requires conflicting descriptor image layouts");
 }
}
static void captureInputs(ImageInputs& into,const std::map<uint32_t,VkDescriptorSet>& sets,ImageInputVisits<VkDescriptorSet>* visited=nullptr){
 cpu::Scope profile(cpu::CaptureImageInputs);
 std::scoped_lock l(swapchains_mutex);
 for(auto&[index,set]:sets){
  const auto before=compiledImagePlans()&&visited?bindingMutationClock.stamp():std::optional<uint64_t>{};
  if(visited&&visited->contains(set,before)){++imageInputSkips;continue;}
  captureSetInput(into,set);++imageInputMerges;
  if(visited)visited->remember(set,before,bindingMutationClock.stamp());
 }
}
using BindImageInputs=OrderedWorkMap<VkImageView,VkImageLayout>;
static void captureInputs(BindImageInputs& into,std::span<const VkDescriptorSet> sets){
 cpu::Scope profile(cpu::CaptureImageInputs);
 std::scoped_lock l(swapchains_mutex);
 for(auto set:sets)captureSetInput(into,set);
}
struct LayoutWork {
 std::vector<VkImage> attachments;
 OrderedWorkMap<std::tuple<VkImage,uint32_t,uint32_t>,VkImageMemoryBarrier> pending;
 std::vector<VkImageMemoryBarrier> barriers;
};
template<class Inputs> static void prepareInputLayouts(VkCommandBuffer cb,const Inputs& inputs,VkFramebuffer target,bool initialize=false){
 cpu::Scope profile(cpu::PrepareImageLayouts);
 if(inputs.empty())return;
 std::scoped_lock l(mirror_mutex);
 BindScratchLease<LayoutWork> lease;auto& work=lease.get();
 auto& attachments=work.attachments;attachments.clear();
 {std::scoped_lock fl(framebuffer_depth_view_mutex);auto fb=framebuffer_infos.find(target);
  if(fb!=framebuffer_infos.end())for(auto view:fb->second.attachments){auto v=image_view_infos.find(view);if(v!=image_view_infos.end())attachments.push_back(v->second.image);}}
 auto& pending=work.pending;pending.clear();
 for(auto&[view,want]:inputs){
  ImageViewInfo vi{};{std::scoped_lock fl(framebuffer_depth_view_mutex);auto i=image_view_infos.find(view);if(i==image_view_infos.end())continue;vi=i->second;}
  // Attachment transitions belong to the render pass itself, including depth
  // read-only/input-attachment layouts. Never override them with a sampled use.
  if(std::find(attachments.begin(),attachments.end(),vi.image)!=attachments.end())continue;
  auto mi=mirrored_images.find(vi.image);if(mi==mirrored_images.end())continue;
  const auto image=mi->second;
  const auto have=mirror_layout_of(image);
  if(have==want)continue;
  if(want==VK_IMAGE_LAYOUT_UNDEFINED)continue;
  if(!initialize&&!mirror_images_written.count(image))continue; // warming; bind readiness gate still applies
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=have;b.newLayout=want;
  b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
  b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange=vi.range;
  auto range=checked_mirror_range(image,vi.range);
  if(initialize)range.levelCount=1; // framebuffer attachment selects its base mip
  if(vi.format==VK_FORMAT_D24_UNORM_S8_UINT||vi.format==VK_FORMAT_D32_SFLOAT_S8_UINT||vi.format==VK_FORMAT_D16_UNORM_S8_UINT)
   b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
  for(uint32_t layer=range.baseArrayLayer;layer<range.baseArrayLayer+range.layerCount;++layer)for(uint32_t mip=range.baseMipLevel;mip<range.baseMipLevel+range.levelCount;++mip){
   b.subresourceRange={b.subresourceRange.aspectMask,mip,1,layer,1};auto [it,added]=pending.emplace(std::make_tuple(image,mip,layer),b);if(!added&&it->second.newLayout!=want)fail("overlapping descriptors require conflicting image layouts");}
 }
 if(!pending.empty()){
  const auto resume=pauseForLayouts(cb);
  validateSeedCommand(cb);
  auto& barriers=work.barriers;barriers.clear();for(auto&[key,b]:pending)append_mirror_transitions(barriers,b);
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};memory.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
  real_cmd_pipeline_barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&memory,0,nullptr,uint32_t(barriers.size()),barriers.data());
  resumeAfterLayouts(cb,resume);
 }
}
static void prepareAttachments(VkCommandBuffer cb,const VkRenderPassBeginInfo& begin){
 auto& d=passDefinition(begin.renderPass);ImageInputs inputs;
 {std::scoped_lock ml(mirror_mutex);if(!ensure_mirror_framebuffer(begin.framebuffer))fail("native framebuffer mirror missing before pass");}
 {std::scoped_lock fl(framebuffer_depth_view_mutex);auto f=framebuffer_infos.find(begin.framebuffer);if(f==framebuffer_infos.end()||f->second.attachments.size()!=d.attachments.size())fail("native framebuffer attachment metadata mismatch");
  for(size_t i=0;i<d.attachments.size();++i)inputs[f->second.attachments[i]]=d.attachments[i].initialLayout;}
 prepareInputLayouts(cb,inputs,VK_NULL_HANDLE,true);
}
static void prepareDescriptorImagePlan(VkCommandBuffer cb,std::span<const VkDescriptorSet> sets,VkFramebuffer target){
 cpu::Scope profile(cpu::PrepareImageLayouts);
 BindScratchLease<ImagePlanKey> keyLease;auto& key=keyLease.get();key.clear();
 key.push_back(reinterpret_cast<uintptr_t>(target));
 for(auto set:sets)key.push_back(reinterpret_cast<uintptr_t>(set));
 const auto before=bindingMutationClock.stamp();
 auto plan=imagePlanCache.find(key,before);
 if(plan){++imagePlanHits;}
 else {
  ++imagePlanBuilds;
  BindScratchLease<BindImageInputs> inputLease;auto& inputs=inputLease.get();inputs.clear();
  captureInputs(inputs,sets);
  auto building=std::make_shared<ImageLayoutPlan>();building->reserve(inputs.size());
  // Metadata locks are not nested. The mutation stamp covers descriptor,
  // framebuffer, view and image creation/destruction. Plans own all values.
  {std::scoped_lock fl(framebuffer_depth_view_mutex);
   BindScratchLease<std::vector<VkImage>> attachmentLease;auto& attachments=attachmentLease.get();attachments.clear();
   auto fb=framebuffer_infos.find(target);
   if(fb!=framebuffer_infos.end())for(auto view:fb->second.attachments){auto vi=image_view_infos.find(view);if(vi!=image_view_infos.end())attachments.push_back(vi->second.image);}
   for(const auto& [view,want]:inputs){
    auto vi=image_view_infos.find(view);if(vi==image_view_infos.end())continue;
    const auto& v=vi->second;
    if(std::find(attachments.begin(),attachments.end(),v.image)!=attachments.end())continue;
    building->push_back({v.image,want,v.range,v.format});
   }
  }
  plan=std::move(building);imagePlanCache.put(key,plan,before,bindingMutationClock.stamp());
 }
 // Cache hits still resolve the CURRENT mirror mapping, initialization and
 // layouts. New mirrors, render passes, barriers and rewinds cannot be hidden
 // by a CPU metadata cache. In particular, never cache a 'no barrier' verdict.
 if(plan->empty())return;
 std::scoped_lock ml(mirror_mutex);
 BindScratchLease<LayoutWork> workLease;auto& work=workLease.get();work.pending.clear();
 imagePlanLiveVisits+=plan->size();
 visitImageLayoutPlan<VkImage>(*plan,false,
  [](VkImage source){auto it=mirrored_images.find(source);return it==mirrored_images.end()?VK_NULL_HANDLE:it->second;},
  [](VkImage image){return mirror_layout_of(image);},
  [](VkImage image){return mirror_images_written.count(image)!=0;},
  [&](const ImageLayoutRequirement& input,VkImage image,VkImageLayout have){
   VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=have;b.newLayout=input.want;
   b.srcAccessMask=b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
   b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange=input.range;
   const auto range=checked_mirror_range(image,input.range);
   if(input.format==VK_FORMAT_D24_UNORM_S8_UINT||input.format==VK_FORMAT_D32_SFLOAT_S8_UINT||input.format==VK_FORMAT_D16_UNORM_S8_UINT)
    b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
   for(uint32_t layer=range.baseArrayLayer;layer<range.baseArrayLayer+range.layerCount;++layer)for(uint32_t mip=range.baseMipLevel;mip<range.baseMipLevel+range.levelCount;++mip){
    b.subresourceRange={b.subresourceRange.aspectMask,mip,1,layer,1};auto [it,added]=work.pending.emplace(std::make_tuple(image,mip,layer),b);
    if(!added&&it->second.newLayout!=input.want)fail("overlapping descriptors require conflicting image layouts");
   }
  });
 if(work.pending.empty())return;
 const auto resume=pauseForLayouts(cb);validateSeedCommand(cb);
 work.barriers.clear();for(auto& [key,b]:work.pending)append_mirror_transitions(work.barriers,b);
 VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};memory.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
 real_cmd_pipeline_barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&memory,0,nullptr,uint32_t(work.barriers.size()),work.barriers.data());
 resumeAfterLayouts(cb,resume);
}
static bool finalTarget(VkFramebuffer fb,VkImage* target=nullptr){
 VkImage image{};{std::scoped_lock lock(framebuffer_depth_view_mutex);auto view=framebuffer_color0_views.find(fb);if(view==framebuffer_color0_views.end())return false;auto im=image_view_to_image.find(view->second);if(im==image_view_to_image.end())return false;image=im->second;}
 std::scoped_lock lock(swapchains_mutex);for(auto&[sc,rec]:swapchains)for(auto im:rec.images)if(im==image){if(target)*target=image;return true;}return false;
}
template<class Command>
static void record(VkCommandBuffer cb,uint64_t key,const Command& fn,bool state=true){
 if(!replayPreparationEnabled()){fn();return;}
 recordReplayCommand(replayStates[cb],key,fn,state);
}
#include "NativeCommandLifetime.inc"
static void preserveStorageAttachmentInputs(VkCommandBuffer cb,const VkRenderPassBeginInfo& begin){
 if(!mirrorPreparationEnabled())return;
 // A color attachment may later become a readable/writable storage image.
 // Preserve its contents before the first real pass, once per frame. Seeding
 // after FrameRoot copied already tonemapped eye-0 pixels into eye 1's LOAD
 // passes. Run 18 isolated that error with matching HDR resource readbacks.
 // Warm-up may allocate mirrors, but cannot publish until provenance is valid.
 if(!inside_real_frame_root||!outstanding||!mirrors_converged())return;
 std::vector<VkImageView> inputs;
 {std::scoped_lock fl(framebuffer_depth_view_mutex);auto f=framebuffer_infos.find(begin.framebuffer);if(f==framebuffer_infos.end())fail("unknown real framebuffer while preserving storage inputs");
  for(auto view:f->second.attachments){auto vi=image_view_infos.find(view);if(vi==image_view_infos.end())continue;auto ii=created_image_infos.find(vi->second.image);if(ii==created_image_infos.end())continue;
   if((ii->second.usage&VK_IMAGE_USAGE_STORAGE_BIT)&&ii->second.mips==1&&ii->second.layers==1&&vi->second.format<VK_FORMAT_D16_UNORM)inputs.push_back(view);
  }
 }
 if(inputs.empty())return;
 validateSeedCommand(cb);
 std::scoped_lock ml(mirror_mutex);
 for(auto view:inputs){if(!ensure_mirror_storage_image(view))fail("storage attachment preservation mirror unavailable");seedMirrorImage(cb,view,false,"seed-before-left-attachment");}
 preservedAttachmentInputs=true;
 {std::scoped_lock sl(submissionMutex);if(submittedCommands.count(cb))fail("storage input preservation after submission");recordedCommands.insert(cb);}
 static uint32_t count{};if(++count<=8)kharvoxnative::log::info(std::format("Native storage input preservation frame={} attachments={} beforeRealPass=true",renderingPose.serial,inputs.size()));
}
static void VKAPI_CALL replayBeginPass(VkCommandBuffer cb,const VkRenderPassBeginInfo* info,VkSubpassContents contents){
 cpu::Scope profile(cpu::ReplayPass);
 std::scoped_lock lock(replayMutex);auto&r=replayStates[cb];
 
 beginGpuPass(cb,info);
 // DOOM caches ORIGINAL bindings across roots. Vulkan still has our left
 // snapshot/vertex clones bound when the game omits a redundant bind. Replay
 // the current logical state in right-eye context before its first pass.
 if(inside_frame_root_double){
  if(r.inRenderPass)fail("right eye inherited bindings inside an active pass");
  if(restoreEyeBindingsOnce(cb,rightEyeBindingCommands,r.state)){
   static uint32_t count{};if(++count<=8)kharvoxnative::log::info(std::format("Native right eye inherited bindings frame={} command={} states={} beforeFirstPass=true",renderingPose.serial,reinterpret_cast<void*>(cb),r.state.size()));
  }
 }
 if(info)preserveStorageAttachmentInputs(cb,*info);
 if(inside_real_frame_root&&info&&inputPlanCaptureEnabled()){r.realPassIndex=realPassInputs.size();realPassInputs.push_back({info->framebuffer,info->renderPass,{}, {},info->renderArea});passInputIndexDirty=true;}
 else r.realPassIndex=SIZE_MAX;
 if(inside_frame_root_double&&info){
  verifySceneDepthClear(*info,cb);
  // DOOM can collapse/repeat passes and select a different cached framebuffer
  // for the same attachments when load operations change. Input requirements
  // follow ordered attachment identity, not invocation ordinals or handles.
  const auto* inputs=indexedPassInputs(info->framebuffer);
  if(inputs&&inputs->conflict)fail("repeated target has conflicting input layouts");
  // New second-eye targets are handled by exact descriptor-bind preflight.
  ++doubledPassCursor;
  if(inputs)prepareInputLayouts(cb,inputs->images,info->framebuffer);
  prepareAttachments(cb,*info);
 }
 if(info)r.activeBegin=*info;
 r.inRenderPass=true;
 r.final=info&&rootDone&&finalTarget(info->framebuffer);
 if(r.final&&replayPreparationEnabled()){
  trace::setEye(0);trace::row("final-original-begin",0,{reinterpret_cast<uintptr_t>(cb)});
  if(info->pNext||contents!=VK_SUBPASS_CONTENTS_INLINE)fail("final pass extensions/secondary command buffers unsupported");
  r.begin=*info;r.clears.clear();if(info->clearValueCount)r.clears.assign(info->pClearValues,info->pClearValues+info->clearValueCount);
  r.initial.clear();for(auto&[key,fn]:r.state)r.initial.push_back(fn);r.initialPushState=r.pushState;r.commands.clear();
  r.finalInputs.clear();r.finalInputVisits.clear();if(inputPlanCaptureEnabled())captureInputs(r.finalInputs,r.boundSets,&r.finalInputVisits);
  inside_final_pass_real=true;last_final_pass_framebuffer=info->framebuffer;
 }
 hook_cmd_begin_render_pass(cb,info,contents);

 if(info)sourcePassState(*info,true);
 if(inside_frame_root_double){std::scoped_lock ml(mirror_mutex);attachmentState(r.activeBegin,true);}
}
static void VKAPI_CALL replayEndPass(VkCommandBuffer cb){
 cpu::Scope profile(cpu::ReplayPass);
 std::scoped_lock lock(replayMutex);auto&r=replayStates[cb];hook_cmd_end_render_pass(cb);r.inRenderPass=false;
 endGpuPass(cb);
 sourcePassState(r.activeBegin,false);
 if(inside_frame_root_double){std::scoped_lock ml(mirror_mutex);attachmentState(r.activeBegin,false);}

 captureStagePass(cb);
 if(!r.final)return;inside_final_pass_real=false;r.final=false;
 if(leftEyeOnlyDiagnostic()){
  if(!finalTarget(r.activeBegin.framebuffer,&diagnosticFinalImage))fail("left diagnostic final target lost");
  {std::scoped_lock sl(submissionMutex);if(submittedCommands.count(cb))fail("left diagnostic final recorded after submission");recordedCommands.insert(cb);}
  endFinal();return;
 }
 bool mirrorReady;{std::scoped_lock ml(mirror_mutex);mirrorReady=ensure_mirror_framebuffer(r.begin.framebuffer);}
 if(!mirrorReady)fail("completion framebuffer mirror unavailable");
 if(!mirrors_converged())return;
 trace::setEye(1);trace::row("final-replay-begin",0,{reinterpret_cast<uintptr_t>(cb)});
 r.replaying=true;final_pass_mirror_color=VK_NULL_HANDLE;inside_final_pass_double=true;
 const auto finalGpuSpan=beginGpuSpan(cb,"final-replay",1);
 prepareInputLayouts(cb,r.finalInputs,r.begin.framebuffer);
 for(auto&fn:r.initial)fn();
 r.initialPushState.replay([cb](auto layout,auto stages,auto offset,auto size,const void* data){nextPush(cb,layout,stages,offset,size,data);});
 auto begin=r.begin;begin.pClearValues=r.clears.empty()?nullptr:r.clears.data();
 prepareAttachments(cb,begin);r.activeBegin=begin;
 activateGpuPass(cb,finalGpuSpan,&begin);
 hook_cmd_begin_render_pass(cb,&begin,VK_SUBPASS_CONTENTS_INLINE);
 {std::scoped_lock ml(mirror_mutex);attachmentState(begin,true);}
 r.inRenderPass=true;
 for(auto&fn:r.commands)fn();
 hook_cmd_end_render_pass(cb);
 {std::scoped_lock ml(mirror_mutex);attachmentState(begin,false);}
 r.inRenderPass=false;
 inside_final_pass_double=false;r.replaying=false;trace::setEye(2);
 endGpuPass(cb);
 // Restore the game's post-pass state so subsequent recording sees no clones.
 for(auto&[key,fn]:r.state)fn();
 r.pushState.replay([cb](auto layout,auto stages,auto offset,auto size,const void* data){nextPush(cb,layout,stages,offset,size,data);});
 final_pass_doubles.fetch_add(1);endFinal();
}
static void VKAPI_CALL replayPipeline(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipeline pipeline){cpu::Scope profile(cpu::ReplayPipeline);std::scoped_lock lock(replayMutex);auto fn=[=]{if(point==VK_PIPELINE_BIND_POINT_GRAPHICS)executeReplayPipeline(replayStates[cb],pipeline,[=]{hook_cmd_bind_pipeline(cb,point,pipeline);});else hook_cmd_bind_pipeline(cb,point,pipeline);};if(point==VK_PIPELINE_BIND_POINT_GRAPHICS)record(cb,1,fn);else fn();}
static void executeDescriptorBind(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* dynamic){
 notePassWork(cb,true);
 noteGpuDescriptorImages(cb,count,sets);
 if(inside_frame_root_double||inside_final_pass_double){
  auto& r=replayStates[cb];const auto target=r.inRenderPass?r.activeBegin.framebuffer:VK_NULL_HANDLE;
  if(compiledImagePlans())prepareDescriptorImagePlan(cb,std::span<const VkDescriptorSet>(sets,count),target);
  else {BindScratchLease<BindImageInputs> lease;auto& inputs=lease.get();inputs.clear();captureInputs(inputs,std::span<const VkDescriptorSet>(sets,count));prepareInputLayouts(cb,inputs,target);}
 }
 hook_cmd_bind_descriptor_sets(cb,point,layout,first,count,sets,dynamicCount,dynamic);
 if(point==VK_PIPELINE_BIND_POINT_GRAPHICS)captureStageUniforms(cb,count,sets,dynamicCount,dynamic);
}
static void replayDescriptorsFull(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* dynamic){
 cpu::Scope profile(cpu::ReplayDescriptors);
 std::scoped_lock lock(replayMutex);
 if(!replayPreparationEnabled()){executeDescriptorBind(cb,point,layout,first,count,sets,dynamicCount,dynamic);return;}
 // Compute has no deferred final-pass state; consume valid caller arguments
 // synchronously, without constructing an owned graphics replay command.
 if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS){if(cpu::current.active)++cpu::current.borrowedComputeBinds;executeDescriptorBind(cb,point,layout,first,count,sets,dynamicCount,dynamic);return;}
 auto&r=replayStates[cb];if(!prepareDescriptorRange(r,first,count))fail("partially overlapping descriptor state ranges unsupported");
 const uint64_t key=0x10000ull+first;bool reused{};
 // Keep descriptor contents, layout plans and snapshot checks live on every
 // call. Only equal, already-owned command arguments can avoid reconstruction.
 for(uint32_t i=0;i<count;i++)r.boundSets[first+i]=sets[i];
 if(inputPlanCaptureEnabled()&&r.inRenderPass&&inside_real_frame_root&&r.realPassIndex<realPassInputs.size()){auto& pass=realPassInputs[r.realPassIndex];captureInputs(pass.images,r.boundSets,&pass.visited);passInputIndexDirty=true;}
 if(inputPlanCaptureEnabled()&&r.final&&!r.replaying)captureInputs(r.finalInputs,r.boundSets,&r.finalInputVisits);
 const auto previous=r.descriptorPayloads.find(first);
 const bool identical=previous!=r.descriptorPayloads.end()&&previous->second&&sameReplayArguments(*previous->second,point,layout,first,std::span<const VkDescriptorSet>(sets,count),std::span<const uint32_t>(dynamic,dynamicCount));
 if(reuseReplayCommand(r,key,identical,[&]{executeDescriptorBind(cb,point,layout,first,count,sets,dynamicCount,dynamic);})){if(cpu::current.active)++cpu::current.identicalDescriptorCommands;return;}

 auto payload=prepareReplayPayload(r.descriptorPayloads[first],r.state.contains(key),reused);
 if(cpu::current.active){if(reused)++cpu::current.reusedReplayPayloads;else ++cpu::current.newReplayPayloads;}
 payload->point=point;payload->layout=layout;payload->first=first;
 payload->sets.clear();if(count)payload->sets.assign(sets,sets+count);
 payload->offsets.clear();if(dynamicCount)payload->offsets.assign(dynamic,dynamic+dynamicCount);
 auto fn=[cb,payload]{
  // Pin while executing: recursive recording must not mutate arguments that
  // this invocation is still reading, even when invoked directly from state.
  auto executing=payload;
  executeDescriptorBind(cb,executing->point,executing->layout,executing->first,uint32_t(executing->sets.size()),executing->sets.data(),uint32_t(executing->offsets.size()),executing->offsets.data());
 };
 record(cb,key,fn);
}
// Retain complete baseline preparation, including draw-time access validation.
static void VKAPI_CALL replayDescriptors(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount,const uint32_t* dynamic){
 replayDescriptorsFull(cb,point,layout,first,count,sets,dynamicCount,dynamic);
}
static void VKAPI_CALL replayViewport(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkViewport* values){std::scoped_lock lock(replayMutex);for(uint32_t i=0;i<count;i++){auto v=values[i];auto index=first+i;record(cb,0x20000+index,[=]{hook_cmd_set_viewport(cb,index,1,&v);});}}
static void VKAPI_CALL replayScissor(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkRect2D* values){std::scoped_lock lock(replayMutex);for(uint32_t i=0;i<count;i++){auto v=values[i];auto index=first+i;record(cb,0x30000+index,[=]{hook_cmd_set_scissor(cb,index,1,&v);});}}
static void VKAPI_CALL replayVertex(VkCommandBuffer cb,uint32_t first,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets){
 cpu::Scope profile(cpu::ReplayVertex);
 if(!replayPreparationEnabled()){hook_cmd_bind_vertex_buffers(cb,first,count,buffers,offsets);return;}
 std::scoped_lock lock(replayMutex);auto& r=replayStates[cb];
 if(cpu::current.active){++cpu::current.vertexBindBatches;cpu::current.vertexBindElements+=count;}
 const auto reused=recordReplayBatchCached(r,r.vertexValues,0x40000,first,count,
  [=](uint32_t i){return std::make_pair(buffers[i],offsets[i]);},
  [=](uint32_t i){const auto b=buffers[i];const auto o=offsets[i];const auto index=first+i;
   return [=]{hook_cmd_bind_vertex_buffers(cb,index,1,&b,&o);};},
  [=]{std::vector<VkBuffer> ownedBuffers(buffers,buffers+count);std::vector<VkDeviceSize> ownedOffsets(offsets,offsets+count);
   return [cb,first,ownedBuffers=std::move(ownedBuffers),ownedOffsets=std::move(ownedOffsets)]{
    hook_cmd_bind_vertex_buffers(cb,first,uint32_t(ownedBuffers.size()),ownedBuffers.data(),ownedOffsets.data());};},
  [=]{hook_cmd_bind_vertex_buffers(cb,first,count,buffers,offsets);});
 if(cpu::current.active)cpu::current.identicalVertexStates+=reused;
}
static void VKAPI_CALL replayIndex(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,VkIndexType type){std::scoped_lock lock(replayMutex);record(cb,2,[=]{hook_cmd_bind_index_buffer(cb,buffer,offset,type);});}
static void VKAPI_CALL replayPush(VkCommandBuffer cb,VkPipelineLayout layout,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void* data){
 if(!replayPreparationEnabled()){nextPush(cb,layout,stages,offset,size,data);return;}
 std::scoped_lock lock(replayMutex);
 try{recordNativePush(replayStates[cb],layout,stages,offset,size,data,[cb](auto l,auto s,auto o,auto n,const void* bytes){nextPush(cb,l,s,o,n,bytes);});}
 catch(const std::runtime_error& error){fail(error.what());}
}
static void traceNativeDraw(VkCommandBuffer cb,uint32_t vertices){
 static const bool enabled=kharvox::runtimeFileExists(L"capture_native_stereo_once")||sequenceDiagnostic();
 if(!enabled||phase()!=Phase::Ready||vertices>6||(!inside_real_frame_root&&!inside_frame_root_double)||nativeFrameTrace.size()>2048)return;
 auto& r=replayStates[cb];std::string line=std::format("side={} pipeline={} framebuffer={} vertices={}",inside_frame_root_double?"right":"left",reinterpret_cast<void*>(r.pipeline),reinterpret_cast<void*>(r.activeBegin.framebuffer),vertices);
 {std::scoped_lock fl(framebuffer_depth_view_mutex);auto f=framebuffer_infos.find(r.activeBegin.framebuffer);if(f!=framebuffer_infos.end()){line+=std::format(" extent={}x{}",f->second.width,f->second.height);for(size_t i=0;i<f->second.attachments.size();++i){auto v=image_view_infos.find(f->second.attachments[i]);if(v!=image_view_infos.end())line+=std::format(" out{}={}({})",i,reinterpret_cast<void*>(v->second.image),int(v->second.format));}}}
 {std::scoped_lock sl(swapchains_mutex);for(auto&[index,set]:r.boundSets){auto images=descriptor_image_sets.find(set);if(images!=descriptor_image_sets.end())for(auto&[binding,image]:images->second){auto v=image_view_to_image.find(image.image_view);line+=std::format(" in{}.{}={} view={}",index,binding,v==image_view_to_image.end()?nullptr:reinterpret_cast<void*>(v->second),reinterpret_cast<void*>(image.image_view));}auto bufs=descriptor_sets.find(set);if(bufs!=descriptor_sets.end())for(auto&[binding,buffer]:bufs->second)line+=std::format(" buf{}.{}={}+{}:{}",index,binding,reinterpret_cast<void*>(buffer.buffer),buffer.offset,buffer.range);}}
 nativeFrameTrace.push_back(std::move(line));
 if(inside_frame_root_double){
  for(auto&[index,set]:r.boundSets){DfCloneViews effective{};if(df_lookup_clone_views(set,effective)){
   std::string bound=std::format("effective pipeline={} setIndex={} source={} clone={}",reinterpret_cast<void*>(r.pipeline),index,reinterpret_cast<void*>(set),reinterpret_cast<void*>(effective.clone));
   std::scoped_lock ml(mirror_mutex);std::scoped_lock fl(framebuffer_depth_view_mutex);
   for(auto&[binding,view]:effective.bindings){VkImage image{};bool mirror=false;auto own=image_view_infos.find(view);if(own!=image_view_infos.end())image=own->second.image;for(auto&[source,mv]:mirrored_views)if(mv.view==view){image=mv.image;mirror=true;break;}bound+=std::format(" b{}:view={} image={} mirror={}",binding,reinterpret_cast<void*>(view),reinterpret_cast<void*>(image),mirror);}
   nativeFrameTrace.push_back(std::move(bound));
  }}
 }
}
static void VKAPI_CALL replayDraw(VkCommandBuffer cb,uint32_t a,uint32_t b,uint32_t c,uint32_t d){cpu::Scope profile(cpu::ReplayDraw);std::scoped_lock lock(replayMutex);traceNativeDraw(cb,a);record(cb,0,[=]{notePassWork(cb,false,uint64_t(a)*b);validateStorageSnapshotsForDraw(cb);hook_cmd_draw(cb,a,b,c,d);},false);}
static void VKAPI_CALL replayDrawIndexed(VkCommandBuffer cb,uint32_t a,uint32_t b,uint32_t c,int32_t d,uint32_t e){cpu::Scope profile(cpu::ReplayDraw);std::scoped_lock lock(replayMutex);traceNativeDraw(cb,a);record(cb,0,[=]{notePassWork(cb,false,uint64_t(a)*b);{validateStorageSnapshotsForDraw(cb);hook_cmd_draw_indexed(cb,a,b,c,d,e);}},false);}
static void VKAPI_CALL replayDrawIndirect(VkCommandBuffer cb,VkBuffer b,VkDeviceSize o,uint32_t c,uint32_t st){cpu::Scope profile(cpu::ReplayDraw);std::scoped_lock lock(replayMutex);record(cb,0,[=]{notePassWork(cb,false,0,true);validateStorageSnapshotsForDraw(cb);hook_cmd_draw_indirect(cb,b,o,c,st);},false);}
static void VKAPI_CALL replayDrawIndexedIndirect(VkCommandBuffer cb,VkBuffer b,VkDeviceSize o,uint32_t c,uint32_t st){cpu::Scope profile(cpu::ReplayDraw);std::scoped_lock lock(replayMutex);record(cb,0,[=]{notePassWork(cb,false,0,true);validateStorageSnapshotsForDraw(cb);hook_cmd_draw_indexed_indirect(cb,b,o,c,st);},false);}
static void VKAPI_CALL replayDepthBias(VkCommandBuffer cb,float a,float b,float c){std::scoped_lock lock(replayMutex);record(cb,3,[=]{hook_cmd_set_depth_bias(cb,a,b,c);});}
template<class Bind>
static void recordStencil(VkCommandBuffer cb,uint64_t base,VkStencilFaceFlags faces,uint32_t value,Bind bind){
 std::scoped_lock lock(replayMutex);
 const auto emit=[=](uint32_t face){bind(cb,face,value);};
 if(!replayPreparationEnabled()){emit(faces);return;}
 recordReplayFaces(replayStates[cb],base,faces,emit);
}
static void VKAPI_CALL replayStencilCompare(VkCommandBuffer cb,VkStencilFaceFlags a,uint32_t b){recordStencil(cb,0x50000,a,b,hook_cmd_set_stencil_compare_mask);}
static void VKAPI_CALL replayStencilRef(VkCommandBuffer cb,VkStencilFaceFlags a,uint32_t b){recordStencil(cb,0x60000,a,b,hook_cmd_set_stencil_reference);}
static PFN_vkCmdSetBlendConstants nextBlend{};
static PFN_vkCmdSetDepthBounds nextDepthBounds{};
static PFN_vkCmdSetLineWidth nextLineWidth{};
static PFN_vkCmdSetStencilWriteMask nextStencilWrite{};
static void VKAPI_CALL replayBlend(VkCommandBuffer cb,const float values[4]){std::scoped_lock l(replayMutex);std::array<float,4> v{values[0],values[1],values[2],values[3]};record(cb,4,[=]{nextBlend(cb,v.data());});}
static void VKAPI_CALL replayDepthBounds(VkCommandBuffer cb,float a,float b){std::scoped_lock l(replayMutex);record(cb,5,[=]{nextDepthBounds(cb,a,b);});}
static void VKAPI_CALL replayLineWidth(VkCommandBuffer cb,float a){std::scoped_lock l(replayMutex);record(cb,6,[=]{nextLineWidth(cb,a);});}
static void VKAPI_CALL replayStencilWrite(VkCommandBuffer cb,VkStencilFaceFlags a,uint32_t b){recordStencil(cb,0x70000,a,b,nextStencilWrite);}
template<typename T,int Tag> struct UnsupportedFinal;
template<int Tag,typename R,typename... A> struct UnsupportedFinal<R(VKAPI_PTR*)(VkCommandBuffer,A...),Tag>{
 using Fn=R(VKAPI_PTR*)(VkCommandBuffer,A...);static inline Fn next{};
 static R VKAPI_PTR call(VkCommandBuffer cb,A... args){std::scoped_lock l(replayMutex);auto it=replayStates.find(cb);if(inside_frame_root_double||inside_final_pass_double||(!leftEyeOnlyDiagnostic()&&it!=replayStates.end()&&it->second.final))fail("unsupported Vulkan command in native replay");return next(cb,args...);}
};
static PFN_vkEndCommandBuffer nextEndCommand{};
static VkResult VKAPI_CALL replayEndCommand(VkCommandBuffer cb){std::scoped_lock lock(replayMutex);const auto result=nextEndCommand(cb);replayStates[cb].recording=result!=VK_SUCCESS;return result;}
void validateSeedCommand(VkCommandBuffer cb){std::scoped_lock lock(replayMutex);auto it=replayStates.find(cb);if(it==replayStates.end()||!canSeedStorage(it->second.recording,it->second.inRenderPass,false))fail("storage seed requires a recording command buffer outside a render pass");}
static bool loadingDrainCommandClosed(){
 std::scoped_lock lock(replayMutex);
 auto it=replayStates.find(reinterpret_cast<VkCommandBuffer>(loadingDrain.command()));
 return it!=replayStates.end()&&!it->second.recording&&!it->second.inRenderPass;
}
bool prepareDoubleBoundary(VkCommandBuffer cb) {
 std::scoped_lock lock(replayMutex);
 const auto it=replayStates.find(cb);
 const bool known=it!=replayStates.end();
 const bool recording=known&&it->second.recording;
 const bool inPass=known&&it->second.inRenderPass;
 if(cb&&canSeedStorage(recording,inPass,false)){captureStageLeftBoundary();finishUniformSnapshots(0);return true;}
 bool canDrain=false;LoadingDrain candidateDrain;
 {std::scoped_lock l(submissionMutex);
  canDrain=!loadingDrain.active()&&candidateDrain.begin(reinterpret_cast<uintptr_t>(cb),known&&recording&&inPass,
   preservedAttachmentInputs&&recordedCommands.size()==1&&recordedCommands.count(cb)&&submittedCommands.empty(),
   stereoWorkStarted||inside_frame_root_double||inside_final_pass_double||rootDone.load()!=0||finalDone.load()!=0);
 }
 if(canDrain){
  // Only the original eye has recorded work. Its immutable inputs still need
  // initialization before the engine can submit the open command buffer.
  // This uses the existing private copy/fence, never commands in its open pass.
  finishUniformSnapshots(0);
  loadingDrain=candidateDrain; // The private input-copy pool is already complete.
  rootDeferred=true;rootDone=0;finalDone=0;
  frame_root_double_enabled=false;final_pass_double_armed=false;
  if(phase()==Phase::Ready)enterPhase(Phase::Warming);
  kharvoxnative::log::info(std::format("Native loading drain scheduled frame={} cb={} recording=true inRenderPass=true originalOnly=true ownershipRetainedUntilPresent=true",renderingPose.serial,reinterpret_cast<void*>(cb)));
  std::ofstream(kharvox::runtimePathA("renderer_status.txt"))<<"Renderer: Native Stereo warming after load boundary; no stereo pair";
  return false;
 }
 // This is before ANY injected command in this transaction. Never use this
 // deferral as an in-process recovery from partially recorded native work.
 {std::scoped_lock l(submissionMutex);if(!recordedCommands.empty()){
   kharvoxnative::log::info(std::format("Native boundary refusal frame={} cb={} known={} recording={} inRenderPass={} recordedCommands={} submittedCommands={} preservedAttachments={} tid={} pid={}",
    renderingPose.serial,reinterpret_cast<void*>(cb),known,recording,inPass,recordedCommands.size(),submittedCommands.size(),preservedAttachmentInputs,GetCurrentThreadId(),GetCurrentProcessId()));
   fail("cannot defer after native recording started");
 }}
 static uint64_t deferred{};
 if(++deferred<=8||deferred%120==0)
  kharvoxnative::log::info(std::format("Native boundary deferred frame={} cb={} known={} recording={} inRenderPass={} count={}",
   renderingPose.serial,reinterpret_cast<void*>(cb),known,recording,inPass,deferred));
 {std::scoped_lock l(submissionMutex);
  if(loadingDrain.active()||!emptyLoadingDrain.begin(recordedCommands.empty()&&submittedCommands.empty(),
    !stereoWorkStarted&&!inside_frame_root_double&&!inside_final_pass_double&&rootDone.load()==0&&finalDone.load()==0))
   fail("empty loading boundary cannot retain frame ownership");
 }
 // r159 cleared outstanding here while leaving memoryFrees active. Retain both
 // until the original root has returned and owner Present verifies completion.
 rootDeferred=true;rootDone=0;finalDone=0;
 frame_root_double_enabled=false;final_pass_double_armed=false;
 if(phase()==Phase::Ready)enterPhase(Phase::Warming);
 return false;
}
static PFN_vkFreeDescriptorSets nextFreeDescriptors{};
#include "NativeTransfers.inc"
static VkResult VKAPI_CALL guardedFreeDescriptors(VkDevice device,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet* sets){beforeDestroy("vkFreeDescriptorSets",reinterpret_cast<uintptr_t>(pool));const auto result=nextFreeDescriptors(device,pool,count,sets);if(result==VK_SUCCESS)forgetSourceDescriptors(count,sets);return result;}
PFN_vkVoidFunction wrapRecorded(const char* name,PFN_vkVoidFunction next){
#define REPLAY(api,handler,original) if(!strcmp(name,#api)){original=reinterpret_cast<decltype(original)>(next);return reinterpret_cast<PFN_vkVoidFunction>(&handler);}
 REPLAY(vkCreateShaderModule,accessCreateShader,real_create_shader_module)
 REPLAY(vkDestroyShaderModule,accessDestroyShader,real_destroy_shader_module)
 REPLAY(vkCreateGraphicsPipelines,accessCreateGraphics,real_create_graphics_pipelines)
 REPLAY(vkDestroyPipeline,accessDestroyPipeline,real_destroy_pipeline)
 REPLAY(vkCreateRenderPass,captureRenderPass,real_create_render_pass)
 REPLAY(vkDestroyRenderPass,destroyCapturedRenderPass,real_destroy_render_pass)
 REPLAY(vkCmdCopyImage,nativeCopyImage,real_cmd_copy_image)
 REPLAY(vkCmdBlitImage,nativeBlitImage,real_cmd_blit_image)
 REPLAY(vkCmdPipelineBarrier,sourceBarrier,real_cmd_pipeline_barrier)
 REPLAY(vkCmdDispatch,stageDispatch,real_cmd_dispatch)
 REPLAY(vkCmdDispatchIndirect,stageDispatchIndirect,real_cmd_dispatch_indirect)
 REPLAY(vkFreeDescriptorSets,guardedFreeDescriptors,nextFreeDescriptors)
 REPLAY(vkCmdSetBlendConstants,replayBlend,nextBlend)
 REPLAY(vkCmdSetDepthBounds,replayDepthBounds,nextDepthBounds)
 REPLAY(vkCmdSetLineWidth,replayLineWidth,nextLineWidth)
 REPLAY(vkCmdSetStencilWriteMask,replayStencilWrite,nextStencilWrite)
 REPLAY(vkAllocateCommandBuffers,replayAllocateCommands,real_allocate_command_buffers)
 REPLAY(vkDestroyCommandPool,replayDestroyCommandPool,real_destroy_command_pool)
 REPLAY(vkBeginCommandBuffer,replayBeginCommand,nextBeginCommand)
 REPLAY(vkResetCommandBuffer,replayResetCommand,nextResetCommand)
 REPLAY(vkResetCommandPool,replayResetCommandPool,nextResetCommandPool)
 REPLAY(vkFreeCommandBuffers,replayFreeCommands,nextFreeCommands)
 REPLAY(vkEndCommandBuffer,replayEndCommand,nextEndCommand)
 REPLAY(vkCmdBeginRenderPass,replayBeginPass,real_cmd_begin_render_pass)
 REPLAY(vkCmdEndRenderPass,replayEndPass,real_cmd_end_render_pass)
 REPLAY(vkCmdBindPipeline,replayPipeline,real_cmd_bind_pipeline)
 REPLAY(vkCmdBindDescriptorSets,replayDescriptors,real_cmd_bind_descriptor_sets)
 REPLAY(vkCmdSetViewport,replayViewport,real_cmd_set_viewport)
 REPLAY(vkCmdSetScissor,replayScissor,real_cmd_set_scissor)
 REPLAY(vkCmdBindVertexBuffers,replayVertex,real_cmd_bind_vertex_buffers)
 REPLAY(vkCmdBindIndexBuffer,replayIndex,real_cmd_bind_index_buffer)
 REPLAY(vkCmdPushConstants,replayPush,nextPush)
 REPLAY(vkCmdDraw,replayDraw,real_cmd_draw)
 REPLAY(vkCmdDrawIndexed,replayDrawIndexed,real_cmd_draw_indexed)
 REPLAY(vkCmdDrawIndirect,replayDrawIndirect,real_cmd_draw_indirect)
 REPLAY(vkCmdDrawIndexedIndirect,replayDrawIndexedIndirect,real_cmd_draw_indexed_indirect)
 REPLAY(vkCmdSetDepthBias,replayDepthBias,real_cmd_set_depth_bias)
 REPLAY(vkCmdSetStencilCompareMask,replayStencilCompare,real_cmd_set_stencil_compare_mask)
 REPLAY(vkCmdSetStencilReference,replayStencilRef,real_cmd_set_stencil_reference)
#undef REPLAY
#define GUARDED(api) if(!strcmp(name,#api)){using Guard=UnsupportedFinal<PFN_##api,__COUNTER__>;Guard::next=reinterpret_cast<PFN_##api>(next);return reinterpret_cast<PFN_vkVoidFunction>(&Guard::call);}
 GUARDED(vkCmdNextSubpass) GUARDED(vkCmdClearAttachments) GUARDED(vkCmdExecuteCommands)
 GUARDED(vkCmdUpdateBuffer) GUARDED(vkCmdFillBuffer) GUARDED(vkCmdSetEvent) GUARDED(vkCmdResetEvent)
 GUARDED(vkCmdClearColorImage) GUARDED(vkCmdClearDepthStencilImage)
#undef GUARDED
 return nullptr;
}
}

namespace kharvox::native {
void checkFinalCommand(VkCommandBuffer cb,const char* name){if(leftEyeOnlyDiagnostic())return;std::scoped_lock lock(replayMutex);auto it=replayStates.find(cb);if(it!=replayStates.end()&&it->second.final&&!it->second.replaying)fail(name);}
}
