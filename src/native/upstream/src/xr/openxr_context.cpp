#include "kharvoxnative/openxr_context.h"
#include "kharvoxnative/wraparound.h"
#include "kharvoxnative/log.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <set>
#include <utility>
#include <vector>

namespace kharvoxnative {
namespace {
OpenXRContext g_xr;
VulkanState g_vk;

// ===================== XR-SIDE HOTKEY RETIREMENT (2026-08-24) ===============
// See docs/AUDIT-2026-08-24-HOTKEY-OWNERSHIP.md for the full per-key table.
//
// The two GetAsyncKeyState sites in this file were the ONLY hotkeys in the
// plugin that are live in EVERY build - everything in vulkan_hooks.cpp sits
// inside `#ifdef DOOMVR_RESEARCH_TOOLS`. Both sampled the consumed `&1` bit
// and IGNORED MODIFIERS, so they fired on SHIFT+F11 and SHIFT+F12 as well as
// on the bare keys, and a modifier cannot un-share a key when the other
// claimant does not check them. Their vulkan_hooks counterparts already work
// around this with private 0x8000 edge detectors, which says the collision was
// known and tolerated rather than fixed.
//
// F12 - AlternateEye. RETIRED. Result recorded in
// docs/HANDOFF-NEXT-SESSION.md: "real depth confirmed but not simultaneous,
// has a compositor-reprojection blur artifact, not the target architecture."
// Same treatment as kLegacyStereoModesEnabled in vulkan_hooks.cpp: the body
// below stays, dead, so the mechanism can be revived - but a revived one must
// claim a key nothing else uses, not share. alternate_eye_enabled_ already
// defaults to false, so this changes nothing about a run in which nobody
// presses F12; it only removes the ability to press it by accident.
constexpr bool kXrAlternateEyeKeyEnabled = false;

// F11 - look injection (head rotation -> SendInput mouse delta). NOT retired:
// this is a feature, not a spent diagnostic, and no doc records a result
// closing it. DEMOTED to the switch-file pattern instead
// (doomvr-visual.txt / doomvr-hooks.txt / doomvr-swapeyes.txt), because F11 is
// the key rdc_configure_if_present deliberately rebinds RenderDoc's capture to
// - so every capture press was silently flipping head-look as well.
//
// Read ONCE into a function-local static, not per frame: this is called from
// update_look_injection, which runs on every located view. The old doc line
// "F11 | MonoTracked head-look | On by default" is STALE -
// look_injection_enabled_ has defaulted to false; absent file = OFF preserves
// exactly that.
bool look_injection_switch_file_present() {
    static const bool present = [] {
        FILE* f = nullptr;
        if (fopen_s(&f, "C:\\dev\\doomvr-lookinject.txt", "r") == 0 && f) {
            std::fclose(f);
            return true;
        }
        return false;
    }();
    return present;
}

void log_pose(const XrView& v, int eye) {
    char b[256];
    std::snprintf(b, sizeof(b), "eye=%d pos=(%.3f %.3f %.3f) ori=(%.3f %.3f %.3f %.3f)", eye,
        v.pose.position.x, v.pose.position.y, v.pose.position.z,
        v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w);
    log::info(b);
}

bool log_xr_failure(const char* operation, XrResult result) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed: XrResult=%d", operation,
        static_cast<int>(result));
    log::error(text);
    return false;
}

template <typename T>
T load_device_proc(PFN_vkGetDeviceProcAddr get_proc, VkDevice device, const char* name) {
    return reinterpret_cast<T>(get_proc(device, name));
}
}

const char* OpenXRContext::session_state_name(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_UNKNOWN:      return "UNKNOWN";
        case XR_SESSION_STATE_IDLE:         return "IDLE";
        case XR_SESSION_STATE_READY:        return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
        case XR_SESSION_STATE_STOPPING:     return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING:      return "EXITING";
        default:                            return "UNRECOGNISED";
    }
}

OpenXRContext& xr() { return g_xr; }
VulkanState& vulkan_state() { return g_vk; }

bool OpenXRContext::initialize(const VulkanState& vk) {
    if (session_ != XR_NULL_HANDLE) return true;
    if (!vk.instance || !vk.physical_device || !vk.device || !vk.queue || vk.queue_family == UINT32_MAX) {
        log::warn("OpenXR initialization deferred: Vulkan state is incomplete");
        return false;
    }

    shutdown();

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ici.applicationInfo.applicationName, "DOOM VR Mod", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.applicationVersion = 1;
    std::strncpy(ici.applicationInfo.engineName, "id Tech 6 / doomvr", XR_MAX_ENGINE_NAME_SIZE - 1);
    ici.applicationInfo.engineVersion = 1;
    // XR_CURRENT_API_VERSION follows the fetched SDK (currently OpenXR 1.1),
    // but several installed PC runtimes still expose only OpenXR 1.0. This
    // prototype uses no 1.1-only functionality, so request the compatible
    // baseline explicitly.
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    const char* exts[] = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    XrResult result = xrCreateInstance(&ici, &instance_);
    if (XR_FAILED(result)) return log_xr_failure("xrCreateInstance", result);

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystem(instance_, &sgi, &system_);
    if (XR_FAILED(result)) { log_xr_failure("xrGetSystem", result); shutdown(); return false; }

    PFN_xrGetVulkanGraphicsRequirementsKHR get_vulkan_requirements = nullptr;
    result = xrGetInstanceProcAddr(
        instance_,
        "xrGetVulkanGraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&get_vulkan_requirements));
    if (XR_FAILED(result) || !get_vulkan_requirements) {
        log_xr_failure("xrGetInstanceProcAddr(xrGetVulkanGraphicsRequirementsKHR)", result);
        shutdown();
        return false;
    }

    XrGraphicsRequirementsVulkanKHR requirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    result = get_vulkan_requirements(instance_, system_, &requirements);
    if (XR_FAILED(result)) {
        log_xr_failure("xrGetVulkanGraphicsRequirementsKHR", result);
        shutdown();
        return false;
    }

    char requirements_text[192];
    std::snprintf(requirements_text, sizeof(requirements_text),
        "OpenXR Vulkan requirements: min=%u.%u.%u max=%u.%u.%u",
        XR_VERSION_MAJOR(requirements.minApiVersionSupported),
        XR_VERSION_MINOR(requirements.minApiVersionSupported),
        XR_VERSION_PATCH(requirements.minApiVersionSupported),
        XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
        XR_VERSION_MINOR(requirements.maxApiVersionSupported),
        XR_VERSION_PATCH(requirements.maxApiVersionSupported));
    log::info(requirements_text);

    XrGraphicsBindingVulkanKHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    gb.instance = vk.instance;
    gb.physicalDevice = vk.physical_device;
    gb.device = vk.device;
    gb.queueFamilyIndex = vk.queue_family;
    gb.queueIndex = vk.queue_index;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = system_;
    result = xrCreateSession(instance_, &sci, &session_);
    if (XR_FAILED(result)) { log_xr_failure("xrCreateSession", result); shutdown(); return false; }

    vk_device_ = vk.device;
    vk_queue_ = vk.queue;
    HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
    get_device_proc_addr_ = vulkan ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        GetProcAddress(vulkan, "vkGetDeviceProcAddr")) : nullptr;
    if (!get_device_proc_addr_) {
        log::error("vkGetDeviceProcAddr is unavailable"); shutdown(); return false;
    }

    auto create_command_pool = load_device_proc<PFN_vkCreateCommandPool>(
        get_device_proc_addr_, vk_device_, "vkCreateCommandPool");
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = vk.queue_family;
    VkResult vk_result = create_command_pool
        ? create_command_pool(vk_device_, &pool_info, nullptr, &command_pool_)
        : VK_ERROR_INITIALIZATION_FAILED;
    if (vk_result != VK_SUCCESS) {
        log::error("vkCreateCommandPool failed for OpenXR test renderer");
        shutdown(); return false;
    }

    if (!create_swapchains(vk)) { shutdown(); return false; }

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    result = xrCreateReferenceSpace(session_, &rs, &local_space_);
    if (XR_FAILED(result)) { log_xr_failure("xrCreateReferenceSpace", result); shutdown(); return false; }

    log::info("OpenXR initialized on DOOM Vulkan device");
    return true;
}

bool OpenXRContext::create_swapchains(const VulkanState&) {
    uint32_t view_count = 0;
    XrResult result = xrEnumerateViewConfigurationViews(instance_, system_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    if (XR_FAILED(result) || view_count < 2) return log_xr_failure(
        "xrEnumerateViewConfigurationViews(count)", result);

    std::vector<XrViewConfigurationView> configs(
        view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result = xrEnumerateViewConfigurationViews(instance_, system_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, configs.data());
    if (XR_FAILED(result)) return log_xr_failure(
        "xrEnumerateViewConfigurationViews", result);

    uint32_t format_count = 0;
    result = xrEnumerateSwapchainFormats(session_, 0, &format_count, nullptr);
    if (XR_FAILED(result) || format_count == 0)
        return log_xr_failure("xrEnumerateSwapchainFormats(count)", result);
    std::vector<int64_t> formats(format_count);
    result = xrEnumerateSwapchainFormats(session_, format_count, &format_count, formats.data());
    if (XR_FAILED(result)) return log_xr_failure("xrEnumerateSwapchainFormats", result);

    int64_t chosen = formats.front();
    // COLOUR SPACE (2026-08-16). This list used to prefer _SRGB, and that is
    // wrong for what we actually submit.
    //
    // What reaches these swapchains is DOOM's own presented image, captured
    // from its swapchain, whose format is VK_FORMAT_B8G8R8A8_UNORM (44,
    // confirmed live: "captured DOOM swapchain format=44"). Those bytes are
    // ALREADY sRGB-encoded - they are the exact bytes that go to the monitor
    // and look correct there.
    //
    // vkCmdBlitImage performs a linear->sRGB ENCODE when the destination
    // format is _SRGB, and performs no decode when the source is UNORM. So
    // blitting DOOM's already-encoded pixels into an _SRGB swapchain encodes
    // them a SECOND time. Double encoding lifts blacks and midtones and
    // flattens contrast - a washed-out image in which mid-distance texture
    // detail disappears, which is exactly what the headset has been showing
    // relative to the monitor.
    //
    // The target is a byte-for-byte delivery of what the monitor gets, so we
    // want NO conversion on either side of the blit: a UNORM destination.
    // CORRECTED (2026-08-16, second pass). Preferring UNORM was measured live
    // and was still wrong: chosen=44 is byte-identical to DOOM's swapchain,
    // yet the headset came back "much too bright". Both configurations are
    // wrong by exactly ONE gamma encode, because the earlier reasoning
    // accounted for what our blit does and ignored what the RUNTIME does on
    // read:
    //
    //   _SRGB : we store encode(v); runtime decodes to v; displays encode(v)
    //   UNORM : we store v;         runtime assumes linear;  displays encode(v)
    //
    // where v is DOOM's already-sRGB-encoded byte. The correct configuration
    // is an _SRGB swapchain holding the RAW bytes v: the runtime's decode and
    // its display encode then round-trip to exactly v. Getting raw bytes into
    // an _SRGB image needs vkCmdCopyImage (no conversion, size-compatible
    // formats allowed), not a blit - see the sRGB-typed staging image below.
    constexpr int64_t preferred[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                                     VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    for (int64_t candidate : preferred) {
        if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) {
            chosen = candidate; break;
        }
    }
    swapchain_format_ = static_cast<VkFormat>(chosen);
    // Never assume this - the chosen format was previously not logged at all,
    // so "the XR swapchain is sRGB" was an inference nobody could check.
    {
        std::string offered;
        for (int64_t f : formats) offered += std::format(" {}", f);
        const bool is_srgb = chosen == VK_FORMAT_R8G8B8A8_SRGB ||
                             chosen == VK_FORMAT_B8G8R8A8_SRGB;
        log::warn(std::format(
            "XR swapchain format chosen={} sRGB={} (_SRGB preferred so the runtime's decode "
            "and its display encode round-trip to identity; raw bytes are delivered via the "
            "sRGB-typed staging copy). Runtime offered:{}",
            chosen, is_srgb, offered));
        if (!is_srgb) {
            log::error(
                "XR swapchain fell back to a UNORM format - the runtime offered no _SRGB "
                "option. The runtime will treat DOOM's already-encoded bytes as linear and "
                "encode them again, so the image will look too bright. Real fallback, not a "
                "no-op.");
        }
    }

    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto& target = eyes_[eye];
        target.width = static_cast<int32_t>(configs[eye].recommendedImageRectWidth);
        target.height = static_cast<int32_t>(configs[eye].recommendedImageRectHeight);
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format = chosen;
        info.sampleCount = 1;
        info.width = target.width;
        info.height = target.height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        result = xrCreateSwapchain(session_, &info, &target.handle);
        if (XR_FAILED(result)) return log_xr_failure("xrCreateSwapchain", result);

        uint32_t image_count = 0;
        result = xrEnumerateSwapchainImages(target.handle, 0, &image_count, nullptr);
        if (XR_FAILED(result)) return log_xr_failure("xrEnumerateSwapchainImages(count)", result);
        target.images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        result = xrEnumerateSwapchainImages(target.handle, image_count, &image_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(target.images.data()));
        if (XR_FAILED(result)) return log_xr_failure("xrEnumerateSwapchainImages", result);
        target.initialized.assign(image_count, false);
    }
    log::info("OpenXR stereo test swapchains created");
    return true;
}

bool OpenXRContext::render_eye(uint32_t eye, const XrView& view,
    VkImage source_image, VkExtent2D source_extent, VkFormat source_format,
    XrCompositionLayerProjectionView& projection_view,
    VkImageLayout source_layout, VkFilter source_filter, bool wraparound) {
    auto& target = eyes_[eye];
    // Filled in by the blit below when wraparound is on; the SAME object then
    // supplies the declared fov at the bottom of this function. There is no
    // path that blits one rectangle and declares a different one, because the
    // declaration reads this struct and nothing else.
    wrap::Framing wrap_framing{};
    bool wrap_framing_ok = false;
    uint32_t image_index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(target.handle, &acquire, &image_index);
    if (XR_FAILED(result)) return log_xr_failure("xrAcquireSwapchainImage", result);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(target.handle, &wait);
    if (XR_FAILED(result)) return log_xr_failure("xrWaitSwapchainImage", result);

    auto allocate = load_device_proc<PFN_vkAllocateCommandBuffers>(get_device_proc_addr_, vk_device_, "vkAllocateCommandBuffers");
    auto begin = load_device_proc<PFN_vkBeginCommandBuffer>(get_device_proc_addr_, vk_device_, "vkBeginCommandBuffer");
    auto barrier = load_device_proc<PFN_vkCmdPipelineBarrier>(get_device_proc_addr_, vk_device_, "vkCmdPipelineBarrier");
    auto clear = load_device_proc<PFN_vkCmdClearColorImage>(get_device_proc_addr_, vk_device_, "vkCmdClearColorImage");
    auto blit = load_device_proc<PFN_vkCmdBlitImage>(get_device_proc_addr_, vk_device_, "vkCmdBlitImage");
    auto end = load_device_proc<PFN_vkEndCommandBuffer>(get_device_proc_addr_, vk_device_, "vkEndCommandBuffer");
    auto submit = load_device_proc<PFN_vkQueueSubmit>(get_device_proc_addr_, vk_device_, "vkQueueSubmit");
    auto wait_idle = load_device_proc<PFN_vkQueueWaitIdle>(get_device_proc_addr_, vk_device_, "vkQueueWaitIdle");
    auto free_buffers = load_device_proc<PFN_vkFreeCommandBuffers>(get_device_proc_addr_, vk_device_, "vkFreeCommandBuffers");
    if (!allocate || !begin || !barrier || !clear || !blit || !end || !submit || !wait_idle || !free_buffers) {
        log::error("OpenXR test renderer could not load Vulkan commands"); return false;
    }

    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = command_pool_; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (allocate(vk_device_, &alloc, &command) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin(command, &begin_info);

    const bool copy_doom = source_image != VK_NULL_HANDLE &&
        source_extent.width && source_extent.height &&
        source_format != VK_FORMAT_UNDEFINED;
    const bool side_by_side = copy_doom &&
        static_cast<float>(source_extent.width) / static_cast<float>(source_extent.height) > 3.0f;
    VkImageMemoryBarrier source_to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    if (copy_doom) {
        source_to_copy.oldLayout = source_layout;
        source_to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        source_to_copy.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        source_to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        source_to_copy.image = source_image;
        source_to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &source_to_copy);
    }

    VkImageMemoryBarrier to_clear{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_clear.oldLayout = target.initialized[image_index] ?
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    to_clear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_clear.srcAccessMask = target.initialized[image_index] ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
    to_clear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_clear.image = target.images[image_index].image;
    to_clear.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier(command, target.initialized[image_index] ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_clear);
    if (copy_doom) {
        // sRGB-TYPED STAGING - docs/TASK-QUEUE-2026-08-16.md §A0.
        //
        // We must land DOOM's RAW bytes in the _SRGB swapchain. A direct
        // source->_SRGB blit encodes on write and is what made the image one
        // gamma encode too bright. vkCmdCopyImage performs NO conversion and
        // permits size-compatible formats, so copying into an image that is
        // sRGB-TYPED but byte-identical gives us the raw bytes under an sRGB
        // interpretation. The subsequent _SRGB->_SRGB blit decodes on read and
        // encodes on write - gamma identity - while still scaling to the eye.
        static VkImage staging_image = VK_NULL_HANDLE;
        static VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        static VkExtent2D staging_extent{};
        static bool staging_failed = false;
        static bool staging_state_logged = false;

        auto srgb_sibling = [](VkFormat f) -> VkFormat {
            switch (f) {
                case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
                case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
                default: return VK_FORMAT_UNDEFINED;
            }
        };
        const VkFormat staging_format = srgb_sibling(source_format);
        const bool dst_is_srgb = swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB ||
                                 swapchain_format_ == VK_FORMAT_R8G8B8A8_SRGB;
        bool use_staging = staging_format != VK_FORMAT_UNDEFINED && dst_is_srgb &&
                           !staging_failed;

        if (use_staging &&
            (staging_image == VK_NULL_HANDLE ||
             staging_extent.width != source_extent.width ||
             staging_extent.height != source_extent.height)) {
            const auto& vk = vulkan_state();
            auto create_image = load_device_proc<PFN_vkCreateImage>(get_device_proc_addr_, vk_device_, "vkCreateImage");
            auto get_reqs = load_device_proc<PFN_vkGetImageMemoryRequirements>(get_device_proc_addr_, vk_device_, "vkGetImageMemoryRequirements");
            auto allocate_memory = load_device_proc<PFN_vkAllocateMemory>(get_device_proc_addr_, vk_device_, "vkAllocateMemory");
            auto bind_memory = load_device_proc<PFN_vkBindImageMemory>(get_device_proc_addr_, vk_device_, "vkBindImageMemory");
            HMODULE vkmod = GetModuleHandleW(L"vulkan-1.dll");
            auto get_mem_props = vkmod ? reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
                GetProcAddress(vkmod, "vkGetPhysicalDeviceMemoryProperties")) : nullptr;
            bool ok = create_image && get_reqs && allocate_memory && bind_memory &&
                      get_mem_props && vk.physical_device;
            if (ok) {
                VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                ici.imageType = VK_IMAGE_TYPE_2D;
                ici.format = staging_format;
                ici.extent = {source_extent.width, source_extent.height, 1};
                ici.mipLevels = 1; ici.arrayLayers = 1;
                ici.samples = VK_SAMPLE_COUNT_1_BIT;
                ici.tiling = VK_IMAGE_TILING_OPTIMAL;
                ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                ok = create_image(vk_device_, &ici, nullptr, &staging_image) == VK_SUCCESS;
            }
            VkMemoryRequirements reqs{};
            if (ok) {
                get_reqs(vk_device_, staging_image, &reqs);
                VkPhysicalDeviceMemoryProperties mp{};
                get_mem_props(vk.physical_device, &mp);
                uint32_t type_index = UINT32_MAX;
                for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
                    if ((reqs.memoryTypeBits & (1u << i)) &&
                        (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                        type_index = i; break;
                    }
                }
                ok = type_index != UINT32_MAX;
                if (ok) {
                    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                    mai.allocationSize = reqs.size;
                    mai.memoryTypeIndex = type_index;
                    ok = allocate_memory(vk_device_, &mai, nullptr, &staging_memory) == VK_SUCCESS &&
                         bind_memory(vk_device_, staging_image, staging_memory, 0) == VK_SUCCESS;
                }
            }
            if (ok) {
                staging_extent = source_extent;
                log::warn(std::format(
                    "sRGB staging image ready: {}x{} format={} (raw copy of source format={}, "
                    "then gamma-identity blit into the _SRGB swapchain)",
                    source_extent.width, source_extent.height,
                    static_cast<int>(staging_format), static_cast<int>(source_format)));
            } else {
                staging_failed = true;
                staging_image = VK_NULL_HANDLE;
                log::error("sRGB staging image creation FAILED - falling back to the direct "
                           "blit, which encodes gamma a second time and will look too bright. "
                           "Real fallback, not a no-op.");
            }
            use_staging = !staging_failed;
        }

        // ⚠ PER-EYE AND PER-FORMAT, not once per process (2026-08-17).
        //
        // This was a single static bool, and eye 0 consumed it on the first
        // frame. Eye 0's source is DOOM's swapchain (format 44), which HAS an
        // sRGB sibling, so it took the staging path and logged nothing - and
        // every later call for EYE 1, whose source is the format-122 mirror
        // with no sRGB sibling, fell back to the direct blit in total
        // silence. The owner saw the result in the headset ("very bright,
        // doesn't match the monitor") before the log ever mentioned it.
        //
        // That is Rule 3 broken by a one-shot flag: the fix existed, eye 1
        // was simply not on it, and nothing said so. Keyed on (eye,
        // sourceFormat) now, so each distinct path announces itself once.
        static std::set<std::pair<uint32_t, int>> staging_state_logged_for;
        (void)staging_state_logged;
        if (staging_state_logged_for.insert({eye, static_cast<int>(source_format)}).second) {
            if (!use_staging) {
                log::error(std::format(
                    "sRGB staging NOT in use for eye {}: sourceFormat={} srgbSibling={} "
                    "dstIsSrgb={}. Falling back to the DIRECT BLIT. For an already-encoded 8-bit "
                    "source that is one gamma encode too many; for a LINEAR float source "
                    "(B10G11R11, format 122) the blit's linear->sRGB encode is what is wanted, so "
                    "which of those this is depends on whether the source image holds linear or "
                    "display-encoded values - see the SwapchainReference dump. Real fallback, "
                    "not a no-op.",
                    eye, static_cast<int>(source_format), static_cast<int>(staging_format),
                    dst_is_srgb));
            } else {
                log::warn(std::format(
                    "sRGB staging IN USE for eye {}: sourceFormat={} -> staging={} -> swapchain={}",
                    eye, static_cast<int>(source_format), static_cast<int>(staging_format),
                    static_cast<int>(swapchain_format_)));
            }
        }

        VkImage blit_source = source_image;
        if (use_staging && staging_image != VK_NULL_HANDLE) {
            auto copy_image = load_device_proc<PFN_vkCmdCopyImage>(get_device_proc_addr_, vk_device_, "vkCmdCopyImage");
            if (copy_image) {
                VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                to_dst.image = staging_image;
                to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                barrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &to_dst);

                VkImageCopy copy_region{};
                copy_region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy_region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy_region.extent = {source_extent.width, source_extent.height, 1};
                copy_image(command, source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    staging_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

                VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                to_src.image = staging_image;
                to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &to_src);

                blit_source = staging_image;
            }
        }

        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        const int32_t source_width = static_cast<int32_t>(source_extent.width);
        const int32_t eye_width = side_by_side ? source_width / 2 : source_width;
        region.srcOffsets[0] = {side_by_side && eye == 1 ? eye_width : 0, 0, 0};
        region.srcOffsets[1] = {side_by_side && eye == 0 ? eye_width : source_width,
                                static_cast<int32_t>(source_extent.height), 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffsets[1] = {target.width, target.height, 1};
        // ===== PHASE 1: ASPECT-PRESERVING CROP, NOT A STRETCH ==============
        //
        // The full-rect blit above scales a ~1.879:1 source onto a roughly
        // square eye. That is a large horizontal squeeze, and the fov declared
        // beside it described the UNCROPPED image - declaration and content
        // disagreed by exactly that factor, which is the "zoomed in and
        // blurry" class. Cropping to the eye's aspect and deriving the angles
        // from the crop makes the two agree at any source size, any eye size
        // and any FOV-slider setting.
        //
        // The crop is applied INSIDE this eye's own source rect, so the
        // side-by-side split above still holds and its base x is added back.
        if (wraparound) {
            const int32_t eye_rect_x = side_by_side && eye == 1 ? eye_width : 0;
            const int32_t eye_rect_w = side_by_side ? eye_width : source_width;
            const int32_t eye_rect_h = static_cast<int32_t>(source_extent.height);
            // The crop target comes from view.fov - which, on this path, poll()
            // seeded with THIS eye's located XrFovf. So "how much of DOOM's
            // frame do we keep" is answered by what the headset can actually
            // show, not by a constant. crop=pixel in the switch file selects
            // the buffer-aspect crop instead; see wraparound.h's Config.
            const bool crop_to_fov = wrap::config().crop_to_fov;
            float target_fov_x = crop_to_fov
                ? (view.fov.angleRight - view.fov.angleLeft) : 0.0f;
            float target_fov_y = crop_to_fov
                ? (view.fov.angleUp - view.fov.angleDown) : 0.0f;
            // ===== FULL FRAME UNDER THE CONTENT ANCHOR (2026-08-27) ========
            //
            // The 20:47 run's verdict "a rectangular screen" was this crop:
            // with anchor=content the panel is world-fixed, and cropping the
            // 132x101 deg content down to the headset's 110x90 made the panel
            // EXACTLY as big as the view - every head turn instantly showed
            // its border. The look-around slack the anchor exists to provide
            // was being cut off before submission.
            //
            // Under the anchor, the crop target is the CONTENT's own angles,
            // which makes the crop the identity and the declared FOV the full
            // 132.4 x 100.7 - the compositor gets everything DOOM rendered,
            // and the head has ~+/-11 deg yaw / ~+/-5 deg pitch of world to
            // look across before the edge.
            // OVERSCAN (2026-08-28): full-frame submission is now the normal
            // head-tracked path too, not just the (parked) anchor mode. The
            // declared pose is unchanged - only how much of the already-
            // rendered frame the compositor is given. With the exact-FOV crop,
            // timewarp flashed black past the trailing edge on every head
            // motion, which reads as "a projected screen"; the 132x101 margin
            // makes timewarp invisible.
            if (wrap::anchor_content_active() || wrap::config().overscan) {
                const float s = wrap::frame_scale(nullptr);
                if (s > 0.05f && std::isfinite(s)) {
                    const float content_fov_y = 2.0f * std::atan(1.0f / s);
                    const float src_aspect = eye_rect_h > 0
                        ? static_cast<float>(eye_rect_w) / static_cast<float>(eye_rect_h)
                        : 16.0f / 9.0f;
                    target_fov_y = content_fov_y;
                    target_fov_x =
                        2.0f * std::atan(std::tan(content_fov_y * 0.5f) * src_aspect);
                }
            }
            wrap_framing = wrap::compute_framing(eye_rect_w, eye_rect_h,
                                                 target.width, target.height,
                                                 wrap::frame_scale(nullptr),
                                                 target_fov_x, target_fov_y);
            if (wrap_framing.valid) {
                region.srcOffsets[0] = {eye_rect_x + wrap_framing.crop_x,
                                        wrap_framing.crop_y, 0};
                region.srcOffsets[1] = {eye_rect_x + wrap_framing.crop_x + wrap_framing.crop_w,
                                        wrap_framing.crop_y + wrap_framing.crop_h, 1};
                // 1:1 WHERE IT FITS. The destination rect comes out of the
                // same Framing as the crop, so the blit, the declared angles
                // and the submitted subImage.imageRect below cannot disagree.
                region.dstOffsets[1] = {wrap_framing.dst_w, wrap_framing.dst_h, 1};
                wrap_framing_ok = true;
                wrap::publish_framing(eye, wrap_framing);
            } else {
                // REAL FALLBACK, NOT A NO-OP: the full-rect stretch above
                // stands and the declared fov falls back to view.fov, so this
                // eye is back to the old mismatched pair. Said out loud once
                // per eye so it cannot be mistaken for the wraparound path
                // working.
                static std::set<uint32_t> reported;
                if (reported.insert(eye).second) {
                    log::error(std::format(
                        "Wraparound framing INVALID for eye {}: src={}x{} dst={}x{} scale={:.4f}. "
                        "Falling back to the full-rect stretch and view.fov, which is the old "
                        "declaration/content mismatch. Real fallback, not a no-op.",
                        eye, eye_rect_w, eye_rect_h, target.width, target.height,
                        wrap::frame_scale(nullptr)));
                }
            }
        }
        blit(command, blit_source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            target.images[image_index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &region, source_filter);
    } else {
        const VkClearColorValue color = eye == 0
            ? VkClearColorValue{{0.08f, 0.18f, 0.85f, 1.0f}}
            : VkClearColorValue{{0.85f, 0.18f, 0.08f, 1.0f}};
        clear(command, target.images[image_index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &color, 1, &to_clear.subresourceRange);
    }
    VkImageMemoryBarrier to_runtime = to_clear;
    to_runtime.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_runtime.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_runtime.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_runtime.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_runtime);
    if (copy_doom) {
        VkImageMemoryBarrier source_to_present = source_to_copy;
        source_to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        source_to_present.newLayout = source_layout;
        source_to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        source_to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &source_to_present);
    }
    end(command);
    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1; submit_info.pCommandBuffers = &command;
    const bool submitted = submit(vk_queue_, 1, &submit_info, VK_NULL_HANDLE) == VK_SUCCESS;
    if (submitted) wait_idle(vk_queue_);
    free_buffers(vk_device_, command_pool_, 1, &command);
    target.initialized[image_index] = submitted;

    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    result = xrReleaseSwapchainImage(target.handle, &release);
    if (!submitted || XR_FAILED(result)) return false;
    projection_view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    projection_view.pose = view.pose; projection_view.fov = view.fov;
    // TIED BY CONSTRUCTION. The declared fov is read out of the same Framing
    // object the blit's srcOffsets were built from, a few dozen lines above.
    // Nothing else can set it on this path, so "declared == content" is not a
    // property that has to be maintained - it is the only thing expressible.
    if (wraparound && wrap_framing_ok) {
        projection_view.fov.angleLeft = wrap_framing.angle_left;
        projection_view.fov.angleRight = wrap_framing.angle_right;
        projection_view.fov.angleUp = wrap_framing.angle_up;
        projection_view.fov.angleDown = wrap_framing.angle_down;
    }
    if (wraparound) {
        // Published from the SUBMITTED struct, not from wrap_framing, so the
        // engagement line and the self-test compare the compositor's copy
        // against the blit rectangle rather than a value against itself.
        wrap::note_submitted_fov(eye, projection_view.fov.angleLeft,
                                 projection_view.fov.angleRight,
                                 projection_view.fov.angleUp,
                                 projection_view.fov.angleDown);
    }
    projection_view.subImage.swapchain = target.handle;
    projection_view.subImage.imageRect.offset = {0, 0};
    // MUST match the rectangle the blit actually wrote. Declaring the whole
    // swapchain while writing a sub-rectangle would have the runtime sample
    // pixels we never touched - the same class of error as declaring a field
    // of view the pixels do not subtend, and it comes from the same struct so
    // that it cannot drift.
    projection_view.subImage.imageRect.extent =
        (wraparound && wrap_framing_ok && wrap_framing.dst_w > 0 && wrap_framing.dst_h > 0)
            ? XrExtent2Di{wrap_framing.dst_w, wrap_framing.dst_h}
            : XrExtent2Di{target.width, target.height};
    projection_view.subImage.imageArrayIndex = 0;
    return true;
}

void OpenXRContext::update_look_injection(float qx, float qy, float qz, float qw, float extra_yaw_bias) {
    // F11 RELEASED (2026-08-24). The key toggle is gone; the switch file
    // decides, once, on the first call. See look_injection_switch_file_present
    // above. The `look_have_last_ = false` that used to sit here existed only
    // to stop a camera jump when re-enabling mid-session after drift; with no
    // mid-session toggle there is nothing to re-enable, and the constructor
    // plus shutdown() already clear it.
    {
        static bool applied = false;
        if (!applied) {
            applied = true;
            const bool on = look_injection_switch_file_present();
            look_injection_enabled_.store(on, std::memory_order_relaxed);
            log::warn(on
                ? "Look injection ENABLED by C:\\dev\\doomvr-lookinject.txt (F11 released; the "
                  "key no longer toggles this). Delete the file to turn it off."
                : "Look injection OFF (default). F11 is RELEASED - it no longer toggles this, so "
                  "a RenderDoc capture press cannot flip head-look any more. Create "
                  "C:\\dev\\doomvr-lookinject.txt to turn it on for the next launch.");
        }
    }

    // Forward = local -Z rotated into world space by the head orientation.
    const float fx = -2.0f * (qx * qz + qy * qw);
    const float fy = 2.0f * (qx * qw - qy * qz);
    const float fz = 2.0f * (qx * qx + qy * qy) - 1.0f;

    const float yaw = std::atan2(fx, -fz);
    const float pitch = std::asin(std::clamp(fy, -1.0f, 1.0f));
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) return;  // degenerate pose; skip this frame

    if (!look_have_last_) {
        look_last_yaw_ = yaw;
        look_last_pitch_ = pitch;
        look_have_last_ = true;
        return;
    }

    constexpr float kPi = 3.14159265358979323846f;
    float delta_yaw = yaw - look_last_yaw_;
    // Wrap to [-pi, pi] so crossing the +-pi seam doesn't inject a huge spike.
    while (delta_yaw > kPi) delta_yaw -= 2.0f * kPi;
    while (delta_yaw < -kPi) delta_yaw += 2.0f * kPi;
    delta_yaw += extra_yaw_bias;
    const float delta_pitch = pitch - look_last_pitch_;

    look_last_yaw_ = yaw;
    look_last_pitch_ = pitch;

    // ===== BODYFOLLOW (2026-08-29, thread A) ==============================
    // When the transfer is armed it OWNS the injector: the legacy path below
    // injects the whole per-frame head delta, which is precisely the
    // double-apply that made headlook and lookinject mutually exclusive. The
    // two must never both run in a frame.
    if (wrap::bodyfollow_mode() != wrap::BodyFollow::Off) {
        update_body_transfer(yaw, pitch);
        return;
    }
    wrap::note_transfer_gated(0);

    if (!look_injection_enabled_.load(std::memory_order_relaxed)) return;

    // Calibration constant against observed in-game turn rate. Sign
    // confirmed correct; magnitude tuned by feel across a few live tests
    // (1200 -> 2400 -> 3000). This is a stopgap for the MonoTracked
    // milestone, not a final value - not worth iterating further here.
    constexpr float kPixelsPerRadian = 3000.0f;
    // Defensive cap: a single XR pose sample should never need to inject more
    // than a quarter-turn of mouse movement in one frame. If it ever does
    // (runtime hiccup, pose teleport), clamp rather than fire an enormous
    // SendInput delta into the game.
    constexpr float kMaxDeltaPerFrame = kPixelsPerRadian * (kPi / 2.0f);
    look_pending_dx_ += std::clamp(delta_yaw * kPixelsPerRadian, -kMaxDeltaPerFrame, kMaxDeltaPerFrame);
    look_pending_dy_ += std::clamp(-delta_pitch * kPixelsPerRadian, -kMaxDeltaPerFrame, kMaxDeltaPerFrame);
    if (!std::isfinite(look_pending_dx_) || !std::isfinite(look_pending_dy_)) {
        look_pending_dx_ = 0.0f;
        look_pending_dy_ = 0.0f;
        return;
    }

    const auto dx = static_cast<int>(look_pending_dx_);
    const auto dy = static_cast<int>(look_pending_dy_);
    if (dx == 0 && dy == 0) return;
    look_pending_dx_ -= static_cast<float>(dx);
    look_pending_dy_ -= static_cast<float>(dy);

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    static std::atomic_uint32_t send_input_calls{0};
    static std::atomic_uint32_t send_input_failures{0};
    send_input_calls.fetch_add(1, std::memory_order_relaxed);
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        const DWORD error = GetLastError();
        if (send_input_failures.fetch_add(1, std::memory_order_relaxed) < 20) {
            log::error(std::format("SendInput failed dx={} dy={} error={} totalCalls={}",
                                    dx, dy, error, send_input_calls.load(std::memory_order_relaxed)));
        }
    }
}

// ===========================================================================
// THE DECOUPLED HEAD/BODY TRANSFER (bodyfollow=on)
// ===========================================================================
//
// Takes the ABSOLUTE head yaw/pitch, in the same convention the seam uses:
// forward = local -Z rotated by the head quaternion, yaw = atan2(fx, -fz),
// pitch = asin(fy). That is not an assumption - wrap::yaw_pitch_from_quat and
// the derivation at the top of update_look_injection are term-for-term the
// same three expressions, checked 2026-08-29. If either ever changes, both
// must change together or the residual the seam applies and the residual the
// transfer measures will be two different numbers.
//
// STRUCTURE, and why it cannot oscillate: see wraparound_math.h. Nothing here
// reads back a measured yaw. The reference advances by exactly the pixels
// sent, converted at the configured pixels-per-radian.
void OpenXRContext::update_body_transfer(float head_yaw, float head_pitch) {
    // dt from our own clock. Clamped hard: a hitch or a breakpoint must not
    // become a lurch, and the cap bounds one frame's transfer to rate*0.1 s.
    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (bf_have_last_time_)
        dt = std::chrono::duration<float>(now - bf_last_time_).count();
    bf_last_time_ = now;
    bf_have_last_time_ = true;
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    if (dt > 0.1f) dt = 0.1f;

    if (!std::isfinite(head_yaw) || !std::isfinite(head_pitch)) {
        wrap::note_transfer_gated(3);
        return;
    }
    // The lookinject file is the master arm for every synthetic mouse delta
    // this process sends. Read AT the injection, never from an assumption in a
    // run instruction - a previous run declared a head-tracked pose over a
    // camera that never moved for exactly this reason.
    if (!look_injection_enabled_.load(std::memory_order_relaxed)) {
        wrap::note_transfer_gated(1);
        return;
    }
    // Calibration owns the injector in its own mode, and it is driven from the
    // present hook, not from here. Counted rather than silent so a run cannot
    // report "no injections" without saying which half held the gate.
    if (wrap::bodyfollow_mode() == wrap::BodyFollow::Calibrate) {
        wrap::note_transfer_gated(4);
        return;
    }
    // ⚠ GAMEPLAY ONLY. A synthetic mouse delta at a menu or a loading screen
    // navigates the menu. This is the project's own gameplay predicate - the
    // translation-magnitude test on the live world VP, the same one the
    // retired slew used. (The plan named GameIsRenderingAWorld(); no such
    // symbol exists in this repo - it belongs to the other project. Verified
    // 2026-08-29.) menu_is_open() is deliberately NOT consulted: it is
    // log-only, and it once latched and disabled a whole feature for a run.
    {
        wrap::VpSnapshot vp;
        if (!wrap::world_vp(vp) || !wrap::is_gameplay_view(vp.m)) {
            wrap::note_transfer_gated(2);
            return;
        }
    }

    wrap::BodyTransferState st;
    st.ref_yaw = wrap::recenter_ref_yaw();
    st.ref_pitch = wrap::recenter_ref_pitch();
    constexpr float kDegToRad = 0.01745329252f;
    const float dead = wrap::config_bf_dead_deg() * kDegToRad;
    const float rate = wrap::config_bf_rate_deg_per_sec() * kDegToRad;
    const wrap::BodyTransferStep step =
        wrap::body_transfer_step(st, head_yaw, head_pitch, dead, rate, dt);

    const float ppr = wrap::config_bf_pixels_per_radian();
    if (!(ppr > 0.0f) || !std::isfinite(ppr)) return;
    bf_pending_dx_ += step.want_yaw * ppr;
    bf_pending_dy_ += -step.want_pitch * ppr;
    if (!std::isfinite(bf_pending_dx_) || !std::isfinite(bf_pending_dy_)) {
        bf_pending_dx_ = 0.0f;
        bf_pending_dy_ = 0.0f;
        return;
    }
    const auto dx = static_cast<int>(bf_pending_dx_);
    const auto dy = static_cast<int>(bf_pending_dy_);
    if (dx == 0 && dy == 0) return;
    bf_pending_dx_ -= static_cast<float>(dx);
    bf_pending_dy_ -= static_cast<float>(dy);

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));

    // ⚠ COMMIT EXACTLY WHAT WAS SENT - the checkbook. Not step.want_*: the
    // pixels are integers, and the remainder stays in the accumulator for the
    // next frame. Committing the wanted amount would advance the reference by
    // fractions that were never injected, and the two halves would drift.
    const float committed_yaw = static_cast<float>(dx) / ppr;
    const float committed_pitch = -static_cast<float>(dy) / ppr;
    const float before_yaw = wrap::recenter_ref_yaw();
    wrap::advance_recenter_ref(committed_yaw, committed_pitch);
    wrap::note_transfer(static_cast<float>(dx), static_cast<float>(dy),
                        committed_yaw, committed_pitch);
    // STRUCTURAL POST CHECK on the write. The reference must have moved by
    // exactly the committed amount; if it has not, the invariant is broken and
    // everything the run reports about drift or double-apply is void.
    bf_commits_.fetch_add(1, std::memory_order_relaxed);
    const float moved = wrap::wrap_pi(wrap::recenter_ref_yaw() - before_yaw);
    if (std::fabs(moved - committed_yaw) > 1e-5f) {
        const uint32_t n = bf_commit_post_bad_.fetch_add(1, std::memory_order_relaxed);
        if (n < 8)
            log::error(std::format(
                "BODYFOLLOW POST FAIL #{}: committed {:.6f} rad of yaw but the reference "
                "moved {:.6f}. The transfer invariant is BROKEN - treat every conclusion "
                "from this run about drift or double-apply as void.",
                n, committed_yaw, moved));
    }
}

void OpenXRContext::poll(VkImage source_image, VkExtent2D source_extent, VkFormat source_format,
    VkImage override_source_image, VkExtent2D override_source_extent, VkFormat override_source_format,
    VkImageLayout override_source_layout, VkFilter override_source_filter, bool override_eye1_only) {
    if (session_ == XR_NULL_HANDLE) return;
    // F12 RELEASED (2026-08-24) - see kXrAlternateEyeKeyEnabled above. The body
    // is kept, dead, behind a compile-time false so the mechanism can be
    // revived on a key nothing else uses.
    //
    // ⚠ THE RESET THIS BRANCH CARRIED IS DEAD TOO, NOT ORPHANED. Checked
    // before touching it, because a shared reset gated on a retired lever's
    // flag has bitten this project six times: eye_ever_rendered_ is read at
    // exactly one place (the `if (alternate_eye)` arm below) and written at
    // three (here, shutdown(), and its initialiser). With alternate_eye_enabled_
    // pinned false its only reader is unreachable, so removing this reset
    // disables no probe. alternate_eye_frame_counter_ is likewise only
    // incremented under `if (alternate_eye)`.
    if constexpr (kXrAlternateEyeKeyEnabled) {
        if ((GetAsyncKeyState(VK_F12) & 1) != 0) {
            const bool now_enabled = !alternate_eye_enabled_.load(std::memory_order_relaxed);
            alternate_eye_enabled_.store(now_enabled, std::memory_order_relaxed);
            log::info(now_enabled ? "AlternateEye mode ENABLED (F12)" : "AlternateEye mode DISABLED (F12)");
            eye_ever_rendered_ = {false, false};
            alternate_eye_frame_counter_ = 0;
        }
    }
    // ================= XR SESSION LIFECYCLE, MADE AUDIBLE (2026-08-24) ======
    //
    // WHAT THIS BLOCK USED TO BE, AND WHY IT IS THE PRIME SUSPECT. It handled
    // exactly two transitions - READY and STOPPING - and logged NEITHER. It
    // never named EXITING, LOSS_PENDING, IDLE, SYNCHRONIZED, VISIBLE or
    // FOCUSED, and then did a bare `if (!running_) return;` which, once
    // running_ went false, bailed silently on every subsequent frame forever.
    //
    // Every OTHER path by which VR can go away in this plugin logs loudly: the
    // doubled call's structured-exception self-disarm (vulkan_hooks.cpp ERROR,
    // with the whole switch state), the attempt cap (WARN), the scripted-test
    // stop, Pause/Break, and Run A's teardown. This one was the only silent
    // one - so if a cutscene makes the runtime stop or exit the session, VR
    // would disappear leaving ZERO evidence in the log. That is the most
    // likely reason "VR mode exits after a cutscene" has never been diagnosed.
    //
    // ⚠ SCOPE, POLARITY AND WHAT THIS IS NOT. Everything added here is
    // LOG-ONLY. No xrEndSession, xrDestroySession or xrBeginSession call is
    // added, moved or removed; the two existing transitions behave exactly as
    // before. EXITING and LOSS_PENDING are NAMED and REPORTED, not acted on -
    // acting on them would be a session-lifecycle fix for a cause that has not
    // been measured yet, and this run is the measurement. Nothing here can
    // change a pixel in either eye.
    //
    // POLARITY, stated before the run: on a healthy headset run this prints
    // the boot ladder ONCE (UNKNOWN -> IDLE -> READY -> SYNCHRONIZED ->
    // VISIBLE -> FOCUSED) and then says nothing at all. Any further line
    // during play is the event. Silence after the boot ladder is the healthy
    // reading, NOT a broken probe - the heartbeat below is what distinguishes
    // those two, because it fires only once running_ has actually gone false.
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            const XrSessionState old_state = state_;
            const bool was_running = running_;
            state_ = sc->state;
            if (state_ == XR_SESSION_STATE_READY && !running_) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(session_, &bi))) running_ = true;
            } else if (state_ == XR_SESSION_STATE_STOPPING && running_) {
                xrEndSession(session_); running_ = false;
            }
            // UNCAPPED, and deliberately so. Session-state changes are a
            // handful per run, not per frame - a cap here is the one thing
            // that could truncate the exact transition the run exists to
            // catch, and "held=N (cap N)" has already cost this project a
            // dozen runs of an arbitrary subset.
            log::warn(std::format(
                "XR SESSION STATE {} -> {} | running_ {} -> {} | this transition is {}",
                session_state_name(old_state), session_state_name(state_),
                was_running ? "true" : "false", running_ ? "true" : "false",
                (state_ == XR_SESSION_STATE_STOPPING ||
                 state_ == XR_SESSION_STATE_EXITING ||
                 state_ == XR_SESSION_STATE_LOSS_PENDING)
                    ? "AN EXIT PATH - signature (a): the RUNTIME ended the session. If this "
                      "sits between two CUTSCENE MARKER lines, the answer is session lifecycle "
                      "and DOOM's cutscene path is what triggered it."
                    : "part of the normal ladder (IDLE/READY/SYNCHRONIZED/VISIBLE/FOCUSED)."));
            // EXPLICITLY NAMED rather than left to fall through the two-branch
            // `if` above, which is what made them invisible. Reported only.
            if (state_ == XR_SESSION_STATE_EXITING) {
                log::error(
                    "XR SESSION STATE = EXITING. The runtime is asking the application to shut "
                    "the session down; this plugin does NOT act on it (by design, this build is "
                    "log-only) and will keep trying to submit frames, which the runtime will "
                    "refuse. VR is over from this line onward. THIS IS SIGNATURE (a).");
            } else if (state_ == XR_SESSION_STATE_LOSS_PENDING) {
                log::error(
                    "XR SESSION STATE = LOSS_PENDING. The runtime is about to lose the session "
                    "(device disconnect, runtime restart, or a compositor takeover). Not acted "
                    "on in this build. THIS IS SIGNATURE (a).");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            // The two-branch `if` did not look at this event type AT ALL, so
            // an instance loss was indistinguishable from nothing happening.
            log::error("XR INSTANCE LOSS PENDING - the whole XrInstance is going away, not just "
                       "the session. Also signature (a), one level up.");
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (!running_) {
        // THE HEARTBEAT THAT OUTLIVES THE BLOCK. The bare `return` here was
        // the second half of the silence: once running_ went false this
        // function did nothing, said nothing, and kept doing so every frame
        // for the rest of the session. Rate-limited rather than capped - the
        // first line is loud and complete, then one line a minute proves the
        // plugin is still alive and still not submitting, so a frozen log
        // cannot be mistaken for a healthy one.
        static bool announced = false;
        static uint32_t suppressed = 0;
        static uint64_t last_tick = 0;
        const uint64_t now = GetTickCount64();
        if (!announced || now - last_tick >= 60000) {
            log::error(std::format(
                "VR IS NOT SUBMITTING FRAMES. running_=false, state={} - poll() returns here "
                "before xrWaitFrame every single frame, and will keep doing so. {} "
                "Cross-reference the 'XR SESSION STATE' transition above this line for the state "
                "that ended it, and the CUTSCENE MARKER lines for whether it happened inside a "
                "cutscene.",
                session_state_name(state_),
                announced ? std::format("(heartbeat; {} frames suppressed since the last line)",
                                        suppressed)
                          : std::string("(first occurrence - everything above this line was a "
                                        "session that was still running)")));
            announced = true;
            suppressed = 0;
            last_tick = now;
        } else {
            ++suppressed;
        }
        return;
    }

    // ===== WHERE DOES THE FRAME TIME GO? (2026-08-24) =====================
    //
    // The 19:58 run collapsed from 80 fps to a steady 11 fps and STAYED there,
    // while every per-frame counter we own reported the identical workload
    // (~12.0 suppressed dispatches per frame, before and after). Same work,
    // seven times the wall clock - so the time is being spent somewhere none of
    // our counters look.
    //
    // There are only three places it can be: xrWaitFrame (the runtime pacing
    // us), our own eye rendering (acquire/blit/release, which can block in
    // xrWaitSwapchainImage), or xrEndFrame. This times all three and prints a
    // line whenever a frame is slow, rate-limited to once a second so a
    // sustained stall reports steadily instead of flooding.
    //
    // POLARITY, before the run: on a healthy 80 fps frame all three are ~0-12
    // ms. If the stall is in xrWaitFrame the RUNTIME is throttling us and the
    // fix is upstream of anything we render. If it is in renderEyes we are
    // blocking on a swapchain image the compositor has not released - which is
    // what forcing shouldRender true could plausibly cause.
    using xr_clock = std::chrono::steady_clock;
    const auto t_frame0 = xr_clock::now();
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(session_, &wi, &fs))) return;
    const auto t_afterwait = xr_clock::now();
    // ===== DORMANT LEVER: IGNORE shouldRender (2026-08-24) =================
    //
    // The 19:38 run ended with VR permanently dead: the runtime moved the
    // session FOCUSED -> VISIBLE, shouldRender went false, and we submitted
    // layerCount = 0 for the remaining 3.5 minutes. It never recovered.
    //
    // Per the OpenXR spec, VISIBLE means our composition layers are STILL
    // COMPOSITED - we just do not have input focus. shouldRender=false is a
    // hint to skip expensive rendering, not an instruction to submit nothing.
    // So submitting anyway is spec-legal and MIGHT keep the picture alive
    // through a focus blip instead of losing VR for the rest of the session.
    //
    // MIGHT. It is unproven, it is a behaviour change, and if the focus went
    // to something the compositor draws in front of us it will change nothing.
    // So it ships DORMANT, behind a file, default OFF - it can be turned on
    // between launches with no rebuild, which is what makes testing it cheap.
    //
    // ⚠ POLARITY, before the run: with the file present, a FOCUSED -> VISIBLE
    // transition should leave the headset showing OUR content. If VR still
    // dies, focus loss is not survivable from inside the application and the
    // fix has to be in why DOOM loses focus at all.
    static const bool ignore_should_render = [] {
        FILE* f = nullptr;
        if (fopen_s(&f, "C:\\dev\\doomvr-ignore-shouldrender.txt", "r") == 0 && f) {
            std::fclose(f);
            log::warn("IgnoreShouldRender ACTIVE: submitting composition layers even when the "
                      "runtime sets shouldRender=false. Spec-legal in VISIBLE state, where our "
                      "layers are still composited. If VR survives a FOCUSED -> VISIBLE "
                      "transition with this on, focus loss is survivable and this is the fix.");
            return true;
        }
        return false;
    }();
    // ⚠ THE ORIGINAL VALUE IS KEPT, AND THIS IS NOT OPTIONAL.
    //
    // The 19:58 run could not answer its own question because the first
    // version of this lever overwrote fs.shouldRender BEFORE any log line read
    // it. Every line then printed shouldRender=true no matter what the runtime
    // had actually said, and the only "shouldRender=false" in the whole session
    // was the banner above. The lever destroyed the evidence for the one thing
    // it exists to test - a broken probe, not a null result.
    const bool runtime_should_render = fs.shouldRender != XR_FALSE;
    if (ignore_should_render) fs.shouldRender = XR_TRUE;
    // Report the runtime's ANSWER changing, independently of what we then do
    // with it. Edge-triggered, so a healthy session prints one line and then
    // nothing. This is what tells us whether the runtime asked us to stop.
    {
        static int last_should = -1;
        const int now_should = runtime_should_render ? 1 : 0;
        if (now_should != last_should) {
            log::warn(std::format(
                "RUNTIME shouldRender {} -> {} | state={} running_={} | overriddenByUs={}. {}",
                last_should < 0 ? "(first)" : (last_should ? "true" : "false"),
                runtime_should_render ? "true" : "false",
                session_state_name(state_), running_ ? "true" : "false",
                ignore_should_render ? "YES - we submit anyway" : "no",
                runtime_should_render
                    ? "The runtime wants us rendering."
                    : "⚠ THE RUNTIME IS ASKING US TO STOP RENDERING. Without the override this "
                      "is where VR goes away; with it we keep submitting and the question "
                      "becomes whether the compositor still shows our layers or throttles us."));
            last_should = now_should;
        }
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(session_, &bi))) return;

    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = local_space_;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    const bool alternate_eye = alternate_eye_enabled_.load(std::memory_order_relaxed);
    // Which eye's swapchain gets fresh pixel content THIS present call, and
    // the toe-in yaw bias baked into this frame's look injection so that
    // content actually differs from the other eye's most recent frame.
    // ~1.4 degrees per eye (~2.8 degrees total swing) - a throwaway
    // starting point for the smoke test, not a tuned value.
    const uint32_t active_eye = alternate_eye_frame_counter_ % 2;
    constexpr float kAlternateEyeBiasRadians = 0.025f;
    const float yaw_bias = !alternate_eye ? 0.0f
        : (active_eye == 0 ? -kAlternateEyeBiasRadians : kAlternateEyeBiasRadians);
    // DOOM's live projection scale for this present was latched once by
    // wrap::begin_frame() in the present hook, upstream of here, so both eyes
    // read the same value - two independent reads could land either side of a
    // sampler update and give the two eyes different fields of view, which
    // would look exactly like a stereo/parallax fault and be hunted in the
    // camera. It is latched THERE and not here because that site also runs on
    // a headset-free session, where Phase 0 still has to report the scale.
    bool views_located = false;
    if (XR_SUCCEEDED(xrLocateViews(session_, &li, &vs, (uint32_t)views.size(), &count, views.data())) && count == 2) {
        views_located = true;
        head_x_.store(views[0].pose.orientation.x, std::memory_order_relaxed);
        head_y_.store(views[0].pose.orientation.y, std::memory_order_relaxed);
        head_z_.store(views[0].pose.orientation.z, std::memory_order_relaxed);
        head_w_.store(views[0].pose.orientation.w, std::memory_order_relaxed);
        update_look_injection(views[0].pose.orientation.x, views[0].pose.orientation.y,
                               views[0].pose.orientation.z, views[0].pose.orientation.w, yaw_bias);
        if (alternate_eye) ++alternate_eye_frame_counter_;
        static uint32_t decimate = 0;
        if ((decimate++ % 120) == 0) { log_pose(views[0], 0); log_pose(views[1], 1); }
    }

    const bool side_by_side = source_extent.width && source_extent.height &&
        static_cast<float>(source_extent.width) / static_cast<float>(source_extent.height) > 3.0f;
    static bool stereo_logged = false;
    if (side_by_side && !stereo_logged) {
        log::info("double-wide DOOM frame detected; submitting native stereo to OpenXR eyes");
        stereo_logged = true;
    }
    // StereoPreview (F3) / TrueStereo (F1, see vulkan_hooks.cpp): render
    // from a caller-supplied offscreen image via the projection layer,
    // using the same calculated fixed_view framing as AlternateEye below
    // rather than the real per-eye HMD pose/FOV - our offscreen content is
    // captured at DOOM's own internal rendering FOV, not the HMD's, so
    // using the real FOV here would hit the exact "zoomed in and blurry"
    // bug AlternateEye already found (see fixed_view's own comment).
    // StereoPreview: both eyes show this same image (no parallax).
    // TrueStereo (override_eye1_only): only eye 1 shows it - eye 0 stays
    // DOOM's own normal real frame, real per-eye pose/FOV, in the `else`
    // branch below, same as every non-special frame.
    const bool has_override_source = override_source_image != VK_NULL_HANDLE &&
        override_source_extent.width && override_source_extent.height &&
        override_source_format != VK_FORMAT_UNDEFINED;
    const bool stereo_preview = has_override_source && !override_eye1_only;
    const bool true_stereo = has_override_source && override_eye1_only;
    static bool stereo_preview_logged = false;
    if (stereo_preview && !stereo_preview_logged) {
        log::info("StereoPreview active; submitting offscreen redirect content to OpenXR eyes "
                  "(same image both eyes, no parallax yet)");
        stereo_preview_logged = true;
    }
    static bool true_stereo_logged = false;
    if (true_stereo && !true_stereo_logged) {
        log::info("TrueStereo active; eye 0 = DOOM's normal frame, eye 1 = redirected offscreen "
                  "content with diverged camera - first real per-eye parallax attempt");
        true_stereo_logged = true;
    }

    // Same fixed screen framing as the quad below (3.2m wide, 2m away,
    // world-locked). AlternateEye reuses it instead of the headset's real
    // per-eye FOV/pose: DOOM's image is rendered at DOOM's own internal FOV,
    // not the HMD's, so declaring the wider HMD FOV to the compositor was
    // stretching/magnifying the image (reported as "zoomed in and blurry").
    // Keeping both eyes at this identical fixed pose/FOV also isolates the
    // one thing this smoke test is actually evaluating - whether alternating
    // toe-in-biased CONTENT alone produces a depth cue - from any change in
    // apparent screen size or head-locked-vs-world-locked framing.
    // StereoPreview's content comes from override_source_extent, not
    // source_extent (DOOM's normal swapchain) - in practice these are the
    // same 3818x2032 this session, but compute aspect from the actual
    // source being displayed rather than assume they always match.
    const VkExtent2D& aspect_source_extent =
        (stereo_preview || true_stereo) ? override_source_extent : source_extent;
    // ⚠ THAT "in practice these are the same 3818x2032" WAS AN ASSUMPTION, and
    // nothing measured it (2026-08-29). It sets the FOV basis for the whole
    // true-stereo path, so if the mirror is a different size than DOOM's frame
    // every declared angle on that path is wrong. Reported whenever the pair
    // changes, not once, because the mirror is re-created at scene transitions.
    {
        static uint32_t last_sw = 0, last_sh = 0, last_ow = 0, last_oh = 0;
        if (source_extent.width != last_sw || source_extent.height != last_sh ||
            override_source_extent.width != last_ow ||
            override_source_extent.height != last_oh) {
            last_sw = source_extent.width;   last_sh = source_extent.height;
            last_ow = override_source_extent.width;
            last_oh = override_source_extent.height;
            const bool same = source_extent.width == override_source_extent.width &&
                              source_extent.height == override_source_extent.height;
            log::warn(std::format(
                "SourceExtents doomFrame={}x{} overrideMirror={}x{} match={} "
                "fovBasis={} (stereoPreview={} trueStereo={})\n"
                "READING IT: under true stereo the doubled eye is blitted from the "
                "MIRROR and the declared field of view for BOTH eyes is derived "
                "from the mirror's aspect. match=NO means the two eyes describe "
                "different rectangles and every stereo judgement from the run is "
                "void - fix the extent before reading the picture.",
                source_extent.width, source_extent.height,
                override_source_extent.width, override_source_extent.height,
                same ? "YES" : "***NO***",
                (stereo_preview || true_stereo) ? "overrideMirror" : "doomFrame",
                stereo_preview, true_stereo));
        }
    }
    const float aspect = aspect_source_extent.width && aspect_source_extent.height
        ? static_cast<float>(aspect_source_extent.width) / static_cast<float>(aspect_source_extent.height)
        : 16.0f / 9.0f;
    constexpr float screen_width_metres = 3.2f;
    constexpr float screen_distance_metres = 2.0f;
    XrView fixed_view{XR_TYPE_VIEW};
    fixed_view.pose.orientation.w = 1.0f;
    fixed_view.pose.position = {0.0f, 0.0f, -screen_distance_metres};
    // fixed_view's FOV: derived from DOOM's own confirmed projection-matrix
    // scale term (renderView+0xC44 row index 1 = 1.639 - a clean, isolated
    // diagonal value with the rest of that row at 0, confirmed stable and
    // reproducible across many independent live samples this session, see
    // docs/RE-NOTES.md camera matrix findings), NOT the screen_width_metres/
    // screen_distance_metres geometry above (that pair is specific to the
    // flat MonoTracked quad layer positioned in world space, a separate,
    // unrelated use of those same constants - kept as-is for the quad).
    // Interpreting 1.639 as the standard perspective Y-scale term
    // (1/tan(fovY/2)) gives ~62.8deg vertical / ~97.8deg horizontal at this
    // content's aspect ratio - noticeably wider than the screen-geometry-
    // implied ~77deg horizontal previously used here, which live testing
    // reported as "boxed in, not full screen." Inferred from a real,
    // confirmed value, not guessed - but not verified pixel-exact either;
    // if it's over/under live, that's the next thing to tune, same as the
    // look-injection pixels-per-radian constant was tuned by feel earlier.
    constexpr float kDoomProjectionScaleY = 1.639f;
    const float fixed_fov_y = 2.0f * std::atan(1.0f / kDoomProjectionScaleY);
    const float fixed_fov_x = 2.0f * std::atan(std::tan(fixed_fov_y * 0.5f) * aspect);
    fixed_view.fov.angleLeft = -fixed_fov_x * 0.5f;
    fixed_view.fov.angleRight = fixed_fov_x * 0.5f;
    fixed_view.fov.angleUp = fixed_fov_y * 0.5f;
    fixed_view.fov.angleDown = -fixed_fov_y * 0.5f;

    // ===================== PHASE 0 FACTS + PHASE 2-v1 POSE =================
    //
    // Phase 0's XR half. Published EVERY frame and EVERY session, armed or
    // not, because the line that reads it has to print on the headset-free
    // diagnostic run too and must be able to say which half is missing.
    if (views_located) {
        wrap::XrFacts facts;
        facts.have = true;
        for (uint32_t e = 0; e < 2; ++e) {
            facts.eye_w[e] = eyes_[e].width;
            facts.eye_h[e] = eyes_[e].height;
            facts.fov_l[e] = views[e].fov.angleLeft;
            facts.fov_r[e] = views[e].fov.angleRight;
            facts.fov_u[e] = views[e].fov.angleUp;
            facts.fov_d[e] = views[e].fov.angleDown;
        }
        facts.blit_src_w = aspect_source_extent.width;
        facts.blit_src_h = aspect_source_extent.height;
        wrap::publish_xr_facts(facts);
    }
    // Read AT the measurement, from the flag that actually gates SendInput -
    // not from the switch file, and not from an assumption in a run
    // instruction. The 23:18 run declared a head-tracked pose over a camera
    // that never moved because this was false and nothing checked it.
    wrap::publish_look_injection(look_injection_enabled_.load(std::memory_order_relaxed));

    // PHASE 3. Publish the head's yaw/pitch/roll for Lever A1 to rotate DOOM's
    // world camera by. Taken from views[0] in local space, so it is the head's
    // rotation relative to where the owner was facing at session start - a
    // delta on top of whatever the mouse has the body pointing at, which is
    // exactly the neck-on-torso composition the matrix write performs.
    //
    // Roll is included. Phase 2 stripped it because DOOM's input path has no
    // roll axis; here WE are the camera, the roll is genuinely rendered, and
    // declaring it means submitted pose == rendered pose with no wedges.
    if (views_located) {
        const XrQuaternionf& o = views[0].pose.orientation;
        float hy = 0.0f, hp = 0.0f, hr = 0.0f;
        const bool ok = wrap::yaw_pitch_roll_from_quat(
            wrap::Quat{o.x, o.y, o.z, o.w}, hy, hp, hr);
        wrap::publish_head_angles(hy, hp, hr, ok);
        // ===== THE SLEW (slot 1, 2026-08-27) ===============================
        //
        // Under the content anchor the head has ~+/-11 deg yaw / ~+/-5 deg
        // pitch of world before the content edge. The slew turns the BODY
        // toward any sustained head offset past a deadzone, so the window
        // recenters under the gaze: quick glances stay decoupled (inside the
        // deadzone nothing moves), sustained turns become game turns - the
        // gun comes along, which is how every shipped seated VR mod handles
        // it. Closed-loop with the body anchor: the injected turn moves DOOM's
        // yaw, the anchor tracks DOOM's yaw, the panel follows, the offset
        // shrinks. Uses the SendInput path whose kPixelsPerRadian was
        // live-calibrated in the look-injection era. Gameplay-gated so a menu
        // can never receive synthetic mouse motion.
        if (ok && wrap::anchor_content_active() && wrap::slew_active() &&
            !look_injection_enabled_.load(std::memory_order_relaxed)) {
            bool gameplay = false;
            {
                wrap::VpSnapshot vp;
                if (wrap::world_vp(vp)) gameplay = wrap::is_gameplay_view(vp.m);
            }
            float ay = 0.0f, ap = 0.0f;
            if (gameplay && wrap::body_anchor_angles(ay, ap)) {
                float rel_yaw = hy - ay;
                while (rel_yaw > 3.14159265f) rel_yaw -= 6.28318531f;
                while (rel_yaw < -3.14159265f) rel_yaw += 6.28318531f;
                const float rel_pitch = hp - ap;
                // The dial lives in the switch file: slewdead (deg) is the
                // decoupled-glance zone, slewrate the per-frame catch-up.
                // Defaults are 1:1 VR (0 deg, 0.30): the camera follows the
                // head and the reprojection window stays under the gaze.
                const float dead_yaw = wrap::config().slew_dead_deg * 0.0174533f;
                const float dead_pitch = dead_yaw * 0.6f;
                const float catchup = wrap::config().slew_rate;
                constexpr float kPixelsPerRadian = 3000.0f;  // look-injection calibration
                auto excess = [](float v, float dead) {
                    if (v > dead) return v - dead;
                    if (v < -dead) return v + dead;
                    return 0.0f;
                };
                const float step_yaw = excess(rel_yaw, dead_yaw) * catchup;
                const float step_pitch = excess(rel_pitch, dead_pitch) * catchup;
                static float pending_dx = 0.0f, pending_dy = 0.0f;
                pending_dx += step_yaw * kPixelsPerRadian;
                pending_dy += -step_pitch * kPixelsPerRadian;
                if (!std::isfinite(pending_dx) || !std::isfinite(pending_dy)) {
                    pending_dx = pending_dy = 0.0f;
                }
                const int dx = static_cast<int>(pending_dx);
                const int dy = static_cast<int>(pending_dy);
                if (dx != 0 || dy != 0) {
                    pending_dx -= static_cast<float>(dx);
                    pending_dy -= static_cast<float>(dy);
                    INPUT input{};
                    input.type = INPUT_MOUSE;
                    input.mi.dx = dx;
                    input.mi.dy = dy;
                    input.mi.dwFlags = MOUSEEVENTF_MOVE;
                    SendInput(1, &input, sizeof(INPUT));
                }
            }
        }
    } else {
        wrap::publish_head_angles(0.0f, 0.0f, 0.0f, false);
    }

    // PHASE 2-v1: DECLARE WHAT WAS RENDERED, NOT WHAT THE RUNTIME LOCATED.
    //
    // Look injection drives DOOM's camera from the head's yaw and pitch, so
    // yaw and pitch ARE in the content and declaring them is what world-locks
    // the scene. Roll is NOT: DOOM's input path has no roll axis, so no roll
    // was ever rendered, and handing the runtime's roll to the compositor
    // would assert a tilt the pixels do not have. The compositor then rotates
    // our un-rolled image to the real head roll, which is where the known v1
    // black wedges at the corners come from. Stated before the run, not
    // discovered in the headset.
    //
    // The yaw/pitch extraction is wrap::strip_roll, whose convention is copied
    // term for term from update_look_injection above - the same function that
    // produced the content. A different convention here would be the same
    // class of error as declaring a different fov.
    //
    // Position: each eye declares its own located position by default, which
    // makes the compositor's spatial reprojection near-identity. pose=shared
    // in the switch file collapses both to eye 0's, which is what today's
    // working virtual-screen arm does - one line, no rebuild, if the
    // compositor's stereo baseline turns out to fight the one already baked
    // into the content.
    // ===== THE MENU MUST STAY A WORLD-FIXED SCREEN ========================
    //
    // Owner, after RUN 3: "opening menu is PINNED to my head. This needs to
    // stay like a screen that I can move my head to look around."
    //
    // The menu is drawn in DOOM's own screen space, so it is baked into the
    // image we blit. Declare a head-tracked pose over it and it is welded to
    // the face by construction - there is no crop or FOV that fixes that.
    //
    // So while the menu is up we drop back to the QUAD layer, which is the
    // virtual screen this project already had: a fixed pose in local space,
    // world-locked, exactly "a screen you can look around". Head rotation of
    // DOOM's camera is suspended at the same time, so the world behind the
    // menu holds still too rather than swinging while you read it.
    //
    // Detection is DOOM's own GUI draw storm, measured in this repo before
    // this feature existed: doom_render_view jumps from ~70 to ~493 calls per
    // frame at the menu, and DOOMHOOKS 196 -> 618
    // (STATE-2026-08-25-MENU-STUTTER-SHELVED.md finding 1). The threshold sits
    // in the wide gap between those two states, and every transition is logged
    // so a mis-detection shows up as a line rather than as a mystery.
    // ⚠ menu_is_open() is LOG-ONLY and deliberately not consulted here. See
    // wrap::head_look_active(): the rate thresholds were built from a "~70
    // calls/frame" figure measured in a different scene, this scene's gameplay
    // sits above the off-threshold, and the detector latched into MENU and
    // disabled head rotation for a whole run. The menu needs a real signal, not
    // a draw-rate guess, and until it has one it drives nothing.
    const bool wrap_now = wrap::armed() && views_located;
    std::array<XrView, 2> wrap_views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    if (wrap_now) {
        for (uint32_t e = 0; e < 2; ++e) {
            // PHASE 3: DECLARE THE FULL LOCATED ORIENTATION, ROLL INCLUDED.
            //
            // Phase 2 stripped roll because DOOM's input path has no roll axis,
            // so no roll was ever rendered - and that bought black wedges on
            // head tilt. Lever A1 now writes the head's full rotation into
            // DOOM's world camera, so the roll IS in the content. Submitted
            // pose == rendered pose, the compositor's reprojection is identity,
            // and both the wedges and the input-path swim go away.
            wrap_views[e].pose.orientation = views[e].pose.orientation;
            // ===== THE CONTENT ANCHOR (2026-08-27) =========================
            //
            // Owner's acceptance statement, verbatim: "if we can keep
            // EVERYTHING still and not follow my head it would be perfect" -
            // said about state C, whose image is DOOM's own coherent frame
            // with NO differential rotation anywhere in it.
            //
            // anchor=content declares the LOCAL-space identity orientation -
            // the session's reference facing, where the body aims - instead
            // of the located head pose. The compositor then world-locks the
            // whole frame AS A UNIT and the head looks around WITHIN it.
            // Nothing inside DOOM is rotated at all (run with head-look
            // parked), so the ~20 differential defects cannot exist by
            // construction: they were all disagreements between populations
            // about how much of the head pose each had absorbed, and here
            // NOTHING absorbs it - the pose label carries all of it.
            //
            // Cost, stated before the run: content edges. At slider 130 the
            // content is 132x101 deg against the headset's 110x90, so black
            // appears beyond ~+/-11 deg of yaw and ~+/-5 deg of pitch from
            // the body's facing. That slack is the acceptance question this
            // arm exists to put in front of the owner.
            if (wrap::anchor_content_active()) {
                // Second build (same day): not identity - the BODY's live
                // facing, read from DOOM's own clean camera as a delta from
                // the arming epoch. Mouse turns then move panel and content
                // together, so the world holds still around you while you
                // turn (true VR turning semantics), and a calibration error
                // cannot open up between panel and image because both come
                // from the same engine state. Falls back to identity until
                // the reference basis has been published (first armed frames).
                float ay = 0.0f, ap = 0.0f;
                XrQuaternionf q{0.0f, 0.0f, 0.0f, 1.0f};
                if (wrap::body_anchor_angles(ay, ap)) {
                    const wrap::Quat wq = wrap::quat_from_yaw_pitch(ay, ap);
                    q = XrQuaternionf{wq.x, wq.y, wq.z, wq.w};
                }
                wrap_views[e].pose.orientation = q;
            }
            wrap_views[e].pose.position = wrap::config().pose_shared
                ? views[0].pose.position : views[e].pose.position;
            // Placeholder only. render_eye OVERWRITES this from the crop it
            // actually blitted; it is seeded with the located fov so that a
            // path which somehow skipped the blit cannot submit zeros.
            wrap_views[e].fov = views[e].fov;
        }
    }

    std::array<XrCompositionLayerProjectionView, 2> projection_views{};
    const auto t_render0 = xr_clock::now();
    bool rendered;
    if (alternate_eye) {
        rendered = fs.shouldRender && count == 2 &&
            render_eye(active_eye, fixed_view, source_image, source_extent,
                       source_format, projection_views[active_eye]);
        eye_ever_rendered_[active_eye] = eye_ever_rendered_[active_eye] || rendered;
        const uint32_t other_eye = 1 - active_eye;
        if (rendered && !eye_ever_rendered_[other_eye]) {
            // Bootstrap: prime the other eye once so AlternateEye mode
            // doesn't start by showing a blank/uninitialized view in it.
            eye_ever_rendered_[other_eye] = render_eye(other_eye, fixed_view, source_image,
                source_extent, source_format, projection_views[other_eye]);
        } else if (eye_ever_rendered_[other_eye]) {
            // Reuse the other eye's last rendered pixels (one or more frames
            // stale - that's the expected judder for this smoke test); pose/
            // FOV are the same fixed screen framing every frame regardless.
            auto& target = eyes_[other_eye];
            projection_views[other_eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projection_views[other_eye].pose = fixed_view.pose;
            projection_views[other_eye].fov = fixed_view.fov;
            projection_views[other_eye].subImage.swapchain = target.handle;
            projection_views[other_eye].subImage.imageRect.offset = {0, 0};
            projection_views[other_eye].subImage.imageRect.extent = {target.width, target.height};
            projection_views[other_eye].subImage.imageArrayIndex = 0;
        }
        rendered = rendered && eye_ever_rendered_[0] && eye_ever_rendered_[1];
    } else if (stereo_preview) {
        // fixed_view, not views[0]/views[1]: our offscreen content is
        // captured at DOOM's own internal rendering FOV, not the HMD's -
        // using the real per-eye FOV here would hit the exact same
        // "zoomed in and blurry" bug AlternateEye already found and fixed
        // by using this same calculated fixed_view instead (see the
        // comment above fixed_view's computation). Both eyes get the
        // identical fixed pose/FOV, matching that both eyes currently show
        // identical content (no parallax yet).
        rendered = fs.shouldRender && count == 2 &&
            render_eye(0, wrap_now ? wrap_views[0] : fixed_view, override_source_image,
                       override_source_extent, override_source_format, projection_views[0],
                       override_source_layout, override_source_filter, wrap_now) &&
            render_eye(1, wrap_now ? wrap_views[1] : fixed_view, override_source_image,
                       override_source_extent, override_source_format, projection_views[1],
                       override_source_layout, override_source_filter, wrap_now);
    } else if (true_stereo) {
        // Both eyes use fixed_view, not views[0]/views[1]: the plain
        // `else` branch below computes projection_views[0] from views[0]
        // too, but that data normally goes UNUSED, since the non-special
        // path actually displays through the flat quad layer below (fixed
        // FOV), not the projection layer - this is the first mode that
        // ever actually SUBMITS eye 0 through the projection layer with a
        // declared FOV, and DOOM's content (both eyes here - eye 0 is
        // DOOM's real frame, eye 1 our redirected one) is rendered at
        // DOOM's own internal FOV either way, not the HMD's. Confirmed
        // live: using views[0]'s real FOV for eye 0 reproduced the exact
        // "zoomed in and blurry" bug AlternateEye/StereoPreview already
        // hit and fixed - eye 0 needs the same fix, not just eye 1.
        // EYE SWAP (2026-08-28). Normally eye 0 is DOOM's own frame (the REAL
        // call) and eye 1 is our mirror (the DOUBLED call, the one missing its
        // additive layer). With C:\dev\doomvr-swapeyes.txt present the two are
        // submitted to the opposite eyes, and vulkan_hooks.cpp flips both
        // camera signs to match so the pair stays orthoscopic.
        //
        // The point is to find out whether the missing effects follow the
        // DOUBLED CALL or follow the RIGHT EYE. Nothing has ever tested that,
        // and the two have been treated as the same thing throughout.
        static const bool swap_eyes = [] {
            FILE* f = nullptr;
            if (fopen_s(&f, "C:\\dev\\doomvr-swapeyes.txt", "r") == 0 && f) {
                std::fclose(f);
                return true;
            }
            return false;
        }();
        static bool swap_logged = false;
        if (swap_eyes && !swap_logged) {
            log::warn("EyeSwap ACTIVE: the DOUBLED call's mirror is being submitted to eye 0 "
                      "(LEFT) and DOOM's real frame to eye 1 (RIGHT), with both camera signs "
                      "flipped to match. If the missing effects move to the LEFT eye they follow "
                      "the doubled call; if they stay in the RIGHT eye they follow the eye slot "
                      "and the fault is in presentation, not rendering.");
            swap_logged = true;
        }
        const uint32_t real_eye = swap_eyes ? 1u : 0u;
        const uint32_t doubled_eye = swap_eyes ? 0u : 1u;
        rendered = fs.shouldRender && count == 2 &&
            render_eye(real_eye, wrap_now ? wrap_views[real_eye] : fixed_view, source_image,
                       source_extent, source_format, projection_views[real_eye],
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_FILTER_LINEAR, wrap_now) &&
            render_eye(doubled_eye, wrap_now ? wrap_views[doubled_eye] : fixed_view,
                       override_source_image, override_source_extent,
                       override_source_format, projection_views[doubled_eye],
                       override_source_layout, override_source_filter, wrap_now);
    } else if (wrap_now) {
        // WRAPAROUND WITH A MONO SOURCE. No stereo mode is armed, so both eyes
        // get DOOM's single frame with the wraparound framing and each eye's
        // own roll-stripped located pose. There is NO parallax in this branch
        // and that is said out loud rather than hidden - it exists so an armed
        // wraparound session still produces a world-locked image instead of
        // silently dropping to the flat quad, which would look exactly like
        // the wraparound build not working at all.
        rendered = fs.shouldRender && count == 2 &&
            render_eye(0, wrap_views[0], source_image, source_extent, source_format,
                       projection_views[0], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                       VK_FILTER_LINEAR, true) &&
            render_eye(1, wrap_views[1], source_image, source_extent, source_format,
                       projection_views[1], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                       VK_FILTER_LINEAR, true);
        static bool wrap_mono_logged = false;
        if (!wrap_mono_logged) {
            wrap_mono_logged = true;
            log::warn("Wraparound is running on DOOM's MONO frame in BOTH eyes - no stereo mode "
                      "is armed this session (no PG UP, no DOWN). World lock and framing are "
                      "live; depth is not. Arm the stereo path for the real Phase 2 judgement.");
        }
    } else {
        rendered = fs.shouldRender && count == 2 &&
            render_eye(0, views[0], source_image, source_extent, source_format,
                       projection_views[0]) &&
            (!side_by_side || render_eye(1, views[1], source_image, source_extent,
                                         source_format, projection_views[1]));
    }

    const auto t_render1 = xr_clock::now();

    // Present the mono DOOM frame as a world-fixed virtual monitor instead of
    // stretching it across the headset's full projection FOV. Local-space
    // identity is the headset pose at session start, so z=-2 places the screen
    // approximately two metres in front of the initial viewing position.
    // (aspect/screen_width_metres computed above, shared with AlternateEye's
    // fixed_view framing.)
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = local_space_;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = eyes_[0].handle;
    quad.subImage.imageRect.offset = {0, 0};
    quad.subImage.imageRect.extent = {eyes_[0].width, eyes_[0].height};
    quad.subImage.imageArrayIndex = 0;
    quad.pose.orientation.w = 1.0f;
    quad.pose.position = {0.0f, 0.0f, -screen_distance_metres};
    quad.size = {screen_width_metres, screen_width_metres / aspect};
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};

    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = local_space_;
    projection.viewCount = 2;
    projection.views = projection_views.data();
    // wrap_now joins this list because the QUAD is the virtual screen - the
    // exact thing wraparound replaces. An armed session that fell through to
    // the quad would show a flat panel and read as "the build did nothing".
    if (side_by_side || alternate_eye || stereo_preview || true_stereo || wrap_now) {
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    }

    // ================= WHICH LAYER DOES THE HEADSET ACTUALLY GET? ==========
    //
    // (2026-08-24) The run of 11:46 showed VR "going away" at a cutscene while
    // the XR session stayed FOCUSED, no XR call failed, the swapchain handle
    // never changed and leverAapplied kept climbing at ~120/s. Every one of
    // those counters lives UPSTREAM of this point. None of them can see the
    // one thing that actually decides what reaches the eyes: which composition
    // layer gets submitted.
    //
    // There are exactly three outcomes, and until now the log named none of
    // them:
    //   PROJECTION - real stereo, one image per eye. This is "in VR".
    //   QUAD       - the world-fixed flat virtual monitor, 3.2 m wide, 2 m
    //                away, SAME image to both eyes. This is what "it switched
    //                to a floating screen" means.
    //   NONE       - layerCount 0. We submit nothing and the runtime shows its
    //                own environment. Not a monitor: an empty compositor.
    //
    // EDGE-TRIGGERED, so a steady state costs one comparison per frame and
    // prints nothing. Every transition prints, with the full set of booleans
    // that decided it - because the interesting question is never "which
    // layer" but "which flag moved".
    //
    // ⚠ THE PRECONDITIONS ARE REPORTED, NOT RE-EVALUATED. `rendered` is built
    // from a short-circuited chain that CALLS render_eye, and render_eye does
    // real GPU work - acquire, blit, release. Re-running it here to find out
    // which half failed would change what the frame does. So this records the
    // cheap preconditions (shouldRender, view count, override availability)
    // and reports "a render_eye call failed" by elimination when all of those
    // passed and `rendered` is still false. Log-only, strictly.
    {
        // ⚠ MUST MATCH THE REAL SELECTOR ABOVE, WHICH INCLUDES wrap_now.
        // It did not (2026-08-29), so every armed-wraparound session without a
        // stereo mode was REPORTED as "XR LAYER -> QUAD (flat virtual monitor)"
        // while a PROJECTION layer was actually submitted. That cost a session:
        // the owner reported a flat screen, this line appeared to corroborate
        // it, and the investigation went to the layer type instead of to the
        // framing. A reporter that re-derives its subject can disagree with it;
        // this one is now the same expression.
        const bool use_projection =
            side_by_side || alternate_eye || stereo_preview || true_stereo || wrap_now;
        const int layer_kind = !rendered ? 0 : (use_projection ? 1 : 2);
        // Published every frame, not only on a change: the marker reads this at
        // an arbitrary instant and needs the CURRENT value, not the value as of
        // the last transition.
        last_layer_kind_.store(layer_kind, std::memory_order_relaxed);
        static int last_layer_kind = -1;
        if (layer_kind != last_layer_kind) {
            const char* kName[] = {"NONE (layerCount=0, the runtime shows its own environment)",
                                   "PROJECTION (real stereo, one image per eye - IN VR)",
                                   "QUAD (flat virtual monitor, same image to both eyes - the "
                                   "floating screen)"};
            const char* transition = last_layer_kind < 0 ? "first submission of the session"
                                   : layer_kind == 1     ? "⚠ ENTERED STEREO"
                                   : last_layer_kind == 1 ? "⚠⚠ LEFT STEREO - this is the line "
                                                            "the cutscene question turns on"
                                                          : "changed between two non-stereo modes";
            log::warn(std::format(
                "XR LAYER -> {} | {} | WHY: shouldRender={} viewCount={} hasOverrideSource={} "
                "overrideEye1Only={} sideBySide={} alternateEye={} stereoPreview={} "
                "trueStereo={} rendered={}{}",
                kName[layer_kind], transition,
                fs.shouldRender ? "true" : "false", count,
                has_override_source ? "true" : "false",
                override_eye1_only ? "true" : "false",
                side_by_side ? "true" : "false", alternate_eye ? "true" : "false",
                stereo_preview ? "true" : "false", true_stereo ? "true" : "false",
                rendered ? "true" : "false",
                (!rendered && fs.shouldRender && count == 2)
                    ? " | ALL PRECONDITIONS PASSED AND rendered IS STILL FALSE, so a render_eye "
                      "call failed - acquire, blit or release on one of the eye swapchains."
                    : ""));
            last_layer_kind = layer_kind;
        }
        // ⚠ THE HEARTBEAT THAT THE 19:38 RUN PROVED WAS MISSING.
        //
        // VR died at 19:41:27 and stayed dead for the remaining 3.5 minutes of
        // that session, and the log recorded it in exactly ONE line. The
        // existing "VR IS NOT SUBMITTING FRAMES" heartbeat could not help:
        // it is gated on running_ == false, and running_ stayed TRUE the whole
        // time. The session was alive, we were calling xrWaitFrame/BeginFrame/
        // EndFrame every frame - we were just passing layerCount = 0.
        //
        // That is a silent, permanent VR loss with a healthy-looking session,
        // and it is the exact shape this project keeps getting caught by. So
        // it now gets its own heartbeat, on its own condition, rate-limited to
        // one line a minute and reporting how many frames it has swallowed.
        if (!rendered) {
            static bool zero_announced = false;
            static uint32_t zero_frames = 0;
            static uint64_t zero_last_tick = 0;
            ++zero_frames;
            const uint64_t now = GetTickCount64();
            if (!zero_announced || now - zero_last_tick >= 60000) {
                log::error(std::format(
                    "SUBMITTING NO LAYERS - the headset is showing the runtime's own "
                    "environment, not us. layerCount=0 for {} frames. shouldRender={} "
                    "viewCount={} state={} running_={}. {}",
                    zero_frames, fs.shouldRender ? "true" : "false", count,
                    session_state_name(state_), running_ ? "true" : "false",
                    !fs.shouldRender
                        ? "shouldRender=false is the runtime telling us not to render. Paired "
                          "with an 'XR SESSION STATE FOCUSED -> VISIBLE' line just before it, "
                          "this is FOCUS LOSS: something else took input focus and the "
                          "compositor stopped showing our layers. See doomvr-ignore-shouldrender."
                        : "shouldRender is TRUE, so this is a render_eye failure, not focus "
                          "loss - acquire, blit or release failed on an eye swapchain."));
                zero_announced = true;
                zero_last_tick = now;
                zero_frames = 0;
            }
        }
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = rendered ? 1u : 0u;
    ei.layers = rendered ? layers : nullptr;
    const auto t_end0 = xr_clock::now();
    xrEndFrame(session_, &ei);
    const auto t_end1 = xr_clock::now();
    {
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        const double total_ms = ms(t_frame0, t_end1);
        // 25 ms is slower than every healthy frame measured in any run to date
        // (80 fps = 12.5 ms, and the 60 fps cutscene cadence = 16.7 ms), so it
        // cannot fire during normal play and needs no tuning.
        if (total_ms > 25.0) {
            static uint64_t slow_last_tick = 0;
            static uint32_t slow_frames = 0;
            ++slow_frames;
            const uint64_t now = GetTickCount64();
            if (now - slow_last_tick >= 1000) {
                log::warn(std::format(
                    "XR FRAME SLOW: total={:.1f} ms | xrWaitFrame={:.1f} renderEyes={:.1f} "
                    "xrEndFrame={:.1f} | {} slow frames in the last second | "
                    "runtimeShouldRender={} overridden={} state={} layerKind={}. "
                    "WHERE THE TIME IS decides the cause: xrWaitFrame => the RUNTIME is pacing "
                    "us and nothing we render can fix it; renderEyes => we are blocking on a "
                    "swapchain image the compositor has not released; xrEndFrame => submission "
                    "is back-pressuring.",
                    total_ms, ms(t_frame0, t_afterwait), ms(t_render0, t_render1),
                    ms(t_end0, t_end1), slow_frames,
                    runtime_should_render ? "true" : "false",
                    ignore_should_render ? "yes" : "no",
                    session_state_name(state_),
                    layer_kind_name(last_layer_kind_.load(std::memory_order_relaxed))));
                slow_last_tick = now;
                slow_frames = 0;
            }
        }
    }

    // ENGAGEMENT, AFTER THE SUBMIT. Once a second while armed, matching the
    // FrameBudget window's cadence. Read AT the measurement and from the
    // struct that was actually handed to xrEndFrame, so a run cannot report
    // agreement it did not have. Silent when the switch file is absent.
    wrap::maybe_emit_engagement(
        look_injection_enabled_.load(std::memory_order_relaxed), views_located);
}

void OpenXRContext::shutdown() {
    for (auto& eye : eyes_) {
        if (eye.handle != XR_NULL_HANDLE) xrDestroySwapchain(eye.handle);
        eye = {};
    }
    if (command_pool_ != VK_NULL_HANDLE && get_device_proc_addr_ && vk_device_) {
        auto destroy_pool = load_device_proc<PFN_vkDestroyCommandPool>(
            get_device_proc_addr_, vk_device_, "vkDestroyCommandPool");
        if (destroy_pool) destroy_pool(vk_device_, command_pool_, nullptr);
    }
    command_pool_ = VK_NULL_HANDLE;
    if (local_space_ != XR_NULL_HANDLE) xrDestroySpace(local_space_);
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
    local_space_ = XR_NULL_HANDLE; session_ = XR_NULL_HANDLE; instance_ = XR_NULL_HANDLE;
    system_ = XR_NULL_SYSTEM_ID;
    state_ = XR_SESSION_STATE_UNKNOWN;
    running_ = false;
    vk_device_ = VK_NULL_HANDLE;
    vk_queue_ = VK_NULL_HANDLE;
    get_device_proc_addr_ = nullptr;
    swapchain_format_ = VK_FORMAT_UNDEFINED;
    look_have_last_ = false;
    look_pending_dx_ = 0.0f;
    look_pending_dy_ = 0.0f;
    eye_ever_rendered_ = {false, false};
    alternate_eye_frame_counter_ = 0;
}
}
