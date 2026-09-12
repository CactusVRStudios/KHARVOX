// Native-only causal comparison: share a complete world-space shadow input
// (atlas + its frozen addressing table), never either half on its own.
static bool sharedShadowInputs{};
static bool sharedLightingInputs{};
static uint32_t sharedLightingUniformBindings{},sharedLightingStorageBindings{};
static std::map<VkImageView,VkImage> sharedShadowSourceViews;
static uint32_t sharedShadowTableBindings{},sharedShadowAtlasBindings{};
static void beginSharedShadows(){
 static const bool compare=kharvox::runtimeFileExists(L"native_compare_shared_shadows");
 const bool lighting=compare&&kharvox::runtimeFileExists(L"native_test_shared_lighting_inputs");
 const bool enabled=compare&&(lighting||kharvox::runtimeFileExists(L"native_test_shared_shadows"));
 if(lighting!=sharedLightingInputs){sharedLightingInputs=lighting;kharvoxnative::log::warn(std::format("Native complete static lighting comparison frame={} enabled={} includesClusterLists=true separateEyeCameras=true (diagnostic; frustum-edge coverage unvalidated)",renderingPose.serial,lighting));}
 if(enabled!=sharedShadowInputs){sharedShadowInputs=enabled;kharvoxnative::log::warn(std::format("Native shared shadow inputs frame={} enabled={} atlasAndTableTogether=true worldStereoUnchanged=true (diagnostic)",renderingPose.serial,enabled));}
 sharedShadowSourceViews.clear();sharedShadowTableBindings=sharedShadowAtlasBindings=0;
 sharedLightingUniformBindings=sharedLightingStorageBindings=0;
 if(enabled&&kharvox::runtimeFileExists(L"native_test_shadow_history"))fail("shared shadows cannot run with atlas history copyback");
 if(lighting&&(!readOnlyStorageSnapshotTest()||kharvox::runtimeFileExists(L"native_test_private_compute")||kharvox::runtimeFileExists(L"native_test_left_static_inputs")||kharvox::runtimeFileExists(L"native_test_reuse_light_tables")))fail("complete lighting comparison has conflicting input or compute controls");
}
static void observeSharedShadowAtlas(VkImageView view){
 if(!sharedShadowInputs)return;
 std::scoped_lock fl(framebuffer_depth_view_mutex);
 auto vi=image_view_infos.find(view);if(vi==image_view_infos.end())fail("shared shadow seed view missing");
 auto ii=sourceImageDefinitions.find(vi->second.image);if(ii==sourceImageDefinitions.end())fail("shared shadow image metadata missing");
 const auto& info=ii->second;const auto transfers=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
 if(!shadowHistoryShape(info.width,info.height,info.format==VK_FORMAT_X8_D24_UNORM_PACK32,info.mips,info.layers,info.samples==VK_SAMPLE_COUNT_1_BIT,
     (info.usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))==(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
     (info.usage&transfers)==transfers)||info.type!=VK_IMAGE_TYPE_2D||info.depth!=1)return;
 // Freeze the existing source-view aliases at the real/second-eye boundary.
 // Descriptor binding then needs no nested framebuffer lock under its buffer
 // metadata lock. These maps are only used by the verified inline render thread.
 for(const auto&[alias,definition]:image_view_infos)if(definition.image==vi->second.image){
  if(definition.range.aspectMask!=VK_IMAGE_ASPECT_DEPTH_BIT||definition.range.baseMipLevel||definition.range.baseArrayLayer)continue;
  sharedShadowSourceViews.emplace(alias,definition.image);
 }
}
static bool sharedShadowImage(VkImageView view,VkImageLayout layout){
 if(!sharedShadowInputs)return false;
 auto image=sharedShadowSourceViews.find(view);if(image==sharedShadowSourceViews.end())return false;
 // The original atlas is read only. Require its actually tracked layout;
 // never silently bind the wrong layout or alter the left producer's image.
 auto current=sourceLayouts.whole(image->second);
 if(!current||*current!=layout||layout==VK_IMAGE_LAYOUT_UNDEFINED)fail("shared shadow source layout is not the declared sampled layout");
 return true;
}
