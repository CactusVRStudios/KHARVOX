#pragma once

#include "HandVisibilityPolicy.h"
#include "HandPoseMath.h"
#include "HandInteractionPolicy.h"
#include "HandSceneDepthTracker.h"
#include "../weapon/WeaponHook.h"
#include <vulkan/vulkan.h>
#include <array>
#include <functional>
#include <string>
#include <vector>

struct KharvoxVulkanDispatch;

namespace kharvox::hands {

struct HandGameplayState {
    KharvoxWeaponKind weapon{KharvoxWeaponKind::Unknown};
    bool leftHanded{};
    HandInteractionState interaction{};
    bool twoHanded{};
    std::array<math::Mat4,2> weaponJoints{};
    unsigned weaponJointMask{};
};

using HandLog = std::function<void(const std::string&)>;

class HandRenderer {
public:
    HandRenderer();
    ~HandRenderer();
    HandRenderer(const HandRenderer&) = delete;
    HandRenderer& operator=(const HandRenderer&) = delete;

    bool initialize(VkPhysicalDevice physicalDevice, VkDevice device,
        VkQueue queue, std::uint32_t queueFamily,
        const KharvoxVulkanDispatch& dispatch, VkFormat colorFormat,
        const std::array<VkExtent2D, 2>& eyeExtents,
        const std::array<std::vector<VkImage>, 2>& eyeImages,
        const std::wstring& runtimeDirectory, HandLog log);
    void shutdown();

    HandAssetAvailability availability() const;
    bool hasSupportProfile(KharvoxWeaponKind weapon,bool leftHanded) const;
    bool hasPrimaryProfile(KharvoxWeaponKind weapon) const;
    bool hasGrenadeVisualPose() const;
    bool authoredSupportGrip(KharvoxWeaponKind weapon,bool leftHanded,
        const math::Mat4& jointModel,std::array<float,3>& point) const;
    const HandCalibration& leftCalibration() const;
    const HandCalibration& rightCalibration() const;

    void record(VkCommandBuffer commandBuffer, std::uint32_t eye,
        std::uint32_t imageIndex, const HandEyeView& view,
        const HandPose& leftGrip, const HandPose& rightGrip,
        const HandVisibilityOutput& visibility,
        const HandGameplayState& gameplay);

    // Records into the completed native DOOM color image while borrowing its
    // depth attachment. Returns false without changing ownership when that
    // render target cannot be used safely, allowing the caller to retain the
    // isolated overlay fallback.
    bool recordSceneIntegrated(VkCommandBuffer commandBuffer,
        const HandSceneTarget& target, const HandEyeView& view,
        const HandPose& leftGrip, const HandPose& rightGrip,
        const HandVisibilityOutput& visibility,
        const HandGameplayState& gameplay, const HandPose& laser = {});
    void finishSceneIntegratedFrame();

private:
    struct Impl;
    Impl* impl_{};
};

} // namespace kharvox::hands
