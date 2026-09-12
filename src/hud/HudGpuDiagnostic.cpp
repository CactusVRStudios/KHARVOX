#include "../common/DiagnosticLogging.h"
#include "HudGpuDiagnostic.h"

#include "HudHook.h"
#include "../camera/CameraHook.h"
#include "../common/RuntimePaths.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kharvox::hudgpu {
namespace {

template <typename Handle>
std::uint64_t handleValue(Handle handle) {
    if constexpr (std::is_pointer_v<Handle>)
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
    else
        return static_cast<std::uint64_t>(handle);
}

struct ImageInfo {
    std::uint32_t width{};
    std::uint32_t height{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkImageUsageFlags usage{};
    bool swapchain{};
};

struct ViewInfo {
    std::uint64_t image{};
    VkFormat format{VK_FORMAT_UNDEFINED};
};

struct FramebufferInfo {
    std::uint64_t renderPass{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint64_t> images;
};

struct RenderPassInfo {
    std::vector<VkImageLayout> finalLayouts;
    std::vector<VkAttachmentLoadOp> loadOps;
};

struct SwapchainInfo {
    std::uint32_t width{};
    std::uint32_t height{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkImageUsageFlags usage{};
    std::vector<std::uint64_t> images;
};

struct PipelineInfo {
    std::uint64_t renderPass{};
    std::uint32_t subpass{};
    std::uint32_t stageCount{};
    VkPrimitiveTopology topology{VK_PRIMITIVE_TOPOLOGY_MAX_ENUM};
    bool blending{};
    bool alphaWrite{};
    bool colorWrite{};
    bool depthTest{};
    bool depthWrite{};
};

struct DescriptorImageInfo {
    std::uint32_t set{};
    std::uint32_t binding{};
    std::uint32_t arrayElement{};
    VkDescriptorType type{VK_DESCRIPTOR_TYPE_MAX_ENUM};
    std::uint64_t sampler{};
    std::uint64_t view{};
    std::uint64_t image{};
    std::uint32_t width{};
    std::uint32_t height{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkImageUsageFlags usage{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
};

struct DescriptorSetInfo {
    std::vector<DescriptorImageInfo> images;
};

struct VertexBindingInfo {
    std::uint32_t binding{};
    std::uint64_t buffer{};
    VkDeviceSize offset{};
};

struct AttachmentInfo {
    std::uint64_t image{};
    std::uint32_t width{};
    std::uint32_t height{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkImageUsageFlags usage{};
    VkImageLayout finalLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkAttachmentLoadOp loadOp{VK_ATTACHMENT_LOAD_OP_DONT_CARE};
    bool swapchain{};
};

struct PipelineUse {
    std::uint64_t pipeline{};
    std::uint64_t draws{};
    std::uint64_t workItems{};
    PipelineInfo info{};
};

struct DrawGroup {
    std::uint64_t signature{};
    std::uint64_t pipeline{};
    DrawKind kind{DrawKind::Direct};
    std::uint64_t calls{};
    std::uint64_t submittedDraws{};
    std::uint64_t workItems{};
    std::uint32_t firstOrdinal{};
    std::uint32_t lastOrdinal{};
    PipelineInfo pipelineInfo{};
    std::vector<DescriptorImageInfo> descriptorImages;
    std::vector<VertexBindingInfo> vertexBindings;
    std::uint64_t indexBuffer{};
    VkDeviceSize indexOffset{};
    VkIndexType indexType{VK_INDEX_TYPE_MAX_ENUM};
};

struct PassRecord {
    std::uint64_t commandBuffer{};
    std::uint64_t renderPass{};
    std::uint64_t framebuffer{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t subpasses{1};
    VREye eye{VREye::Mono};
    std::uint64_t hudMatchedBegin{};
    std::uint64_t hudMatchedEnd{};
    std::uint64_t draws{};
    std::uint64_t directDraws{};
    std::uint64_t indexedDraws{};
    std::uint64_t indirectDraws{};
    std::uint64_t workItems{};
    std::uint64_t blendedDraws{};
    std::uint64_t alphaWriteDraws{};
    std::uint64_t colorWriteDraws{};
    std::uint64_t depthTestDraws{};
    std::uint64_t depthWriteDraws{};
    std::uint32_t viewportWidth{};
    std::uint32_t viewportHeight{};
    std::uint32_t scissorWidth{};
    std::uint32_t scissorHeight{};
    bool transparentClear{};
    bool incomplete{};
    std::vector<AttachmentInfo> attachments;
    std::vector<PipelineUse> pipelines;
    std::vector<DrawGroup> drawGroups;
};

struct CommandState {
    std::uint64_t currentPipeline{};
    PipelineInfo currentPipelineInfo{};
    bool currentPipelineKnown{};
    std::array<std::uint64_t, 8> boundDescriptorSets{};
    std::vector<DescriptorImageInfo> boundDescriptorImages;
    std::array<VertexBindingInfo, 8> vertexBindings{};
    std::uint64_t indexBuffer{};
    VkDeviceSize indexOffset{};
    VkIndexType indexType{VK_INDEX_TYPE_MAX_ENUM};
    std::uint32_t nextDrawOrdinal{};
    bool active{};
    PassRecord pass;
};

struct CandidateAggregate {
    std::uint64_t signature{};
    std::uint64_t renderPass{};
    std::uint64_t dominantPipeline{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t uses{};
    std::uint64_t draws{};
    std::uint64_t workItems{};
    std::uint64_t blendedDraws{};
    std::uint64_t alphaWriteDraws{};
    std::uint64_t indirectDraws{};
    std::uint64_t leftUses{};
    std::uint64_t rightUses{};
    std::uint64_t monoUses{};
    bool offscreen{};
    bool sampleable{};
    bool alphaCapable{};
    bool transparentClear{};
    bool incomplete{};
    std::vector<AttachmentInfo> attachments;
    std::unordered_map<std::uint64_t, PipelineUse> pipelines;
};

struct DrawCandidateAggregate {
    std::uint64_t signature{};
    std::uint64_t passSignature{};
    std::uint64_t pipeline{};
    DrawKind kind{DrawKind::Direct};
    std::uint32_t targetWidth{};
    std::uint32_t targetHeight{};
    std::uint64_t calls{};
    std::uint64_t submittedDraws{};
    std::uint64_t workItems{};
    std::uint64_t frameUses{};
    std::uint64_t lastFrame{};
    std::uint32_t minimumOrdinal{~0u};
    std::uint32_t maximumOrdinal{};
    PipelineInfo pipelineInfo{};
    std::vector<DescriptorImageInfo> descriptorImages;
    std::vector<VertexBindingInfo> vertexBindings;
    std::uint64_t indexBuffer{};
    VkDeviceSize indexOffset{};
    VkIndexType indexType{VK_INDEX_TYPE_MAX_ENUM};
    std::unordered_map<std::uint64_t, std::uint64_t> profileFrameUses;
};

std::mutex resourceMutex;
std::unordered_map<std::uint64_t, ImageInfo> images;
std::unordered_map<std::uint64_t, ViewInfo> views;
std::unordered_map<std::uint64_t, FramebufferInfo> framebuffers;
std::unordered_map<std::uint64_t, RenderPassInfo> renderPasses;
std::unordered_map<std::uint64_t, SwapchainInfo> swapchains;
std::unordered_map<std::uint64_t, PipelineInfo> pipelines;
std::unordered_map<std::uint64_t, DescriptorSetInfo> descriptorSets;

std::mutex completedMutex;
std::vector<PassRecord> completedPasses;
std::unordered_map<std::uint64_t, CandidateAggregate> candidates;
std::unordered_map<std::uint64_t, DrawCandidateAggregate> drawCandidates;
std::vector<KharvoxHudDiagnosticEvent> observedHudProfiles;
std::unordered_map<std::uint64_t, std::uint64_t> hudProfileFrameUses;
std::uint64_t lastMatchedAtPresent{};
std::uint64_t lastCompletedAtPresent{};
std::uint64_t lastCrosshairAtPresent{};
std::uint64_t activeHudFrames{};
std::atomic<bool> captureComplete{};

std::mutex quadCandidateMutex;
QuadCandidate latestQuadCandidate{};
std::atomic<std::uint64_t> quadCandidateSerial{};

thread_local std::unordered_map<std::uint64_t, CommandState> commandStates;

std::mutex logMutex;
void log(const std::string& text) {
    if (!kharvox::extendedDiagnosticsEnabled()) return;
    std::lock_guard<std::mutex> guard(logMutex);
    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    std::ofstream out(std::string(temp) + "KHARVOX.log", std::ios::app);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    out << '[' << time.wHour << ':' << time.wMinute << ':' << time.wSecond
        << '.' << time.wMilliseconds << "] [KHARVOX][HUD9-GPU] " << text << '\n';
}

bool formatHasAlpha(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SNORM:
    case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_UINT_PACK32:
    case VK_FORMAT_A8B8G8R8_SINT_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UINT_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return true;
    default:
        return false;
    }
}

bool isCaptureMilestone(std::uint64_t frame) {
    return frame == 1 || frame == 30 || frame == 120
        || frame == 300 || frame == 600 || frame == 1200;
}

std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}

std::uint64_t hudProfileSignature(const KharvoxHudDiagnosticEvent& profile) {
    std::uint64_t signature = 0x510e527fade682d1ull;
    signature = hashCombine(signature, profile.callerRva);
    signature = hashCombine(signature, static_cast<std::uint32_t>(profile.width));
    signature = hashCombine(signature, static_cast<std::uint32_t>(profile.height));
    signature = hashCombine(signature, static_cast<std::uint32_t>(profile.scaleMilli));
    signature = hashCombine(signature, profile.crosshair ? 1 : 0);
    return signature;
}

const KharvoxHudDiagnosticEvent* findHudProfile(std::uint64_t signature) {
    const auto profile = std::find_if(
        observedHudProfiles.begin(), observedHudProfiles.end(),
        [signature](const KharvoxHudDiagnosticEvent& entry) {
            return hudProfileSignature(entry) == signature;
        });
    return profile == observedHudProfiles.end() ? nullptr : &*profile;
}

PipelineInfo lookupPipeline(std::uint64_t pipeline, bool& found) {
    std::lock_guard<std::mutex> guard(resourceMutex);
    const auto iterator = pipelines.find(pipeline);
    found = iterator != pipelines.end();
    return found ? iterator->second : PipelineInfo{};
}

std::vector<AttachmentInfo> lookupAttachments(std::uint64_t framebuffer) {
    std::vector<AttachmentInfo> result;
    std::lock_guard<std::mutex> guard(resourceMutex);
    const auto framebufferIt = framebuffers.find(framebuffer);
    if (framebufferIt == framebuffers.end()) return result;
    const auto renderPass = renderPasses.find(framebufferIt->second.renderPass);
    for (std::size_t attachmentIndex = 0;
         attachmentIndex < framebufferIt->second.images.size(); ++attachmentIndex) {
        const auto image = framebufferIt->second.images[attachmentIndex];
        AttachmentInfo attachment{};
        attachment.image = image;
        const auto imageIt = images.find(image);
        if (imageIt != images.end()) {
            attachment.width = imageIt->second.width;
            attachment.height = imageIt->second.height;
            attachment.format = imageIt->second.format;
            attachment.usage = imageIt->second.usage;
            attachment.swapchain = imageIt->second.swapchain;
        }
        if (renderPass != renderPasses.end()) {
            if (attachmentIndex < renderPass->second.finalLayouts.size())
                attachment.finalLayout = renderPass->second.finalLayouts[attachmentIndex];
            if (attachmentIndex < renderPass->second.loadOps.size())
                attachment.loadOp = renderPass->second.loadOps[attachmentIndex];
        }
        result.push_back(attachment);
    }
    return result;
}

bool isImageDescriptorType(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_SAMPLER
        || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
        || type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
        || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
        || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}

void refreshBoundDescriptorImages(CommandState& state) {
    state.boundDescriptorImages.clear();
    std::lock_guard<std::mutex> guard(resourceMutex);
    for (std::uint32_t setIndex = 0;
         setIndex < static_cast<std::uint32_t>(state.boundDescriptorSets.size()); ++setIndex) {
        const auto descriptorSet = state.boundDescriptorSets[setIndex];
        if (!descriptorSet) continue;
        const auto set = descriptorSets.find(descriptorSet);
        if (set == descriptorSets.end()) continue;
        for (auto image : set->second.images) {
            image.set = setIndex;
            state.boundDescriptorImages.push_back(image);
            if (state.boundDescriptorImages.size() >= 48) return;
        }
    }
}

std::uint64_t drawSignature(
    const CommandState& state, DrawKind kind, const PassRecord& pass) {
    std::uint64_t signature = 0x6a09e667f3bcc909ull;
    signature = hashCombine(signature, pass.renderPass);
    signature = hashCombine(signature, pass.width);
    signature = hashCombine(signature, pass.height);
    signature = hashCombine(signature, state.currentPipeline);
    signature = hashCombine(signature, static_cast<std::uint64_t>(kind));
    for (const auto& image : state.boundDescriptorImages) {
        signature = hashCombine(signature, image.set);
        signature = hashCombine(signature, image.binding);
        signature = hashCombine(signature, image.arrayElement);
        signature = hashCombine(signature, static_cast<std::uint64_t>(image.type));
        signature = hashCombine(signature, image.view);
        signature = hashCombine(signature, image.width);
        signature = hashCombine(signature, image.height);
        signature = hashCombine(signature, static_cast<std::uint64_t>(image.format));
    }
    for (const auto& vertex : state.vertexBindings) {
        if (!vertex.buffer) continue;
        signature = hashCombine(signature, vertex.binding);
        signature = hashCombine(signature, vertex.buffer);
    }
    signature = hashCombine(signature, state.indexBuffer);
    signature = hashCombine(signature, static_cast<std::uint64_t>(state.indexType));
    return signature;
}

void finishPass(CommandState& state, bool incomplete) {
    if (!state.active) return;
    KharvoxHudDiagnosticSnapshot hud{};
    KharvoxHudGetDiagnosticSnapshot(hud);
    state.pass.hudMatchedEnd = hud.matchedSurfaces;
    state.pass.incomplete = incomplete;
    if (!incomplete && state.pass.width == 960 && state.pass.height == 540
        && state.pass.draws == 1 && state.pass.blendedDraws == 1
        && state.pass.attachments.size() == 1 && state.pass.transparentClear) {
        const auto& attachment = state.pass.attachments.front();
        const auto requiredUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
            | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (attachment.image && !attachment.swapchain
            && attachment.format == VK_FORMAT_R8G8B8A8_UNORM
            && (attachment.usage & requiredUsage) == requiredUsage
            && attachment.finalLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
            QuadCandidate candidate{};
            candidate.image = reinterpret_cast<VkImage>(
                static_cast<std::uintptr_t>(attachment.image));
            candidate.extent = {attachment.width, attachment.height};
            candidate.format = attachment.format;
            candidate.layout = attachment.finalLayout;
            candidate.serial = quadCandidateSerial.fetch_add(
                1, std::memory_order_acq_rel) + 1;
            bool changed{};
            {
                std::lock_guard<std::mutex> guard(quadCandidateMutex);
                changed = latestQuadCandidate.image != candidate.image;
                latestQuadCandidate = candidate;
            }
            if (changed) {
                std::ostringstream out;
                out << "HUD9 quad source selected image=0x" << std::hex
                    << attachment.image << std::dec
                    << " extent=" << attachment.width << 'x' << attachment.height
                    << " format=" << attachment.format
                    << " finalLayout=" << attachment.finalLayout;
                log(out.str());
            }
        }
    }
    if (state.pass.draws) {
        std::lock_guard<std::mutex> guard(completedMutex);
        if (completedPasses.size() < 4096)
            completedPasses.push_back(std::move(state.pass));
    }
    state.pass = {};
    state.active = false;
}

std::uint64_t dominantPipeline(const PassRecord& pass) {
    const PipelineUse* best{};
    for (const auto& pipeline : pass.pipelines)
        if (!best || pipeline.draws > best->draws) best = &pipeline;
    return best ? best->pipeline : 0;
}

std::uint64_t candidateSignature(const PassRecord& pass) {
    std::uint64_t signature = 0xcbf29ce484222325ull;
    signature = hashCombine(signature, pass.renderPass);
    signature = hashCombine(signature, pass.width);
    signature = hashCombine(signature, pass.height);
    signature = hashCombine(signature, dominantPipeline(pass));
    signature = hashCombine(signature, pass.subpasses);
    for (const auto& attachment : pass.attachments) {
        signature = hashCombine(signature, static_cast<std::uint64_t>(attachment.format));
        signature = hashCombine(signature, attachment.usage);
        signature = hashCombine(signature, attachment.swapchain ? 1 : 0);
    }
    return signature;
}

void aggregatePass(
    const PassRecord& pass, const std::vector<std::uint64_t>& frameHudProfiles) {
    const auto signature = candidateSignature(pass);
    auto& candidate = candidates[signature];
    if (!candidate.uses) {
        candidate.signature = signature;
        candidate.renderPass = pass.renderPass;
        candidate.dominantPipeline = dominantPipeline(pass);
        candidate.width = pass.width;
        candidate.height = pass.height;
        candidate.attachments = pass.attachments;
        candidate.offscreen = !pass.attachments.empty();
        for (const auto& attachment : pass.attachments) {
            candidate.offscreen = candidate.offscreen && !attachment.swapchain;
            candidate.sampleable = candidate.sampleable
                || (attachment.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
            candidate.alphaCapable = candidate.alphaCapable || formatHasAlpha(attachment.format);
        }
    }
    ++candidate.uses;
    candidate.draws += pass.draws;
    candidate.workItems += pass.workItems;
    candidate.blendedDraws += pass.blendedDraws;
    candidate.alphaWriteDraws += pass.alphaWriteDraws;
    candidate.indirectDraws += pass.indirectDraws;
    candidate.transparentClear = candidate.transparentClear || pass.transparentClear;
    candidate.incomplete = candidate.incomplete || pass.incomplete;
    if (pass.eye == VREye::Left) ++candidate.leftUses;
    else if (pass.eye == VREye::Right) ++candidate.rightUses;
    else ++candidate.monoUses;
    for (const auto& pipeline : pass.pipelines) {
        auto& aggregate = candidate.pipelines[pipeline.pipeline];
        if (!aggregate.pipeline) {
            aggregate.pipeline = pipeline.pipeline;
            aggregate.info = pipeline.info;
        }
        aggregate.draws += pipeline.draws;
        aggregate.workItems += pipeline.workItems;
    }
    const auto passSignature = signature;
    for (const auto& draw : pass.drawGroups) {
        auto candidateDrawIterator = drawCandidates.find(draw.signature);
        if (candidateDrawIterator == drawCandidates.end()) {
            if (drawCandidates.size() >= 50000) continue;
            candidateDrawIterator = drawCandidates.emplace(
                draw.signature, DrawCandidateAggregate{}).first;
        }
        auto& candidateDraw = candidateDrawIterator->second;
        if (!candidateDraw.calls) {
            candidateDraw.signature = draw.signature;
            candidateDraw.passSignature = passSignature;
            candidateDraw.pipeline = draw.pipeline;
            candidateDraw.kind = draw.kind;
            candidateDraw.targetWidth = pass.width;
            candidateDraw.targetHeight = pass.height;
            candidateDraw.pipelineInfo = draw.pipelineInfo;
            candidateDraw.descriptorImages = draw.descriptorImages;
            candidateDraw.vertexBindings = draw.vertexBindings;
            candidateDraw.indexBuffer = draw.indexBuffer;
            candidateDraw.indexOffset = draw.indexOffset;
            candidateDraw.indexType = draw.indexType;
        }
        candidateDraw.calls += draw.calls;
        candidateDraw.submittedDraws += draw.submittedDraws;
        candidateDraw.workItems += draw.workItems;
        candidateDraw.minimumOrdinal = std::min(
            candidateDraw.minimumOrdinal, draw.firstOrdinal);
        candidateDraw.maximumOrdinal = std::max(
            candidateDraw.maximumOrdinal, draw.lastOrdinal);
        if (candidateDraw.lastFrame != activeHudFrames) {
            candidateDraw.lastFrame = activeHudFrames;
            ++candidateDraw.frameUses;
            for (const auto profile : frameHudProfiles)
                ++candidateDraw.profileFrameUses[profile];
        }
    }
}

double candidateScore(const CandidateAggregate& candidate) {
    const double draws = static_cast<double>(std::max<std::uint64_t>(1, candidate.draws));
    const double blendRatio = static_cast<double>(candidate.blendedDraws) / draws;
    const double alphaRatio = static_cast<double>(candidate.alphaWriteDraws) / draws;
    const double averageDraws = draws / static_cast<double>(std::max<std::uint64_t>(1, candidate.uses));
    double score = blendRatio * 100.0 + alphaRatio * 20.0;
    if (candidate.offscreen) score += 25.0;
    if (candidate.sampleable) score += 25.0;
    if (candidate.alphaCapable) score += 20.0;
    if (candidate.transparentClear) score += 30.0;
    if (averageDraws >= 1.0 && averageDraws <= 800.0) score += 15.0;
    if (candidate.incomplete) score -= 10.0;
    return score;
}

bool descriptorMatchesObservedHudProfile(const DescriptorImageInfo& image) {
    if (!image.width || !image.height) return false;
    for (const auto& profile : observedHudProfiles) {
        if (profile.crosshair) continue;
        if (profile.width == static_cast<int>(image.width)
            && profile.height == static_cast<int>(image.height)) return true;
    }
    return false;
}

double drawCandidateScore(const DrawCandidateAggregate& candidate) {
    const double calls = static_cast<double>(std::max<std::uint64_t>(1, candidate.calls));
    const double averageWork = static_cast<double>(candidate.workItems) / calls;
    const double frameFrequency = static_cast<double>(candidate.frameUses)
        / static_cast<double>(std::max<std::uint64_t>(1, activeHudFrames));
    double score{};
    if (candidate.targetWidth >= 1920 && candidate.targetHeight >= 1080) score += 25.0;
    if (candidate.pipelineInfo.blending) score += 45.0;
    if (candidate.pipelineInfo.alphaWrite) score += 15.0;
    if (candidate.kind == DrawKind::Indexed || candidate.kind == DrawKind::Direct) score += 10.0;
    if (frameFrequency >= 0.75 && frameFrequency <= 1.05) score += 30.0;
    else if (frameFrequency >= 0.05 && frameFrequency <= 8.0) score += 10.0;
    const auto ordinalSpread = candidate.maximumOrdinal >= candidate.minimumOrdinal
        ? candidate.maximumOrdinal - candidate.minimumOrdinal : ~0u;
    if (ordinalSpread <= 2) score += 25.0;
    else if (ordinalSpread <= 12) score += 12.0;
    if (averageWork > 0.0 && averageWork <= 20000.0) score += 10.0;
    bool sampledAlpha{};
    bool exactHudDimensions{};
    for (const auto& image : candidate.descriptorImages) {
        sampledAlpha = sampledAlpha
            || ((image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 && formatHasAlpha(image.format));
        exactHudDimensions = exactHudDimensions || descriptorMatchesObservedHudProfile(image);
    }
    if (sampledAlpha) score += 25.0;
    if (exactHudDimensions) score += 60.0;
    if (candidate.descriptorImages.empty()) score -= 25.0;
    double bestTransientCorrelation{};
    for (const auto& correlation : candidate.profileFrameUses) {
        const auto profileFrames = hudProfileFrameUses.find(correlation.first);
        if (profileFrames == hudProfileFrameUses.end() || !profileFrames->second) continue;
        const double prevalence = static_cast<double>(profileFrames->second)
            / static_cast<double>(std::max<std::uint64_t>(1, activeHudFrames));
        if (prevalence >= 0.8) continue;
        const double conditional = static_cast<double>(correlation.second)
            / static_cast<double>(profileFrames->second);
        bestTransientCorrelation = std::max(bestTransientCorrelation, conditional);
    }
    score += bestTransientCorrelation * 45.0;
    return score;
}

const char* drawKindName(DrawKind kind) {
    switch (kind) {
    case DrawKind::Direct: return "direct";
    case DrawKind::Indexed: return "indexed";
    case DrawKind::Indirect: return "indirect";
    case DrawKind::IndexedIndirect: return "indexed-indirect";
    }
    return "unknown";
}

std::string descriptorText(const DrawCandidateAggregate& candidate) {
    std::ostringstream out;
    for (std::size_t index = 0; index < candidate.descriptorImages.size(); ++index) {
        if (index) out << ';';
        const auto& image = candidate.descriptorImages[index];
        out << "s" << image.set << "b" << image.binding << '[' << image.arrayElement << ']'
            << ":view=0x" << std::hex << image.view << std::dec
            << ',' << image.width << 'x' << image.height
            << ",fmt=" << image.format
            << ",usage=0x" << std::hex << image.usage << std::dec
            << ",type=" << image.type;
        if (descriptorMatchesObservedHudProfile(image)) out << ",HUDDIM";
    }
    return out.str();
}

std::string correlationText(const DrawCandidateAggregate& candidate) {
    struct Correlation {
        std::uint64_t signature{};
        std::uint64_t cooccurrence{};
        std::uint64_t profileFrames{};
        double conditional{};
    };
    std::vector<Correlation> correlations;
    correlations.reserve(candidate.profileFrameUses.size());
    for (const auto& entry : candidate.profileFrameUses) {
        const auto frames = hudProfileFrameUses.find(entry.first);
        if (frames == hudProfileFrameUses.end() || !frames->second) continue;
        correlations.push_back({entry.first, entry.second, frames->second,
            static_cast<double>(entry.second) / static_cast<double>(frames->second)});
    }
    std::sort(correlations.begin(), correlations.end(),
        [](const Correlation& left, const Correlation& right) {
            if (left.conditional != right.conditional)
                return left.conditional > right.conditional;
            return left.cooccurrence > right.cooccurrence;
        });
    std::ostringstream out;
    for (std::size_t index = 0; index < std::min<std::size_t>(4, correlations.size()); ++index) {
        if (index) out << ';';
        const auto& correlation = correlations[index];
        const auto* profile = findHudProfile(correlation.signature);
        out << "profile=0x" << std::hex << correlation.signature << std::dec;
        if (profile) {
            out << ",caller=0x" << std::hex << profile->callerRva << std::dec
                << ',' << profile->width << 'x' << profile->height
                << ",crosshair=" << (profile->crosshair ? 1 : 0);
        }
        out << ",co=" << correlation.cooccurrence << '/' << correlation.profileFrames
            << ",p=" << std::fixed << std::setprecision(3) << correlation.conditional;
    }
    return out.str();
}

void logDrawCandidates() {
    struct RankedDraw {
        const DrawCandidateAggregate* candidate{};
        double score{};
    };
    std::vector<RankedDraw> ranked;
    ranked.reserve(drawCandidates.size());
    for (const auto& entry : drawCandidates)
        ranked.push_back({&entry.second, drawCandidateScore(entry.second)});
    std::sort(ranked.begin(), ranked.end(), [](const RankedDraw& left, const RankedDraw& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.candidate->frameUses > right.candidate->frameUses;
    });
    log("draw-level signatures=" + std::to_string(drawCandidates.size())
        + " observedHudProfiles=" + std::to_string(observedHudProfiles.size()));
    for (std::size_t index = 0; index < std::min<std::size_t>(30, ranked.size()); ++index) {
        const auto& draw = *ranked[index].candidate;
        const double averageWork = static_cast<double>(draw.workItems)
            / static_cast<double>(std::max<std::uint64_t>(1, draw.calls));
        const double frequency = static_cast<double>(draw.frameUses)
            / static_cast<double>(std::max<std::uint64_t>(1, activeHudFrames));
        std::ostringstream out;
        out << std::fixed << std::setprecision(3)
            << "draw#" << index + 1
            << " score=" << ranked[index].score
            << " sig=0x" << std::hex << draw.signature
            << " passSig=0x" << draw.passSignature
            << " pipe=0x" << draw.pipeline << std::dec
            << " target=" << draw.targetWidth << 'x' << draw.targetHeight
            << " kind=" << drawKindName(draw.kind)
            << " calls=" << draw.calls
            << " frameUses=" << draw.frameUses
            << " frequency=" << frequency
            << " ordinal=" << draw.minimumOrdinal << ".." << draw.maximumOrdinal
            << " avgWork=" << averageWork
            << " blend=" << (draw.pipelineInfo.blending ? 1 : 0)
            << " alpha=" << (draw.pipelineInfo.alphaWrite ? 1 : 0)
            << " depth=" << (draw.pipelineInfo.depthTest ? 1 : 0)
            << " topology=" << draw.pipelineInfo.topology
            << " ib=0x" << std::hex << draw.indexBuffer << std::dec
            << " descriptors=[" << descriptorText(draw) << ']'
            << " correlations=[" << correlationText(draw) << ']';
        log(out.str());
    }
}

std::string attachmentText(const CandidateAggregate& candidate) {
    std::ostringstream out;
    for (std::size_t index = 0; index < candidate.attachments.size(); ++index) {
        if (index) out << ';';
        const auto& attachment = candidate.attachments[index];
        out << "img=0x" << std::hex << attachment.image << std::dec
            << ',' << attachment.width << 'x' << attachment.height
            << ",fmt=" << attachment.format
            << ",usage=0x" << std::hex << attachment.usage << std::dec
            << ",wsi=" << (attachment.swapchain ? 1 : 0);
    }
    return out.str();
}

void logCandidates(std::uint64_t presentSerial, const KharvoxHudDiagnosticSnapshot& hud) {
    struct Ranked {
        const CandidateAggregate* candidate{};
        double score{};
    };
    std::vector<Ranked> ranked;
    ranked.reserve(candidates.size());
    for (const auto& entry : candidates)
        ranked.push_back({&entry.second, candidateScore(entry.second)});
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& left, const Ranked& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.candidate->blendedDraws > right.candidate->blendedDraws;
    });

    {
        std::ostringstream out;
        out << "milestone activeHudFrames=" << activeHudFrames
            << " present=" << presentSerial
            << " signatures=" << candidates.size()
            << " drawSignatures=" << drawCandidates.size()
            << " observedProfiles=" << observedHudProfiles.size()
            << " lastProfile={caller=0x" << std::hex << hud.lastCallerRva << std::dec
            << ",size=" << hud.lastWidth << 'x' << hud.lastHeight
            << ",scaleMilli=" << hud.lastScaleMilli
            << ",thread=" << hud.lastThreadId
            << ",crosshair=" << (hud.lastWasCrosshair ? 1 : 0) << '}';
        log(out.str());
    }
    {
        std::ostringstream out;
        out << "profiles=";
        for (std::size_t index = 0; index < observedHudProfiles.size(); ++index) {
            if (index) out << ';';
            const auto& profile = observedHudProfiles[index];
            out << "0x" << std::hex << profile.callerRva << std::dec
                << ',' << profile.width << 'x' << profile.height
                << ",scale=" << profile.scaleMilli
                << ",crosshair=" << (profile.crosshair ? 1 : 0)
                << ",frames=" << hudProfileFrameUses[hudProfileSignature(profile)];
        }
        log(out.str());
    }

    for (std::size_t index = 0; index < std::min<std::size_t>(12, ranked.size()); ++index) {
        const auto& candidate = *ranked[index].candidate;
        const double averageDraws = static_cast<double>(candidate.draws)
            / static_cast<double>(std::max<std::uint64_t>(1, candidate.uses));
        std::ostringstream out;
        out << std::fixed << std::setprecision(2)
            << "candidate#" << index + 1
            << " score=" << ranked[index].score
            << " sig=0x" << std::hex << candidate.signature
            << " rp=0x" << candidate.renderPass
            << " primaryPipe=0x" << candidate.dominantPipeline << std::dec
            << " extent=" << candidate.width << 'x' << candidate.height
            << " uses=" << candidate.uses
            << " avgDraws=" << averageDraws
            << " blend=" << candidate.blendedDraws << '/' << candidate.draws
            << " alphaWrite=" << candidate.alphaWriteDraws << '/' << candidate.draws
            << " indirect=" << candidate.indirectDraws
            << " offscreen=" << (candidate.offscreen ? 1 : 0)
            << " sampled=" << (candidate.sampleable ? 1 : 0)
            << " alphaFmt=" << (candidate.alphaCapable ? 1 : 0)
            << " clearA0=" << (candidate.transparentClear ? 1 : 0)
            << " eyes=L" << candidate.leftUses << "/R" << candidate.rightUses
            << "/M" << candidate.monoUses
            << " attachments=[" << attachmentText(candidate) << ']';
        log(out.str());

        std::vector<const PipelineUse*> pipelineRanking;
        for (const auto& pipeline : candidate.pipelines)
            pipelineRanking.push_back(&pipeline.second);
        std::sort(pipelineRanking.begin(), pipelineRanking.end(),
            [](const PipelineUse* left, const PipelineUse* right) {
                return left->draws > right->draws;
            });
        std::ostringstream pipelineText;
        pipelineText << "candidate#" << index + 1 << " pipelines=";
        for (std::size_t pipelineIndex = 0;
             pipelineIndex < std::min<std::size_t>(4, pipelineRanking.size()); ++pipelineIndex) {
            if (pipelineIndex) pipelineText << ';';
            const auto& pipeline = *pipelineRanking[pipelineIndex];
            pipelineText << "0x" << std::hex << pipeline.pipeline << std::dec
                << ",draws=" << pipeline.draws
                << ",blend=" << (pipeline.info.blending ? 1 : 0)
                << ",alpha=" << (pipeline.info.alphaWrite ? 1 : 0)
                << ",depth=" << (pipeline.info.depthTest ? 1 : 0)
                << ",subpass=" << pipeline.info.subpass;
        }
        log(pipelineText.str());
    }
    logDrawCandidates();
}

} // namespace

bool enabled() {
    static const bool value = kharvox::extendedDiagnosticsEnabled() && kharvox::runtimeFileExists(L"enable_hud_gpu_diagnostic");
    return value;
}

void swapchainCreated(VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info) {
    if (!enabled()) return;
    if (!swapchain) return;
    std::lock_guard<std::mutex> guard(resourceMutex);
    swapchains[handleValue(swapchain)] = {
        info.imageExtent.width, info.imageExtent.height, info.imageFormat, info.imageUsage, {}};
}

void swapchainImages(VkSwapchainKHR swapchain, std::uint32_t count, const VkImage* swapchainImages) {
    if (!enabled()) return;
    if (!swapchain || !swapchainImages) return;
    std::lock_guard<std::mutex> guard(resourceMutex);
    const auto swapchainIt = swapchains.find(handleValue(swapchain));
    if (swapchainIt == swapchains.end()) return;
    auto& swapchainInfo = swapchainIt->second;
    swapchainInfo.images.clear();
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto image = handleValue(swapchainImages[index]);
        swapchainInfo.images.push_back(image);
        images[image] = {swapchainInfo.width, swapchainInfo.height,
            swapchainInfo.format, swapchainInfo.usage, true};
    }
}

void swapchainDestroyed(VkSwapchainKHR swapchain) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> guard(resourceMutex);
    const auto iterator = swapchains.find(handleValue(swapchain));
    if (iterator == swapchains.end()) return;
    for (const auto image : iterator->second.images) images.erase(image);
    swapchains.erase(iterator);
}

void imageCreated(VkImage image, const VkImageCreateInfo& info) {
    if (!enabled()) return;
    if (!image) return;
    std::lock_guard<std::mutex> guard(resourceMutex);
    images[handleValue(image)] = {
        info.extent.width, info.extent.height, info.format, info.usage, false};
}

void imageDestroyed(VkImage image) {
    if (!enabled()) return;
    const auto destroyed = handleValue(image);
    {
        std::lock_guard<std::mutex> guard(resourceMutex);
        images.erase(destroyed);
    }
    std::lock_guard<std::mutex> candidateGuard(quadCandidateMutex);
    if (handleValue(latestQuadCandidate.image) == destroyed)
        latestQuadCandidate = {};
}

void imageViewCreated(VkImageView view, const VkImageViewCreateInfo& info) {
    if (!enabled()) return;
    if (!view) return;
    std::lock_guard<std::mutex> guard(resourceMutex);
    VkFormat format = info.format;
    const auto imageIt = images.find(handleValue(info.image));
    if (format == VK_FORMAT_UNDEFINED && imageIt != images.end()) format = imageIt->second.format;
    views[handleValue(view)] = {handleValue(info.image), format};
}

void imageViewDestroyed(VkImageView view) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> guard(resourceMutex);
    views.erase(handleValue(view));
}

void framebufferCreated(VkFramebuffer framebuffer, const VkFramebufferCreateInfo& info) {
    if (!enabled()) return;
    if (!framebuffer) return;
    FramebufferInfo framebufferInfo{};
    framebufferInfo.renderPass = handleValue(info.renderPass);
    framebufferInfo.width = info.width;
    framebufferInfo.height = info.height;
    {
        std::lock_guard<std::mutex> guard(resourceMutex);
        for (std::uint32_t index = 0; index < info.attachmentCount; ++index) {
            const auto viewHandle = info.pAttachments ? handleValue(info.pAttachments[index]) : 0;
            const auto view = views.find(viewHandle);
            framebufferInfo.images.push_back(view == views.end() ? 0 : view->second.image);
        }
        framebuffers[handleValue(framebuffer)] = std::move(framebufferInfo);
    }
}

void framebufferDestroyed(VkFramebuffer framebuffer) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> guard(resourceMutex);
    framebuffers.erase(handleValue(framebuffer));
}

void renderPassCreated(
    VkRenderPass renderPass, std::uint32_t attachmentCount,
    const VkAttachmentDescription* attachments) {
    if (!enabled()) return;
    if (!renderPass || (attachmentCount && !attachments)) return;
    RenderPassInfo info{};
    info.finalLayouts.reserve(attachmentCount);
    info.loadOps.reserve(attachmentCount);
    for (std::uint32_t index = 0; index < attachmentCount; ++index) {
        info.finalLayouts.push_back(attachments[index].finalLayout);
        info.loadOps.push_back(attachments[index].loadOp);
    }
    std::lock_guard<std::mutex> guard(resourceMutex);
    renderPasses[handleValue(renderPass)] = std::move(info);
}

void renderPass2Created(
    VkRenderPass renderPass, std::uint32_t attachmentCount,
    const VkAttachmentDescription2* attachments) {
    if (!enabled()) return;
    if (!renderPass || (attachmentCount && !attachments)) return;
    RenderPassInfo info{};
    info.finalLayouts.reserve(attachmentCount);
    info.loadOps.reserve(attachmentCount);
    for (std::uint32_t index = 0; index < attachmentCount; ++index) {
        info.finalLayouts.push_back(attachments[index].finalLayout);
        info.loadOps.push_back(attachments[index].loadOp);
    }
    std::lock_guard<std::mutex> guard(resourceMutex);
    renderPasses[handleValue(renderPass)] = std::move(info);
}

void renderPassDestroyed(VkRenderPass renderPass) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> guard(resourceMutex);
    renderPasses.erase(handleValue(renderPass));
}

void graphicsPipelinesCreated(
    std::uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
    const VkPipeline* createdPipelines) {
    if (!enabled()) return;
    if (!infos || !createdPipelines) return;
    std::lock_guard<std::mutex> guard(resourceMutex);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (!createdPipelines[index]) continue;
        const auto& info = infos[index];
        PipelineInfo pipeline{};
        pipeline.renderPass = handleValue(info.renderPass);
        pipeline.subpass = info.subpass;
        pipeline.stageCount = info.stageCount;
        if (info.pInputAssemblyState)
            pipeline.topology = info.pInputAssemblyState->topology;
        if (info.pColorBlendState) {
            for (std::uint32_t attachment = 0;
                 attachment < info.pColorBlendState->attachmentCount; ++attachment) {
                const auto& blend = info.pColorBlendState->pAttachments[attachment];
                pipeline.blending = pipeline.blending || blend.blendEnable == VK_TRUE;
                pipeline.alphaWrite = pipeline.alphaWrite
                    || (blend.colorWriteMask & VK_COLOR_COMPONENT_A_BIT) != 0;
                pipeline.colorWrite = pipeline.colorWrite || blend.colorWriteMask != 0;
            }
        }
        if (info.pDepthStencilState) {
            pipeline.depthTest = info.pDepthStencilState->depthTestEnable == VK_TRUE;
            pipeline.depthWrite = info.pDepthStencilState->depthWriteEnable == VK_TRUE;
        }
        pipelines[handleValue(createdPipelines[index])] = pipeline;
    }
}

void pipelineDestroyed(VkPipeline pipeline) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> guard(resourceMutex);
    pipelines.erase(handleValue(pipeline));
}

void updateDescriptorSets(
    std::uint32_t writeCount, const VkWriteDescriptorSet* writes,
    std::uint32_t copyCount, const VkCopyDescriptorSet* copies) {
    if (!enabled()) return;
    if (captureComplete.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> guard(resourceMutex);
    for (std::uint32_t writeIndex = 0; writeIndex < writeCount; ++writeIndex) {
        const auto& write = writes[writeIndex];
        if (!write.dstSet || !isImageDescriptorType(write.descriptorType)
            || !write.pImageInfo) continue;
        auto& descriptorSet = descriptorSets[handleValue(write.dstSet)];
        for (std::uint32_t imageIndex = 0; imageIndex < write.descriptorCount; ++imageIndex) {
            const auto arrayElement = write.dstArrayElement + imageIndex;
            descriptorSet.images.erase(std::remove_if(
                descriptorSet.images.begin(), descriptorSet.images.end(),
                [&write, arrayElement](const DescriptorImageInfo& existing) {
                    return existing.binding == write.dstBinding
                        && existing.arrayElement == arrayElement;
                }), descriptorSet.images.end());
            const auto& source = write.pImageInfo[imageIndex];
            DescriptorImageInfo image{};
            image.binding = write.dstBinding;
            image.arrayElement = arrayElement;
            image.type = write.descriptorType;
            image.sampler = handleValue(source.sampler);
            image.view = handleValue(source.imageView);
            image.layout = source.imageLayout;
            const auto view = views.find(image.view);
            if (view != views.end()) image.image = view->second.image;
            const auto imageInfo = images.find(image.image);
            if (imageInfo != images.end()) {
                image.width = imageInfo->second.width;
                image.height = imageInfo->second.height;
                image.format = imageInfo->second.format;
                image.usage = imageInfo->second.usage;
            }
            descriptorSet.images.push_back(image);
        }
    }
    for (std::uint32_t copyIndex = 0; copyIndex < copyCount; ++copyIndex) {
        const auto& copy = copies[copyIndex];
        const auto sourceSet = descriptorSets.find(handleValue(copy.srcSet));
        if (sourceSet == descriptorSets.end() || !copy.dstSet) continue;
        std::vector<DescriptorImageInfo> copied;
        for (const auto& source : sourceSet->second.images) {
            if (source.binding != copy.srcBinding
                || source.arrayElement < copy.srcArrayElement
                || source.arrayElement >= copy.srcArrayElement + copy.descriptorCount) continue;
            auto destination = source;
            destination.binding = copy.dstBinding;
            destination.arrayElement = copy.dstArrayElement
                + source.arrayElement - copy.srcArrayElement;
            copied.push_back(destination);
        }
        auto& destinationSet = descriptorSets[handleValue(copy.dstSet)];
        for (const auto& destination : copied) {
            destinationSet.images.erase(std::remove_if(
                destinationSet.images.begin(), destinationSet.images.end(),
                [&destination](const DescriptorImageInfo& existing) {
                    return existing.binding == destination.binding
                        && existing.arrayElement == destination.arrayElement;
                }), destinationSet.images.end());
            destinationSet.images.push_back(destination);
        }
    }
}

void beginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* info) {
    if (!enabled()) return;
    if (captureComplete.load(std::memory_order_acquire) || !commandBuffer || !info) return;
    auto& state = commandStates[handleValue(commandBuffer)];
    finishPass(state, true);
    KharvoxHudDiagnosticSnapshot hud{};
    KharvoxHudGetDiagnosticSnapshot(hud);
    state.active = true;
    state.pass.commandBuffer = handleValue(commandBuffer);
    state.pass.renderPass = handleValue(info->renderPass);
    state.pass.framebuffer = handleValue(info->framebuffer);
    state.pass.width = info->renderArea.extent.width;
    state.pass.height = info->renderArea.extent.height;
    state.pass.eye = KharvoxCameraCurrentEye();
    state.pass.hudMatchedBegin = hud.matchedSurfaces;
    state.pass.attachments = lookupAttachments(state.pass.framebuffer);
    state.nextDrawOrdinal = 0;

    {
        std::lock_guard<std::mutex> guard(resourceMutex);
        const auto framebuffer = framebuffers.find(state.pass.framebuffer);
        if (framebuffer != framebuffers.end()) {
            if (!state.pass.width) state.pass.width = framebuffer->second.width;
            if (!state.pass.height) state.pass.height = framebuffer->second.height;
        }
    }

    if (info->pClearValues) {
        const auto count = std::min<std::size_t>(
            info->clearValueCount, state.pass.attachments.size());
        for (std::size_t index = 0; index < count; ++index) {
            if (state.pass.attachments[index].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR
                && formatHasAlpha(state.pass.attachments[index].format)
                && std::isfinite(info->pClearValues[index].color.float32[3])
                && std::fabs(info->pClearValues[index].color.float32[3]) < 0.0001f) {
                state.pass.transparentClear = true;
            }
        }
    }
}

void nextSubpass(VkCommandBuffer commandBuffer) {
    if (!enabled()) return;
    const auto iterator = commandStates.find(handleValue(commandBuffer));
    if (iterator != commandStates.end() && iterator->second.active)
        ++iterator->second.pass.subpasses;
}

void endRenderPass(VkCommandBuffer commandBuffer) {
    if (!enabled()) return;
    const auto iterator = commandStates.find(handleValue(commandBuffer));
    if (iterator != commandStates.end()) finishPass(iterator->second, false);
}

void bindPipeline(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
    if (!enabled()) return;
    if (captureComplete.load(std::memory_order_acquire)
        || bindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    auto& state = commandStates[handleValue(commandBuffer)];
    state.currentPipeline = handleValue(pipeline);
    state.currentPipelineInfo = lookupPipeline(state.currentPipeline, state.currentPipelineKnown);
}

void bindDescriptorSets(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint,
    std::uint32_t firstSet, std::uint32_t setCount, const VkDescriptorSet* sets) {
    if (!enabled()) return;
    if (captureComplete.load(std::memory_order_acquire)
        || bindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS || !sets) return;
    auto& state = commandStates[handleValue(commandBuffer)];
    for (std::uint32_t index = 0; index < setCount; ++index) {
        const auto setIndex = firstSet + index;
        if (setIndex >= state.boundDescriptorSets.size()) break;
        state.boundDescriptorSets[setIndex] = handleValue(sets[index]);
    }
    refreshBoundDescriptorImages(state);
}

void bindVertexBuffers(
    VkCommandBuffer commandBuffer, std::uint32_t firstBinding,
    std::uint32_t bindingCount, const VkBuffer* buffers, const VkDeviceSize* offsets) {
    if (!enabled()) return;
    if (captureComplete.load(std::memory_order_acquire) || !buffers) return;
    auto& state = commandStates[handleValue(commandBuffer)];
    for (std::uint32_t index = 0; index < bindingCount; ++index) {
        const auto binding = firstBinding + index;
        if (binding >= state.vertexBindings.size()) break;
        state.vertexBindings[binding] = {
            binding, handleValue(buffers[index]), offsets ? offsets[index] : 0};
    }
}

void bindIndexBuffer(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType) {
    if (!enabled()) return;
    if (captureComplete.load(std::memory_order_acquire)) return;
    auto& state = commandStates[handleValue(commandBuffer)];
    state.indexBuffer = handleValue(buffer);
    state.indexOffset = offset;
    state.indexType = indexType;
}

void setViewport(
    VkCommandBuffer commandBuffer, std::uint32_t count, const VkViewport* viewports) {
    if (!enabled()) return;
    if (!viewports || !count) return;
    const auto iterator = commandStates.find(handleValue(commandBuffer));
    if (iterator == commandStates.end() || !iterator->second.active) return;
    for (std::uint32_t index = 0; index < count; ++index) {
        iterator->second.pass.viewportWidth = std::max(
            iterator->second.pass.viewportWidth,
            static_cast<std::uint32_t>(std::lround(std::fabs(viewports[index].width))));
        iterator->second.pass.viewportHeight = std::max(
            iterator->second.pass.viewportHeight,
            static_cast<std::uint32_t>(std::lround(std::fabs(viewports[index].height))));
    }
}

void setScissor(
    VkCommandBuffer commandBuffer, std::uint32_t count, const VkRect2D* scissors) {
    if (!enabled()) return;
    if (!scissors || !count) return;
    const auto iterator = commandStates.find(handleValue(commandBuffer));
    if (iterator == commandStates.end() || !iterator->second.active) return;
    for (std::uint32_t index = 0; index < count; ++index) {
        iterator->second.pass.scissorWidth = std::max(
            iterator->second.pass.scissorWidth, scissors[index].extent.width);
        iterator->second.pass.scissorHeight = std::max(
            iterator->second.pass.scissorHeight, scissors[index].extent.height);
    }
}

void draw(
    VkCommandBuffer commandBuffer, DrawKind kind,
    std::uint64_t submittedDraws, std::uint64_t workItems) {
    if (!enabled()) return;
    if (captureComplete.load(std::memory_order_acquire)) return;
    const auto iterator = commandStates.find(handleValue(commandBuffer));
    if (iterator == commandStates.end() || !iterator->second.active) return;
    auto& state = iterator->second;
    auto& pass = state.pass;
    pass.draws += submittedDraws;
    pass.workItems += workItems;
    if (kind == DrawKind::Indirect || kind == DrawKind::IndexedIndirect)
        pass.indirectDraws += submittedDraws;
    else if (kind == DrawKind::Indexed)
        pass.indexedDraws += submittedDraws;
    else
        pass.directDraws += submittedDraws;
    if (state.currentPipelineKnown) {
        if (state.currentPipelineInfo.blending) pass.blendedDraws += submittedDraws;
        if (state.currentPipelineInfo.alphaWrite) pass.alphaWriteDraws += submittedDraws;
        if (state.currentPipelineInfo.colorWrite) pass.colorWriteDraws += submittedDraws;
        if (state.currentPipelineInfo.depthTest) pass.depthTestDraws += submittedDraws;
        if (state.currentPipelineInfo.depthWrite) pass.depthWriteDraws += submittedDraws;
    }
    auto pipeline = std::find_if(pass.pipelines.begin(), pass.pipelines.end(),
        [&state](const PipelineUse& use) { return use.pipeline == state.currentPipeline; });
    if (pipeline == pass.pipelines.end() && pass.pipelines.size() < 32) {
        pass.pipelines.push_back({state.currentPipeline, 0, 0, state.currentPipelineInfo});
        pipeline = std::prev(pass.pipelines.end());
    }
    if (pipeline != pass.pipelines.end()) {
        pipeline->draws += submittedDraws;
        pipeline->workItems += workItems;
    }
    const auto ordinal = ++state.nextDrawOrdinal;
    const auto signature = drawSignature(state, kind, pass);
    auto drawGroup = std::find_if(pass.drawGroups.begin(), pass.drawGroups.end(),
        [signature](const DrawGroup& group) { return group.signature == signature; });
    if (drawGroup == pass.drawGroups.end() && pass.drawGroups.size() < 1024) {
        DrawGroup group{};
        group.signature = signature;
        group.pipeline = state.currentPipeline;
        group.kind = kind;
        group.firstOrdinal = ordinal;
        group.lastOrdinal = ordinal;
        group.pipelineInfo = state.currentPipelineInfo;
        group.descriptorImages = state.boundDescriptorImages;
        for (const auto& vertex : state.vertexBindings)
            if (vertex.buffer) group.vertexBindings.push_back(vertex);
        group.indexBuffer = state.indexBuffer;
        group.indexOffset = state.indexOffset;
        group.indexType = state.indexType;
        pass.drawGroups.push_back(std::move(group));
        drawGroup = std::prev(pass.drawGroups.end());
    }
    if (drawGroup != pass.drawGroups.end()) {
        ++drawGroup->calls;
        drawGroup->submittedDraws += submittedDraws;
        drawGroup->workItems += workItems;
        drawGroup->lastOrdinal = ordinal;
    }
}

void present(std::uint64_t presentSerial) {
    if (!enabled()) return;
    if (captureComplete.load(std::memory_order_acquire)) return;
    KharvoxHudDiagnosticSnapshot hud{};
    KharvoxHudGetDiagnosticSnapshot(hud);
    std::vector<PassRecord> framePasses;
    {
        std::lock_guard<std::mutex> guard(completedMutex);
        framePasses.swap(completedPasses);
    }

    const auto previousMatched = lastMatchedAtPresent;
    const auto matchedDelta = hud.matchedSurfaces - previousMatched;
    const auto completedDelta = hud.completedSurfaces - lastCompletedAtPresent;
    const auto crosshairDelta = hud.crosshairSurfaces - lastCrosshairAtPresent;
    lastMatchedAtPresent = hud.matchedSurfaces;
    lastCompletedAtPresent = hud.completedSurfaces;
    lastCrosshairAtPresent = hud.crosshairSurfaces;
    if (!matchedDelta) return;

    std::array<KharvoxHudDiagnosticEvent, 256> hudEvents{};
    const auto hudEventCount = KharvoxHudCopyDiagnosticEvents(
        previousMatched + 1, hudEvents.data(), hudEvents.size());
    std::vector<std::uint64_t> frameHudProfiles;
    for (std::size_t eventIndex = 0; eventIndex < hudEventCount; ++eventIndex) {
        const auto& event = hudEvents[eventIndex];
        const auto profileSignature = hudProfileSignature(event);
        if (std::find(frameHudProfiles.begin(), frameHudProfiles.end(), profileSignature)
            == frameHudProfiles.end()) frameHudProfiles.push_back(profileSignature);
        const auto duplicate = std::find_if(
            observedHudProfiles.begin(), observedHudProfiles.end(),
            [&event](const KharvoxHudDiagnosticEvent& existing) {
                return existing.callerRva == event.callerRva
                    && existing.width == event.width
                    && existing.height == event.height
                    && existing.scaleMilli == event.scaleMilli
                    && existing.crosshair == event.crosshair;
            });
        if (duplicate != observedHudProfiles.end()) continue;
        observedHudProfiles.push_back(event);
        std::ostringstream profile;
        profile << "new HUD profile serial=" << event.serial
            << " caller=0x" << std::hex << event.callerRva << std::dec
            << " size=" << event.width << 'x' << event.height
            << " scaleMilli=" << event.scaleMilli
            << " crosshair=" << (event.crosshair ? 1 : 0);
        log(profile.str());
    }

    ++activeHudFrames;
    for (const auto profile : frameHudProfiles) ++hudProfileFrameUses[profile];
    for (const auto& pass : framePasses) aggregatePass(pass, frameHudProfiles);
    if (activeHudFrames <= 3 || isCaptureMilestone(activeHudFrames)) {
        std::ostringstream out;
        out << "frame active=" << activeHudFrames
            << " present=" << presentSerial
            << " cpuMatchedDelta=" << matchedDelta
            << " completedDelta=" << completedDelta
            << " crosshairDelta=" << crosshairDelta
            << " recordedPasses=" << framePasses.size();
        log(out.str());
    }
    if (isCaptureMilestone(activeHudFrames)) logCandidates(presentSerial, hud);
    if (activeHudFrames >= 1200) {
        captureComplete.store(true, std::memory_order_release);
        log("automatic capture complete after 1200 HUD-active presents; preserve KHARVOX.log for HUD-quad draw isolation");
    }
}

bool getQuadCandidate(QuadCandidate& candidate) {
    if (!enabled()) {
        candidate = {};
        return false;
    }
    std::lock_guard<std::mutex> guard(quadCandidateMutex);
    candidate = latestQuadCandidate;
    return candidate.image != VK_NULL_HANDLE && candidate.extent.width
        && candidate.extent.height && candidate.layout != VK_IMAGE_LAYOUT_UNDEFINED;
}

void deviceDestroyed() {
    if (!enabled()) return;
    {
        std::lock_guard<std::mutex> guard(resourceMutex);
        images.clear();
        views.clear();
        framebuffers.clear();
        renderPasses.clear();
        swapchains.clear();
        pipelines.clear();
        descriptorSets.clear();
    }
    {
        std::lock_guard<std::mutex> guard(completedMutex);
        completedPasses.clear();
        candidates.clear();
        drawCandidates.clear();
        observedHudProfiles.clear();
        hudProfileFrameUses.clear();
        lastMatchedAtPresent = 0;
        lastCompletedAtPresent = 0;
        lastCrosshairAtPresent = 0;
        activeHudFrames = 0;
    }
    captureComplete.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> guard(quadCandidateMutex);
        latestQuadCandidate = {};
    }
    quadCandidateSerial.store(0, std::memory_order_release);
    commandStates.clear();
}

} // namespace kharvox::hudgpu
