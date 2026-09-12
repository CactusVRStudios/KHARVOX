#include "../src/camera/CyberdemonQuadPolicy.h"
#include "../src/camera/BodyAnchorPolicy.h"
#include <cstdlib>
void check(bool ok){if(!ok)std::abort();}
int main(){
    using O=kharvox::CyberdemonSequenceObservation;
    kharvox::CyberdemonSequenceHold hold;
    check(!hold.update(O::Unknown,1,1,true));
    check(hold.update(O::Active,2,1,false)); // Recognize before first camera frame.
    check(hold.update(O::Unknown,4,1,true)); // Brief unavailable player-list snapshot.
    check(!hold.update(O::Unknown,6,1,true)); // Cannot persist without renewed evidence.
    check(hold.update(O::Active,7,1,true));
    check(!hold.update(O::Inactive,7,1,true)); // Other/finished sequence releases immediately.
    check(hold.update(O::Active,8,1,true));
    check(!hold.update(O::Unknown,8,2,true)); // Loading clears old generation.
    check(hold.update(O::Active,9,2,true)); // Reacquire the new native player while physics is invalid.
    check(!hold.update(O::Unknown,10,2,false)); // Do not drag the override into gameplay.
    using kharvox::mayCalibrateBodyAnchor;
    check(mayCalibrateBodyAnchor(true,false,true,false));
    check(!mayCalibrateBodyAnchor(false,false,true,false)); // Foreign camera produced the bad persistent offset.
    check(!mayCalibrateBodyAnchor(true,true,true,false)); // Authored movement is not stance calibration.
    check(!mayCalibrateBodyAnchor(true,false,false,false)); // Loading/non-playable entry.
    check(!mayCalibrateBodyAnchor(true,false,true,true)); // Restored boss sequence still owns the camera.
    using kharvox::isCyberdemonQuadSequence;
    check(isCyberdemonQuadSequence("zion/syncmelee/cyberdemon"));
    check(isCyberdemonQuadSequence("cyber_sync_res_checkpoint"));
    check(isCyberdemonQuadSequence("cyber_syncanims_intro_checkpoint"));
    check(isCyberdemonQuadSequence("cyber_zion_syncanims_cineractive_maps_lazarus_labs_cyberdemon_intro_sync11_cine_idsync_1"));
    for(auto name:{"", "game/sp/lazarus_2/lazarus_2", "CP_13_MID_CD",
        "zion/syncmelee/mancubus_cyber", "zion/syncmelee/imp",
        "zion/syncmelee/playerdeath/revenant", "cyber_resurrection_timeline",
        "cyber_ai_demon_cyberdemon_2", "cyber_interact_panels_cyberdemon_door_panel_1",
        "cyber_sync_unrelated", "zion/syncmelee/cyberdemon_other"})
        check(!isCyberdemonQuadSequence(name));
    for(auto name:{"zion/syncmelee/talismanguard",
        "boss_syncanims_combo_guard_cine_checkpoint", "boss_syncanims_intro_twins_checkpoint",
        "boss_zion_syncanims_cineractive_maps_bloodkeep_c_talisman_guard_intro_sync11_combo_guard_cine_idsync_1",
        "boss_zion_syncanims_cineractive_maps_bloodkeep_c_talisman_guard_intro_sync11_twins_cine_idsync_1"}) {
        check(kharvox::isHellGuardsQuadSequence(name));
        check(kharvox::isBossQuadSequence(name));
    }
    check(kharvox::isBossQuadSequence("zion/syncmelee/cyberdemon"));
    for(auto name:{"game/sp/blood_keep_c/blood_keep_c", "boss_start",
        "zion/synccollab/aionai/oliviasguard/on_talismanguard",
        "boss_ai_demon_talismanguard_combo_2", "interact/syncentity/elite_guard",
        "zion/syncmelee/hellknight", "zion/syncmelee/talismanguard_other",
        "boss_syncanims_intro_twins_checkpoint_other"})
        check(!kharvox::isBossQuadSequence(name));

    const auto twins="boss_zion_syncanims_cineractive_maps_bloodkeep_c_talisman_guard_intro_sync11_twins_cine_idsync_1";
    check(kharvox::isActiveHellGuardsCameraSequence(twins,1));
    check(!kharvox::isActiveHellGuardsCameraSequence(twins,0));
    check(!kharvox::isActiveHellGuardsCameraSequence(twins,255));
    check(!kharvox::isActiveHellGuardsCameraSequence("zion/syncmelee/talismanguard",1));
    check(!kharvox::isActiveHellGuardsCameraSequence("boss_ai_demon_talismanguard_hammer_2",1));

    using kharvox::shouldResetBossMapCameraScope;
    check(!shouldResetBossMapCameraScope(10,20,0,0,false));
    check(!shouldResetBossMapCameraScope(10,20,0,30,true));
    check(!shouldResetBossMapCameraScope(10,20,10,20,true));
    check(shouldResetBossMapCameraScope(10,20,11,20,true));
    check(shouldResetBossMapCameraScope(10,20,10,30,true));
    using kharvox::combineBossSequenceObservations;
    kharvox::CyberdemonSequenceHold mapHold;
    // Replay the observed alternating valid/null global game context. The
    // independent native map entity remains active for the whole cinematic.
    for(uint64_t frame=0;frame<600;++frame){
        const auto player=frame%2?O::Unknown:O::Inactive;
        check(mapHold.update(combineBossSequenceObservations(player,O::Active),frame,1,false));
    }
    check(!mapHold.update(combineBossSequenceObservations(O::Inactive,O::Inactive),600,1,true));
    check(combineBossSequenceObservations(O::Inactive,O::Unknown)==O::Unknown);
    check(combineBossSequenceObservations(O::Active,O::Unknown)==O::Active);

}
