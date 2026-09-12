#include "../src/hud/HudMenuPolicy.h"

int main() {
    if(!kharvox::shouldObserveCinematicMenuSurface(true,false,false,true))return 141;
    if(kharvox::shouldObserveCinematicMenuSurface(true,false,false,false))return 142;
    if(kharvox::shouldObserveCinematicMenuSurface(true,true,false,true))return 143;
    if(kharvox::shouldObserveCinematicMenuSurface(false,false,false,true))return 144;

    using kharvox::HudGuiSurfaceKind;
    using kharvox::classifyHudGuiSurface;
    using kharvox::isFullscreenMenuSurface;
    using kharvox::shouldCenterNativeMenuGameCamera;
    using kharvox::shouldKeepCinematicMenuSession;
    using kharvox::shouldKeepEndOfLevelMenuSession;
    using kharvox::shouldKeepNativeMenuSession;
    using kharvox::shouldKeepPulsedMenuSession;
    using kharvox::PlayerUpgradePickupPhase;
    using kharvox::selectPlayerUpgradePickupPhase;
    using kharvox::shouldKeepRuneTrialMenuSession;
    using kharvox::shouldAcceptRuneMenuAnimationEvent;
    using kharvox::shouldReleaseRunePopupSelection;
    using kharvox::shouldReleaseRuneChallengeAction;
    using kharvox::shouldReleaseRuneChallengeCheckpointRecovery;
    using kharvox::shouldRetainRuneChallengeMapLoadAction;
    using kharvox::shouldArmRuneChallengeMapLoadAction;
    using kharvox::shouldRetainRuneChallengeMapLoadAfterHide;
    using kharvox::shouldTransferRuneChallengeMapLoadToDestinationScreen;
    using kharvox::shouldReleaseRuneChallengeMapLoad;
    using kharvox::shouldApplyHudTransform;
    using kharvox::shouldPlaceHudOffscreen;
    using kharvox::shouldPublishGameCameraHeadPose;
    using kharvox::screenMaskAfterHide;

    // Preserve the established Dossier full-screen classification.
    if (classifyHudGuiSurface(0xBAA703, 1920, 1080, 10000)
        != HudGuiSurfaceKind::DossierMenu) return 1;
    if (classifyHudGuiSurface(0xBAA703, 1024, 576, 10000)
        != HudGuiSurfaceKind::Other) return 2;
    if (!isFullscreenMenuSurface(HudGuiSurfaceKind::DossierMenu)) return 3;

    // The observed Field Drone surface is a proximity prompt. It never owns
    // Quad/UI routing; the native WeaponModBot activator does.
    if (classifyHudGuiSurface(0xC83FEE, 1920, 1080, 1000)
        != HudGuiSurfaceKind::FieldDronePrompt) return 4;
    if (classifyHudGuiSurface(0xC83FEE, 2560, 1440, 980)
        != HudGuiSurfaceKind::FieldDronePrompt) return 5;
    if (isFullscreenMenuSurface(HudGuiSurfaceKind::FieldDronePrompt)) return 6;
    // The proximity surface is deliberately not a lifecycle signal. The
    // native WeaponModBot activator owns both entry and exit, while the
    // deadline remains only an abnormal-session safety net.
    if (!shouldKeepNativeMenuSession(true, 4000, 5000)) return 7;
    if (shouldKeepNativeMenuSession(false, 4000, 5000)) return 8;
    if (shouldKeepNativeMenuSession(true, 5001, 5000)) return 9;

    // The Quad remains head-locked. On the first active frame the underlying
    // game camera receives one neutral pose, then retains that centered pose.
    if (shouldPublishGameCameraHeadPose(true)) return 10;
    if (!shouldPublishGameCameraHeadPose(false)) return 11;
    if (!shouldCenterNativeMenuGameCamera(true, false)) return 12;
    if (shouldCenterNativeMenuGameCamera(true, true)) return 13;
    if (shouldCenterNativeMenuGameCamera(false, false)) return 14;

    // Partial surfaces, scale mismatches and the observed wall-terminal call
    // sites remain outside the full-screen UI policy.
    if (classifyHudGuiSurface(0xC83FEE, 1024, 576, 1000)
        != HudGuiSurfaceKind::Other) return 15;
    if (classifyHudGuiSurface(0xC83FEE, 1920, 1080, 1200)
        != HudGuiSurfaceKind::Other) return 16;
    if (classifyHudGuiSurface(0x6B6F75, 1920, 1080, 1000)
        != HudGuiSurfaceKind::Other) return 17;
    if (classifyHudGuiSurface(0x907FA0, 1920, 1080, 1000)
        != HudGuiSurfaceKind::Other) return 18;

    // End-of-Level is owned by its native Show/Hide lifecycle. Losing the old
    // world while the screen remains open must not release Quad. If a direct
    // map load skips HideScreen, confirmed gameplay in the new generation is
    // the safety release.
    if (!shouldKeepEndOfLevelMenuSession(true, 7, 7, true)) return 19;
    if (!shouldKeepEndOfLevelMenuSession(true, 7, 8, false)) return 20;
    if (shouldKeepEndOfLevelMenuSession(true, 7, 8, true)) return 21;
    if (shouldKeepEndOfLevelMenuSession(false, 7, 7, false)) return 22;

    // Alternative outer/Dossier screens may overlap. Closing one concrete
    // screen must retain the deathscreen-style session until the last active
    // bit closes; internal transition 0/1 never clears a bit.
    if (screenMaskAfterHide(0b011u, 0b001u, 1) != 0b011u) return 23;
    if (screenMaskAfterHide(0b011u, 0b001u, 2) != 0b010u) return 24;
    if (screenMaskAfterHide(0b010u, 0b010u, 2) != 0u) return 25;
    // Argent/VEGA uses transition 1 to finish the accepted upgrade rather
    // than to open a child page. Its concrete screen bit must close there so
    // the following Immersive cinematic can return to Projection.
    if (screenMaskAfterHide(0b011u, 0b001u, 1, true) != 0b010u) return 37;

    // PlayerUpgrade's manager owns the actually rendered Praetor pickup menu
    // even though its child ShowScreen callbacks are bypassed. Consecutive
    // manager frames keep one session; stopping those frames releases it.
    if (!shouldKeepPulsedMenuSession(1000, 1200, 250)) return 26;
    if (!shouldKeepPulsedMenuSession(1000, 1250, 250)) return 27;
    if (shouldKeepPulsedMenuSession(1000, 1251, 250)) return 28;
    if (shouldKeepPulsedMenuSession(0, 1200, 250)) return 29;
    if (shouldKeepPulsedMenuSession(1300, 1200, 250)) return 30;

    // Elite Guard activation only arms a pending transition. The preceding
    // pickup animation remains immersive; the actual PlayerUpgrade manager
    // owns centered Quad. A non-gameplay fallback covers variants which skip
    // the exact callbacks, and normal gameplay/timeout ends the pending state.
    if (selectPlayerUpgradePickupPhase(
            true, false, false, true, 1100, 1000, 250, 5000)
        != PlayerUpgradePickupPhase::WaitingForCinematicOrMenu) return 31;
    if (selectPlayerUpgradePickupPhase(
            true, true, true, true, 2000, 1000, 250, 5000)
        != PlayerUpgradePickupPhase::ImmersiveCinematic) return 32;
    if (selectPlayerUpgradePickupPhase(
            true, true, false, false, 2000, 1000, 250, 5000)
        != PlayerUpgradePickupPhase::CenteredMenuFallback) return 33;
    if (selectPlayerUpgradePickupPhase(
            true, true, false, true, 2000, 1000, 250, 5000)
        != PlayerUpgradePickupPhase::Complete) return 34;
    if (selectPlayerUpgradePickupPhase(
            true, false, false, false, 2000, 1000, 250, 5000)
        != PlayerUpgradePickupPhase::CenteredMenuFallback) return 35;
    if (selectPlayerUpgradePickupPhase(
            true, true, true, false, 5001, 1000, 250, 5000)
        != PlayerUpgradePickupPhase::Complete) return 36;

    // Confirmed pickup keeps Quad even when DOOM reports a cinematic and
    // a held gameplay pose simultaneously; activation alone stays immersive.
    if (selectPlayerUpgradePickupPhase(true,true,true,true,2500,1000,250,5000,2000)
        != PlayerUpgradePickupPhase::CenteredMenuFallback) return 131;
    if (selectPlayerUpgradePickupPhase(true,true,false,true,2100,1000,250,5000,2000)
        != PlayerUpgradePickupPhase::CenteredMenuFallback) return 132;
    if (selectPlayerUpgradePickupPhase(true,true,false,true,2500,1000,250,5000,2000)
        != PlayerUpgradePickupPhase::Complete) return 133;
    if (selectPlayerUpgradePickupPhase(true,true,true,true,5001,1000,250,5000,2000)
        != PlayerUpgradePickupPhase::Complete) return 134;
    if (selectPlayerUpgradePickupPhase(false,true,true,true,2500,1000,250,5000,2000)
        != PlayerUpgradePickupPhase::Complete) return 135;

    // VEGA's accepted-upgrade refresh trigger remains active for a conservative
    // minimum even if the generic camera flag reports gameplay too early.
    // A real cinematic/non-gameplay tail extends it, bounded by the deadline.
    if (!shouldKeepCinematicMenuSession(
            true, false, true, 8999, 1000, 8000, 31000)) return 38;
    if (shouldKeepCinematicMenuSession(
            true, false, true, 9001, 1000, 8000, 31000)) return 39;
    if (!shouldKeepCinematicMenuSession(
            true, true, true, 12000, 1000, 8000, 31000)) return 40;
    if (shouldKeepCinematicMenuSession(
            true, true, true, 31001, 1000, 8000, 31000)) return 41;

    // Ordinary HUD transformation remains gameplay-only. The exact captured
    // crosshair always reaches its hard off-screen transform. During DOOM's
    // exact native Ledge transition every recognized HUD surface shares that
    // placement; unrelated cinematics remain excluded.
    if (!shouldApplyHudTransform(true, false, false)) return 42;
    if (!shouldApplyHudTransform(true, true, false)) return 43;
    if (shouldApplyHudTransform(false, false, false)) return 44;
    if (!shouldApplyHudTransform(false, true, false)) return 45;
    if (!shouldApplyHudTransform(false, false, true)) return 46;
    if (shouldPlaceHudOffscreen(false, false)) return 47;
    if (!shouldPlaceHudOffscreen(true, false)) return 48;
    if (!shouldPlaceHudOffscreen(false, true)) return 49;

    // Rune Trial ownership is explicit. A playable camera behind the new
    // level's Start / Exit screen must not release the accepted-load bridge.
    if (!shouldKeepRuneTrialMenuSession(
            true, false, 4, 4, true, 2000, 5000)) return 50;
    if (shouldKeepRuneTrialMenuSession(
            false, false, 4, 4, true, 2000, 5000)) return 51;
    if (!shouldKeepRuneTrialMenuSession(
            true, true, 4, 5, false, 2000, 5000)) return 52;
    if (!shouldKeepRuneTrialMenuSession(
            true, true, 4, 5, true, 2000, 5000)) return 53;
    if (!shouldKeepRuneTrialMenuSession(
            true, true, 4, 4, true, 2000, 5000)) return 54;
    if (shouldKeepRuneTrialMenuSession(
            true, true, 4, 5, false, 5001, 5000)) return 55;
    if (!shouldAcceptRuneMenuAnimationEvent(true)) return 56;
    if (shouldAcceptRuneMenuAnimationEvent(false)) return 57;
    if (classifyHudGuiSurface(0xBCEA7D, 1024, 1024, 1000)
        != HudGuiSurfaceKind::RuneTrialChallengeMenu) return 58;
    if (classifyHudGuiSurface(0xBCEA7D, 1920, 1080, 1000)
        != HudGuiSurfaceKind::Other) return 59;
    if (isFullscreenMenuSurface(
            HudGuiSurfaceKind::RuneTrialChallengeMenu)) return 60;
    if (!shouldReleaseRunePopupSelection(true, false, 1)) return 61;
    if (shouldReleaseRunePopupSelection(true, false, 0)) return 62;
    if (shouldReleaseRunePopupSelection(true, true, 1)) return 63;
    if (shouldReleaseRunePopupSelection(false, false, 1)) return 64;
    // A rapid second A press is consumed (true) before DOOM commits Start.
    // Repeated consumed presses must keep Quad and controller menu ownership.
    for (int press = 0; press < 3; ++press)
        if (shouldReleaseRuneChallengeAction(true, true, 1, 0, false)) return 110;
    if (!shouldReleaseRuneChallengeAction(true, true, 1, 0, true)) return 65;
    if (shouldReleaseRuneChallengeAction(true, true, 1, 1, true)) return 66;
    if (shouldReleaseRuneChallengeAction(true, true, 1, 2, true)) return 67;
    if (!shouldReleaseRuneChallengeAction(true, true, 1, 4, true)) return 68;
    if (shouldReleaseRuneChallengeAction(true, false, 1, 2, true)) return 69;
    if (shouldReleaseRuneChallengeAction(false, true, 1, 2, true)) return 70;
    if (shouldReleaseRuneChallengeAction(true, true, 0, 2, true)) return 71;
    if (shouldReleaseRuneChallengeAction(true, true, 1, 5, true)) return 72;
    if (shouldReleaseRuneChallengeCheckpointRecovery(
            true, false, 4, 5, true)) return 73;
    if (shouldReleaseRuneChallengeCheckpointRecovery(
            true, true, 4, 4, true)) return 74;
    if (shouldReleaseRuneChallengeCheckpointRecovery(
            true, true, 4, 5, false)) return 75;
    if (!shouldReleaseRuneChallengeCheckpointRecovery(
            true, true, 4, 5, true)) return 76;
    if (shouldReleaseRuneChallengeCheckpointRecovery(
            false, true, 4, 5, true)) return 77;
    if (screenMaskAfterHide(0b1000u, 0b1000u, 1, true) != 0u) return 78;
    if (screenMaskAfterHide(0b1000u, 0b1000u, 1, false) != 0b1000u) return 79;
    if (!shouldRetainRuneChallengeMapLoadAction(true, true, 1, 1)) return 80;
    if (!shouldRetainRuneChallengeMapLoadAction(true, true, 1, 2)) return 93;
    if (shouldRetainRuneChallengeMapLoadAction(true, false, 1, 1)) return 81;
    if (shouldRetainRuneChallengeMapLoadAction(false, true, 1, 2)) return 82;
    if (shouldRetainRuneChallengeMapLoadAction(true, true, 0, 1)) return 83;
    if (shouldRetainRuneChallengeMapLoadAction(true, true, 1, 0)) return 84;
    if (shouldReleaseRuneChallengeMapLoad(true, true, 9, 9, true)) return 85;
    if (shouldReleaseRuneChallengeMapLoad(true, true, 9, 10, false)) return 86;
    if (!shouldReleaseRuneChallengeMapLoad(true, true, 9, 10, true)) return 87;
    if (shouldReleaseRuneChallengeMapLoad(true, false, 9, 10, true)) return 88;
    if (shouldReleaseRuneChallengeMapLoad(false, true, 9, 10, true)) return 89;
    if (!shouldTransferRuneChallengeMapLoadToDestinationScreen(true, true)) return 90;
    if (shouldTransferRuneChallengeMapLoadToDestinationScreen(true, false)) return 91;
    if (shouldTransferRuneChallengeMapLoadToDestinationScreen(false, true)) return 92;
    if (!shouldArmRuneChallengeMapLoadAction(true, 1, 1)) return 94;
    if (!shouldArmRuneChallengeMapLoadAction(true, 1, 2)) return 95;
    if (shouldArmRuneChallengeMapLoadAction(true, 1, 0)) return 96;
    if (shouldArmRuneChallengeMapLoadAction(false, 1, 2)) return 97;
    if (!shouldRetainRuneChallengeMapLoadAfterHide(true, true, false)) return 98;
    if (shouldRetainRuneChallengeMapLoadAfterHide(true, true, true)) return 99;
    if (shouldRetainRuneChallengeMapLoadAfterHide(true, false, false)) return 100;

    return 0;
}
