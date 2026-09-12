#pragma once

#include <cstdint>

namespace kharvox {

enum class HudGuiSurfaceKind {
    Other,
    DossierMenu,
    FieldDronePrompt,
    RuneTrialChallengeMenu
};

inline HudGuiSurfaceKind classifyHudGuiSurface(
    std::uintptr_t callerRva, int width, int height, int scaleMilli) {
    const bool fullScreenDimensions = width >= 1280 && height >= 720;

    // The Dossier uses a strongly enlarged native GUI-list surface.
    if (callerRva == 0xBAA703
        && fullScreenDimensions
        && scaleMilli >= 1000)
        return HudGuiSurfaceKind::DossierMenu;

    // This surface is the interaction prompt visible near a Field Drone. It
    // identifies proximity only; the prompt must never open a Quad by itself.
    if (callerRva == 0xC83FEE
        && fullScreenDimensions
        && scaleMilli >= 900
        && scaleMilli <= 1100)
        return HudGuiSurfaceKind::FieldDronePrompt;

    // The Start / Exit / Retry screen inside a Rune Trial is rendered by the
    // EndOfChallenge HUD at 1024x1024. Unlike the terminal interaction it has
    // no introductory cinematic, so this exact surface is the render fallback
    // if the native screen Show callback is bypassed during a map handoff.
    if (callerRva == 0xBCEA7D
        && width == 1024 && height == 1024
        && scaleMilli >= 950 && scaleMilli <= 1050)
        return HudGuiSurfaceKind::RuneTrialChallengeMenu;

    return HudGuiSurfaceKind::Other;
}

inline bool isFullscreenMenuSurface(HudGuiSurfaceKind kind) {
    return kind == HudGuiSurfaceKind::DossierMenu;
}

inline bool shouldKeepNativeMenuSession(
    bool nativeSessionActive,
    std::uint64_t now, std::uint64_t safetyDeadline) {
    return nativeSessionActive && safetyDeadline && now <= safetyDeadline;
}

inline bool shouldKeepPulsedMenuSession(
    std::uint64_t lastFrameTick, std::uint64_t now,
    std::uint64_t frameHoldMilliseconds) {
    // A manager which owns a rendered native menu is called every frame. A
    // short heartbeat covers the gap between the final manager frame and the
    // next OpenXR Present without treating proximity or a controller button as
    // menu lifecycle evidence.
    return lastFrameTick && now >= lastFrameTick
        && now - lastFrameTick <= frameHoldMilliseconds;
}

enum class PlayerUpgradePickupPhase {
    WaitingForCinematicOrMenu,
    ImmersiveCinematic,
    CenteredMenuFallback,
    Complete
};

inline PlayerUpgradePickupPhase selectPlayerUpgradePickupPhase(
    bool nativeSessionActive, bool cinematicObserved, bool cinematicActive,
    bool gameplayActive, std::uint64_t now, std::uint64_t sessionStart,
    std::uint64_t entryGraceMilliseconds, std::uint64_t safetyDeadline,
    std::uint64_t pickupConfirmedTick = 0) {
    // Elite Guard activation happens before the pickup animation, while the
    // interactive Praetor menu has its own manager/screen lifecycle. Merely
    // arming the pickup must therefore not center the preceding cinematic.
    // The pending grace covers the frame gap before DOOM marks the cinematic.
    if (!nativeSessionActive || !sessionStart || !safetyDeadline
        || now > safetyDeadline) return PlayerUpgradePickupPhase::Complete;
    // The award notification can arrive while the interactive screen still
    // uses the cinematic caller. Do not wait for that caller to disappear.
    // Release on actual gameplay return, with only an entry-frame grace.
    if (pickupConfirmedTick && now >= pickupConfirmedTick)
        return cinematicActive || !gameplayActive
            || now - pickupConfirmedTick <= entryGraceMilliseconds
            ? PlayerUpgradePickupPhase::CenteredMenuFallback
            : PlayerUpgradePickupPhase::Complete;
    if (cinematicActive) return PlayerUpgradePickupPhase::ImmersiveCinematic;
    if (!cinematicObserved && now - sessionStart <= entryGraceMilliseconds)
        return PlayerUpgradePickupPhase::WaitingForCinematicOrMenu;

    // Exact PlayerUpgrade manager/screen hooks normally own the menu. This is
    // a fail-safe for campaign variants which bypass them: after the cinematic
    // (or after the entry grace when no cinematic is reported), only a real
    // non-gameplay state may request the centered full-frame presentation.
    return gameplayActive
        ? PlayerUpgradePickupPhase::Complete
        : PlayerUpgradePickupPhase::CenteredMenuFallback;
}

inline bool shouldKeepCinematicMenuSession(
    bool nativeSessionActive, bool cinematicActive, bool gameplayActive,
    std::uint64_t now, std::uint64_t sessionStart,
    std::uint64_t entryGraceMilliseconds, std::uint64_t safetyDeadline) {
    // The picked-up Praetor menu stays on DOOM's cinematic camera path and
    // bypasses the PlayerUpgrade screen/manager callbacks. Its exact HUD
    // notification opens the session. Keep it through the short introductory
    // camera and the following interactive menu, then release only after the
    // normal gameplay camera has really returned. The grace period covers the
    // frame in which the notification precedes cinematic camera detection.
    if (!nativeSessionActive || !sessionStart || !safetyDeadline
        || now > safetyDeadline) return false;
    return now - sessionStart <= entryGraceMilliseconds
        || cinematicActive || !gameplayActive;
}

inline bool shouldKeepEndOfLevelMenuSession(
    bool nativeSessionActive,
    std::uint64_t generationAtShow, std::uint64_t currentGeneration,
    bool gameplayActive) {
    // The world camera can disappear a few frames after the EOL screen opens,
    // which advances the level generation during the menu itself. Keep the
    // native screen latched throughout that non-gameplay interval. A changed
    // generation becomes a safety exit only after the next playable world is
    // confirmed, covering map loads that bypass HideScreen.
    return nativeSessionActive
        && (generationAtShow == currentGeneration || !gameplayActive);
}

inline bool shouldKeepRuneTrialMenuSession(
    bool nativeSessionActive, bool challengeLoadPending,
    std::uint64_t generationAtActivation,
    std::uint64_t currentGeneration, bool gameplayActive,
    std::uint64_t now, std::uint64_t safetyDeadline) {
    (void)challengeLoadPending;
    (void)generationAtActivation;
    (void)currentGeneration;
    (void)gameplayActive;
    if (!nativeSessionActive || !safetyDeadline || now > safetyDeadline)
        return false;

    // A playable camera exists behind the destination's Start / Exit screen,
    // so a generation change plus gameplay is not a valid release signal.
    // Exact RunePopup Exit, native release, or the EndOfChallenge destination
    // screen must explicitly transfer/release ownership. The deadline remains
    // only an abnormal-session safety net.
    return true;
}

inline bool shouldAcceptRuneMenuAnimationEvent(bool activationSessionActive) {
    // ae_showRuneMenu is also replayed while restoring some campaign saves,
    // even when no interactive Rune menu owns the player. It may refresh an
    // exact activation-owned session but must never create one by itself.
    return activationSessionActive;
}

inline bool shouldReleaseRunePopupSelection(
    bool activationSessionActive, bool challengeLoadPending,
    int selectionIndex) {
    // idMenuWidget_RunePopup creates Start/Accept as item 0 and Exit as item
    // 1. Accept is intentionally retained until VegaTraining::Use owns the
    // nextmap handoff; only the exact Exit item releases the terminal session.
    return activationSessionActive && !challengeLoadPending
        && selectionIndex == 1;
}

inline bool shouldReleaseRuneChallengeAction(
    bool challengeScreenActive, bool handled,
    int actionType, int commandIndex, bool startCommitted) {
    // idMenuScreen_Gui_EndOfChallenge::HandleAction_Impl receives command
    // actions as type 1. The class creates Start as command 0, Retry as command
    // 1, and Exit as command 2; commands 3/4 cover its remaining closing UI
    // actions. Retry and Exit both tear down a world: Retry reloads the current
    // challenge, while Exit returns to the campaign. They must retain Quad and
    // input ownership until a replacement generation is stable. Start and the
    // remaining handled commands can release immediately.
    return challengeScreenActive && handled && actionType == 1
        && commandIndex >= 0 && commandIndex <= 4
        && commandIndex != 1 && commandIndex != 2
        && (commandIndex != 0 || startCommitted);
}

inline bool shouldArmRuneChallengeMapLoadAction(
    bool challengeScreenActive, int actionType, int commandIndex) {
    return challengeScreenActive && actionType == 1
        && (commandIndex == 1 || commandIndex == 2);
}

inline bool shouldRetainRuneChallengeMapLoadAction(
    bool challengeScreenActive, bool handled,
    int actionType, int commandIndex) {
    return handled && shouldArmRuneChallengeMapLoadAction(
        challengeScreenActive, actionType, commandIndex);
}

inline bool shouldRetainRuneChallengeMapLoadAfterHide(
    bool challengeScreenActive, bool mapLoadPending,
    bool replacementMenuActive) {
    // Some engine paths may hide the outgoing EndOfChallenge screen inside
    // HandleAction before that function returns. Keep the latch through this
    // synchronous teardown unless an exact replacement menu already owns the
    // full-frame UI.
    return challengeScreenActive && mapLoadPending && !replacementMenuActive;
}

inline bool shouldTransferRuneChallengeMapLoadToDestinationScreen(
    bool challengeScreenActive, bool mapLoadPending) {
    // Retry loads another EndOfChallenge Start/Exit screen before the restarted
    // trial begins. Its exact ShowScreen becomes the new owner; keep Quad/UI
    // active, but stop treating ordinary gameplay behind that menu as the
    // completion of a destination-less map load. Exit normally transfers to
    // RuneSelect instead and therefore reaches the stable-gameplay release.
    return challengeScreenActive && mapLoadPending;
}

inline bool shouldReleaseRuneChallengeMapLoad(
    bool challengeScreenActive, bool mapLoadPending,
    std::uint64_t generationAtLoad,
    std::uint64_t currentGeneration, bool gameplayActive) {
    // A generation change alone happens while the old world and native camera
    // references are still being replaced. Wait for the camera hook to confirm
    // ordinary gameplay in that new generation before Projection/AER resume.
    return challengeScreenActive && mapLoadPending
        && generationAtLoad != currentGeneration && gameplayActive;
}

inline bool shouldReleaseRuneChallengeCheckpointRecovery(
    bool challengeScreenActive, bool pauseRecoveryArmed,
    std::uint64_t generationAtPause,
    std::uint64_t currentGeneration, bool gameplayActive) {
    // EndOfChallenge can Show before the accepted Challenge map finishes its
    // own generation change, so a generation change after Show is not an exit.
    // A changed generation becomes a recovery signal only when the player has
    // opened Pause while this screen owns input (the Load Checkpoint path) and
    // the replacement level has reached a playable camera again.
    return challengeScreenActive && pauseRecoveryArmed
        && generationAtPause != currentGeneration && gameplayActive;
}

inline std::uint32_t screenMaskAfterHide(
    std::uint32_t activeMask, std::uint32_t screenBit, int transitionType,
    bool transitionOneCloses = false) {
    const bool closes = transitionType == 2
        || (transitionOneCloses && transitionType == 1);
    return closes ? activeMask & ~screenBit : activeMask;
}

inline bool shouldPublishGameCameraHeadPose(bool centeredNativeMenuActive) {
    return !centeredNativeMenuActive;
}

inline bool shouldObserveCinematicMenuSurface(bool cinematicActive,
    bool crosshair, bool ledgeTransition, bool nativeMenuSession) {
    return cinematicActive && !crosshair && !ledgeTransition && nativeMenuSession;
}

inline bool shouldApplyHudTransform(
    bool gameplayHudActive, bool crosshair, bool ledgeTransitionActive) {
    // DOOM can submit its exact gameplay crosshair on the boundary of a
    // cinematic camera state even after the native ledge state has changed.
    // Always admit that identified surface. While DOOM's exact native ledge
    // transition is active, admit every recognized gameplay-HUD surface so it
    // can share the same stable off-screen placement.
    return gameplayHudActive || crosshair || ledgeTransitionActive;
}

inline bool shouldPlaceHudOffscreen(
    bool crosshair, bool ledgeTransitionActive) {
    return crosshair || ledgeTransitionActive;
}

inline bool shouldCenterNativeMenuGameCamera(
    bool centeredNativeMenuActive, bool centeredPoseAlreadyHeld) {
    return centeredNativeMenuActive && !centeredPoseAlreadyHeld;
}

inline const char* fullscreenGuiKindName(HudGuiSurfaceKind kind) {
    return kind == HudGuiSurfaceKind::DossierMenu ? "Dossier" : "unknown";
}

} // namespace kharvox
