#include "../src/openxr/OpenXRRuntimePolicy.h"
#include "../src/openxr/CinematicRefreshPolicy.h"
#include "../src/weapon/CollectiblePresentation.h"

int main() {
    for (int anim=-1; anim<=9; ++anim) {
        if (kharvox::collectibleAnimationPlaying(float(anim),0x80)
            != (anim>=6 && anim<=8)) return 210;
        // Remembered selection after animation end must not retain Quad.
        if (kharvox::collectibleAnimationPlaying(float(anim),0x7f)) return 211;
    }
    if (kharvox::collectibleAnimationPlaying(6.5f,0x80)) return 212;
    if (!kharvox::collectibleObservationCurrent(100,108,2,2)) return 213;
    if (kharvox::collectibleObservationCurrent(100,109,2,2)) return 214;
    if (kharvox::collectibleObservationCurrent(100,100,2,3)) return 215;
    if (kharvox::collectibleObservationCurrent(0,1,2,2)) return 216;
    if (kharvox::collectibleObservationCurrent(100,99,2,2)) return 217;
    // All centered menu owners (Praetor, Rune, VEGA, pause) must suppress
    // cinematic FOV/freelook even if the underlying world is still cinematic.
    if (kharvox::shouldArmImmersiveCamera(true,true,false)) return 201;
    if (kharvox::shouldArmImmersiveCamera(true,false,true)) return 202;
    if (!kharvox::shouldArmImmersiveCamera(true,false,false)) return 203;
    if (kharvox::shouldArmImmersiveCamera(false,false,false)) return 204;
    using namespace kharvox;

    if (classifyOpenXRRuntime("virtualdesktop-openxr.json") != OpenXRRuntimeKind::VirtualDesktop) return 1;
    if (classifyOpenXRRuntime("SteamVR") != OpenXRRuntimeKind::SteamVR) return 2;
    if (classifyOpenXRRuntime("meta_openxr") != OpenXRRuntimeKind::MetaOculus) return 3;
    if (classifyOpenXRRuntime("unrecognized") != OpenXRRuntimeKind::Unknown) return 4;

    if (classifyOpenXRRuntime("E:/VDXR4Steam/build/bin/Release/vdxr4steam.json")
        != OpenXRRuntimeKind::VDXR4Steam) return 101;
    if (classifyOpenXRRuntime("VDXR4Steam Runtime")
        != OpenXRRuntimeKind::VDXR4Steam) return 108;
    if (!isSteamBackedOpenXRRuntime(OpenXRRuntimeKind::VDXR4Steam)) return 102;
    if (selectOpenXRVulkanPath(OpenXRRuntimeKind::VDXR4Steam, false, true, true)
        != OpenXRVulkanPath::VulkanEnable2RuntimeManaged) return 103;
    if (!shouldPassthroughSteamRuntimeAuxiliary(
            OpenXRRuntimeKind::VDXR4Steam, true, "steamvr_vrclient_interop")) return 104;
    if (!shouldPassthroughSteamRuntimeAuxiliary(
            OpenXRRuntimeKind::VDXR4Steam, true, "unnamed_runtime_helper")) return 105;
    if (!shouldPassthroughSteamRuntimeAuxiliary(
            OpenXRRuntimeKind::Unknown, false, "steamvr_vrclient_interop")) return 106;
    if (shouldPassthroughSteamRuntimeAuxiliary(
            OpenXRRuntimeKind::Unknown, true, "unrelated_nested_application")) return 107;

    if (selectOpenXRVulkanPath(OpenXRRuntimeKind::SteamVR, false, true, true)
        != OpenXRVulkanPath::VulkanEnable2RuntimeManaged) return 5;
    if (selectOpenXRVulkanPath(OpenXRRuntimeKind::SteamVR, false, true, false)
        != OpenXRVulkanPath::VulkanEnable1Direct) return 6;
    if (selectOpenXRVulkanPath(OpenXRRuntimeKind::VirtualDesktop, true, true, true)
        != OpenXRVulkanPath::VulkanEnable2VirtualDesktopBridge) return 7;
    if (selectOpenXRVulkanPath(OpenXRRuntimeKind::Unknown, false, false, false)
        != OpenXRVulkanPath::None) return 8;

    if (!useCenteredProjectionFov(OpenXRRuntimeKind::VirtualDesktop)) return 127;
    if (useCenteredProjectionFov(OpenXRRuntimeKind::MetaOculus)) return 128;
    if (useCenteredProjectionFov(OpenXRRuntimeKind::SteamVR)) return 129;
    if (useCenteredProjectionFov(OpenXRRuntimeKind::VDXR4Steam)) return 130;
    const float centeredVertical = centeredProjectionHalfAngle(-0.91f, 0.72f);
    if (centeredVertical < 0.909f || centeredVertical > 0.911f) return 134;
    const float centeredHorizontal = centeredProjectionHalfAngle(-0.67f, 0.92f);
    if (centeredHorizontal < 0.919f || centeredHorizontal > 0.921f) return 135;
    // Recorded VDXR FOV: vertical -55/+44 and horizontal -54/+40,
    // -40/+54. Native must include the wider edges in both eyes.
    constexpr float radians=0.01745329252f;
    const auto nearAngle=[](float actual,float expected){return std::abs(actual-expected)<0.00001f;};
    if(!nearAngle(immersiveProjectionHalfAngle(OpenXRRuntimeKind::VirtualDesktop,
        true,-55*radians,44*radians,-55*radians,44*radians),55*radians))return 201;
    if(!nearAngle(immersiveProjectionHalfAngle(OpenXRRuntimeKind::VirtualDesktop,
        true,-54*radians,40*radians,-40*radians,54*radians),54*radians))return 202;
    if(!nearAngle(immersiveProjectionHalfAngle(OpenXRRuntimeKind::VirtualDesktop,
        false,-55*radians,44*radians,-55*radians,44*radians),49.5f*radians))return 203;
    for(auto runtime:{OpenXRRuntimeKind::SteamVR,OpenXRRuntimeKind::MetaOculus})
        if(!nearAngle(immersiveProjectionHalfAngle(runtime,false,
            -55*radians,44*radians,-57*radians,43*radians),57*radians))return 204;
    if (!shouldAutomaticallyFocusDoom(0)) return 131;
    if (!shouldAutomaticallyFocusDoom(119)) return 132;
    if (shouldAutomaticallyFocusDoom(120)) return 133;

    // A compatible swapchain replacement is protected only during the early
    // startup window and only after at least one real XR layer was submitted.
    if (!shouldArmEarlySwapchainCopyRecovery(true, 1, 1)) return 136;
    if (!shouldArmEarlySwapchainCopyRecovery(true, 3,
            earlySwapchainRecoveryMaxSubmittedFrames)) return 137;
    if (shouldArmEarlySwapchainCopyRecovery(false, 1, 1)) return 138;
    if (shouldArmEarlySwapchainCopyRecovery(true, 0, 1)) return 139;
    if (shouldArmEarlySwapchainCopyRecovery(true, 1, 0)) return 140;
    if (shouldArmEarlySwapchainCopyRecovery(true, 1,
            earlySwapchainRecoveryMaxSubmittedFrames + 1)) return 141;

    // SteamVR's Meta compatibility runtime must keep ordinary AER
    // stereoscopic. The exact same-frame path is reserved for a failed,
    // explicitly requested AFW initialization.
    if (useSteamLinkSameFrameMonoFallback(true, true, false, false, false)) return 9;
    if (useSteamLinkSameFrameMonoFallback(true, true, true, false, false)) return 10;
    if (useSteamLinkSameFrameMonoFallback(true, true, true, true, true)) return 11;
    if (!useSteamLinkSameFrameMonoFallback(true, true, true, true, false)) return 12;
    if (useSteamLinkSameFrameMonoFallback(false, true, true, true, false)) return 13;
    if (useSteamLinkSameFrameMonoFallback(true, false, true, true, false)) return 14;

    // SteamVR and direct Virtual Desktop share completed AER pairs.
    if (!useSteamXrCoherentAerPair(
            OpenXRRuntimeKind::SteamVR, true, true, false, false, false)) return 109;
    if (!useSteamXrCoherentAerPair(
            OpenXRRuntimeKind::VirtualDesktop, true, true, false, false, false)) return 110;
    if (useSteamXrCoherentAerPair(
            OpenXRRuntimeKind::VDXR4Steam, true, true, false, false, false)) return 111;
    if (useSteamXrCoherentAerPair(
            OpenXRRuntimeKind::SteamVR, true, true, false, true, false)) return 112;
    if (useSteamXrCoherentAerPair(
            OpenXRRuntimeKind::SteamVR, true, true, false, false, true)) return 113;
    if (!useSteamXrCoherentAerPair(
            OpenXRRuntimeKind::VirtualDesktop, true, true, false, false, false,
            true)) return 123;
    if (!useSteamXrCoherentAerPair(
            OpenXRRuntimeKind::VDXR4Steam, true, true, false, false, false,
            true)) return 124;
    if (!useSteamXrCoherentAerPair(
            OpenXRRuntimeKind::MetaOculus, true, true, false, false, false,
            true)) return 125;
    if (!useSteamXrCoherentAerPair(
            OpenXRRuntimeKind::VirtualDesktop, true, true, false, false, false,
            false)) return 126;
    for (int gate=0; gate<4; ++gate)
        if (useSteamXrCoherentAerPair(OpenXRRuntimeKind::VirtualDesktop,
                gate!=0, true, gate==1, gate==2, gate==3)) return 180+gate;
    if (!useSteamXrCoherentAerPair(OpenXRRuntimeKind::MetaOculus,
            true,true,false,false,false)) return 184;
    if (!useCoherentAerPairPresentationContext(
            true, false, false, false)) return 118;
    if (!useCoherentAerPairPresentationContext(
            false, true, false, false)) return 119;
    if (useCoherentAerPairPresentationContext(
            false, true, true, false)) return 120;
    if (useCoherentAerPairPresentationContext(
            false, true, false, true)) return 121;
    if (useCoherentAerPairPresentationContext(
            false, false, false, false)) return 122;
    const double pair90 = coherentAerFullPairHz(11'111'111);
    const double pair120 = coherentAerFullPairHz(8'333'333);
    const double pair144 = coherentAerFullPairHz(6'944'444);
    if (pair90 < 44.9 || pair90 > 45.1) return 114;
    if (pair120 < 59.9 || pair120 > 60.1) return 115;
    if (pair144 < 71.9 || pair144 > 72.1) return 116;
    if (coherentAerFullPairHz(0) != 0.0) return 117;

    // A native full-screen interface must force QUAD even if the underlying
    // gameplay camera still marks the frame as packed stereo.
    if (!selectQuadPresentation(
            false, false, true, false, false, true, false,
            true, false, false, false)) return 15;
    if (!selectQuadPresentation(
            false, false, true, false, false, true, false,
            false, true, false, false)) return 16;

    // Packed gameplay remains in projection when no full-screen UI owns it.
    if (selectQuadPresentation(
            false, false, true, false, false, true, false,
            false, false, false, false)) return 17;
    if (!selectQuadPresentation(
            false, false, false, true, false, true, false,
            false, false, false, false)) return 18;
    if (selectQuadPresentation(
            false, false, false, true, true, true, false,
            false, false, false, false)) return 19;

    // A Projection -> Quad switch first neutralizes the previously programmed
    // eye camera and lets the three-Present DOOM pipeline retire. The Quad is
    // immediately visible as black; its source is released only at centre-eye.
    if (!shouldBeginCenteredQuadTransition(true, false, false)) return 20;
    if (shouldBeginCenteredQuadTransition(true, true, false)) return 21;
    if (shouldBeginCenteredQuadTransition(true, false, true)) return 22;
    if (!shouldCancelCenteredQuadTransition(false, true)) return 23;
    if (shouldCancelCenteredQuadTransition(true, true)) return 24;
    if (!shouldSuppressStereoForCenteredQuadTransition(true)) return 25;
    if (shouldSuppressStereoForCenteredQuadTransition(false)) return 26;
    unsigned remaining = centeredQuadPipelineSettleFrames;
    remaining = advanceCenteredQuadTransition(remaining);
    if (remaining != 2) return 27;
    remaining = advanceCenteredQuadTransition(remaining);
    remaining = advanceCenteredQuadTransition(remaining);
    if (remaining != 0) return 28;
    if (advanceCenteredQuadTransition(remaining) != 0) return 29;

    // All complete-frame Cinewindows use the same two-metre geometry as the
    // Main Menu, including Praetor/Argent and End-of-Level content.
    if (fullFrameQuadDistanceMeters(false) != 2.0f) return 30;
    if (fullFrameQuadDistanceMeters(true) != 2.0f) return 31;
    if (fullFrameQuadWidthMeters(false) != 2.0f) return 32;
    if (fullFrameQuadWidthMeters(true) != 2.0f) return 33;
    if (std::abs(fullFrameQuadHeightMeters(2.0f,2496,2688)-2.1538462f)>0.00001f) return 133;
    if (fullFrameQuadHeightMeters(2.0f,1920,1080)!=1.125f) return 134;
    if (fullFrameQuadHeightMeters(2.0f,0,0)!=1.125f) return 135;
    if (fullFrameQuadCroppedHeight(2496,2688)!=1404) return 136;
    if (fullFrameQuadCroppedHeight(1920,1080)!=1080) return 137;
    if (fullFrameQuadCroppedHeight(1920,800)!=800) return 138;

    // The Praetor pickup remains reported as an immersive cinematic by the
    // game, but its manager-owned native UI must still force the full frame to
    // Quad just as Field Drone does.
    if (!selectQuadPresentation(
            false, false, true, true, true, true, false,
            true, false, false, false)) return 34;

    // The optional selective Immersive mode sends ordinary cinematics to the
    // existing Quad path, but retains native first-person assists such as a
    // Glory Kill sync attack or a Jump/Ledge pull in Projection.
    if (!shouldKeepCinematicImmersive(
            true, false, true, true, false, false, false, false)) return 35;
    if (shouldKeepCinematicImmersive(
            true, true, true, true, false, false, false, false)) return 36;
    if (!shouldKeepCinematicImmersive(
            true, true, true, true, false, false, true, false)) return 37;
    if (shouldKeepCinematicImmersive(
            true, true, false, true, false, false, true, false)) return 38;
    if (shouldKeepCinematicImmersive(
            true, true, true, true, true, false, true, false)) return 39;
    if (shouldKeepCinematicImmersive(
            false, true, true, true, false, false, true, false)) return 40;

    // Tutorial overlays and videos never enter the selective Quad route. The
    // exception remains latched for their complete cinematic lifetime.
    if (!shouldKeepCinematicImmersive(
            true, true, true, true, false, false, false, true)) return 41;

    // A centered Projection -> Quad transition is exposed immediately as a
    // black Quad; the composed source is released only after the delayed game
    // camera pipeline has reached the centered pose.
    if (!shouldBlackoutCenteredQuadTransition(true, true)) return 42;
    if (shouldBlackoutCenteredQuadTransition(false, true)) return 43;
    if (shouldBlackoutCenteredQuadTransition(true, false)) return 44;

    // Tutorial activity suppresses all automatic Quad causes, including a
    // temporarily missing/interrupted world camera. An unrelated native
    // full-screen menu still retains its higher-priority Quad ownership.
    if (selectQuadPresentation(
            false, false, false, true, false, false, true,
            false, false, false, true)) return 45;
    if (!selectQuadPresentation(
            false, false, false, true, false, false, true,
            true, false, false, true)) return 46;

    // A level/checkpoint generation can expose a transient playable camera
    // between two loading phases. Keep AER suppressed through the complete
    // timed settle window; zero and expired deadlines do not suppress it.
    if (!shouldSuppressStereoForTimedTransition(1200, 4000)) return 47;
    if (shouldSuppressStereoForTimedTransition(4001, 4000)) return 48;
    if (shouldSuppressStereoForTimedTransition(1200, 0)) return 49;

    // Immersive cinematics must retain the headset cadence instead of letting
    // a missed interval teach the policy that a 120 Hz headset is 60 Hz.
    auto fastestPeriod = selectFastestPlausibleDisplayPeriod(0, 8'333'333);
    if (fastestPeriod != 8'333'333) return 50;
    fastestPeriod = selectFastestPlausibleDisplayPeriod(fastestPeriod, 16'666'667);
    if (fastestPeriod != 8'333'333) return 51;
    fastestPeriod = selectFastestPlausibleDisplayPeriod(fastestPeriod, 6'944'444);
    if (fastestPeriod != 6'944'444) return 52;
    if (selectFastestPlausibleDisplayPeriod(fastestPeriod, 33'333'333)
        != fastestPeriod) return 53;
    if (targetGameHzForDisplayPeriod(8'333'333) != 120) return 54;
    if (targetGameHzForDisplayPeriod(11'111'111) != 90) return 55;
    if (targetGameHzForDisplayPeriod(13'888'889) != 72) return 56;
    if (targetGameHzForDisplayPeriod(16'666'667) != 60) return 57;
    if (targetGameHzForDisplayPeriod(4'000'000) != 200) return 58;
    if (!shouldHoldImmersiveRefresh(true, false, true, false, false)) return 59;
    if (!shouldHoldImmersiveRefresh(true, false, false, true, false)) return 60;
    if (shouldHoldImmersiveRefresh(true, true, true, true, true)) return 61;
    if (shouldHoldImmersiveRefresh(false, false, true, true, true)) return 62;
    if (!shouldHoldImmersiveRefresh(true, false, false, false, true)) return 63;
    if (updateNativeAdaptiveParticipantGuardUntil(
            true, true, 1000, 0, 500) != 1500) return 64;
    if (updateNativeAdaptiveParticipantGuardUntil(
            true, false, 1200, 1500, 500) != 1500) return 65;
    if (updateNativeAdaptiveParticipantGuardUntil(
            true, false, 1501, 1500, 500) != 0) return 66;
    if (updateNativeAdaptiveParticipantGuardUntil(
            false, true, 1000, 1500, 500) != 0) return 67;
    if (!nativeAdaptiveParticipantGuardIsActive(1500, 1500)) return 68;
    if (nativeAdaptiveParticipantGuardIsActive(1501, 1500)) return 69;
    if (updateNativeAdaptiveParticipantGuardUntil(
            true, true, ~0ull - 100, 0, 500) != ~0ull) return 70;

    // In Comfort mode only a participant with a known, tracked weapon and
    // DOOM-confirmed live aim/button input stays in stereoscopic Projection.
    // A scripted weapon by itself (VEGA) remains a centered Cinewindow.
    if (!shouldKeepNativeParticipantInComfortProjection(
            false, true, false, true, true, true)) return 71;
    if (shouldKeepNativeParticipantInComfortProjection(
            false, true, false, false, true, true)) return 72;
    if (shouldKeepNativeParticipantInComfortProjection(
            false, true, false, true, false, true)) return 73;
    if (shouldKeepNativeParticipantInComfortProjection(
            false, true, false, true, true, false)) return 74;
    if (shouldKeepNativeParticipantInComfortProjection(
            true, true, false, true, true, true)) return 75;
    if (shouldKeepNativeParticipantInComfortProjection(
            false, true, true, true, true, true)) return 76;
    if (!shouldRouteNativeParticipantToComfortQuad(
            false, true, false, false)) return 77;
    if (shouldRouteNativeParticipantToComfortQuad(
            false, true, false, true)) return 78;
    if (shouldRouteNativeParticipantToComfortQuad(
            true, true, false, false)) return 79;
    if (shouldRouteNativeParticipantToComfortQuad(
            false, false, false, false)) return 80;
    if (shouldRouteNativeParticipantToComfortQuad(
            false, true, true, false)) return 81;

    return 0;
}
