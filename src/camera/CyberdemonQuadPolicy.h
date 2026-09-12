#pragma once
#include <string_view>
#include <cstdint>
namespace kharvox {
// Exact sequence identity: current sync master or native checkpoint-restored
// sync reference. Never the cached mostRecentSyncEntity or the map name.
inline bool isCyberdemonQuadSequence(std::string_view name) {
    return name=="zion/syncmelee/cyberdemon"
        ||name=="cyber_sync_res_checkpoint"
        ||name=="cyber_syncanims_intro_checkpoint"
        ||name=="cyber_zion_syncanims_cineractive_maps_lazarus_labs_cyberdemon_intro_sync11_cine_idsync_1";
}
// Exact Hell Guards player sequences observed in the running blood_keep_c map.
// AI collaboration, ordinary enemy kills and map/entity names are not selectors.
inline bool isHellGuardsQuadSequence(std::string_view name) {
    return name=="zion/syncmelee/talismanguard"
        ||name=="boss_syncanims_combo_guard_cine_checkpoint"
        ||name=="boss_syncanims_intro_twins_checkpoint"
        ||name=="boss_zion_syncanims_cineractive_maps_bloodkeep_c_talisman_guard_intro_sync11_combo_guard_cine_idsync_1"
        ||name=="boss_zion_syncanims_cineractive_maps_bloodkeep_c_talisman_guard_intro_sync11_twins_cine_idsync_1";
}
// Camera-only map sequences can run without the player being a sync participant.
inline bool isHellGuardsCameraSequence(std::string_view name) {
    return name=="boss_zion_syncanims_cineractive_maps_bloodkeep_c_talisman_guard_intro_sync11_combo_guard_cine_idsync_1"
        ||name=="boss_zion_syncanims_cineractive_maps_bloodkeep_c_talisman_guard_intro_sync11_twins_cine_idsync_1";
}
inline bool isActiveHellGuardsCameraSequence(std::string_view name,unsigned char working) {
    return working==1&&isHellGuardsCameraSequence(name);
}
inline bool isBossQuadSequence(std::string_view name) {
    return isCyberdemonQuadSequence(name)||isHellGuardsQuadSequence(name);
}
enum class CyberdemonSequenceObservation { Unknown, Inactive, Active };
inline bool shouldResetBossMapCameraScope(uintptr_t cachedOwner,uintptr_t cachedGame,
    uintptr_t owner,uintptr_t game,bool validGame) {
    // DOOM temporarily clears its global game pointer between update phases.
    // Only a positively identified replacement player/game invalidates a scope.
    return validGame&&owner&&(cachedOwner!=owner||cachedGame!=game);
}
inline CyberdemonSequenceObservation combineBossSequenceObservations(
    CyberdemonSequenceObservation player,CyberdemonSequenceObservation map) {
    using O=CyberdemonSequenceObservation;
    if(player==O::Active||map==O::Active)return O::Active;
    if(player==O::Unknown||map==O::Unknown)return O::Unknown;
    return O::Inactive;
}
// A failed concurrent read is not proof that an active sequence ended.
// Explicit inactivity always releases immediately; unknown data is bounded.
class CyberdemonSequenceHold {
    bool active_{};
    uint64_t present_{},level_{};
public:
    bool update(CyberdemonSequenceObservation observation,uint64_t present,uint64_t level,bool cinematic){
        if(observation==CyberdemonSequenceObservation::Active){
            active_=true;present_=present;level_=level;
        }else if(observation==CyberdemonSequenceObservation::Inactive
            ||!cinematic||level!=level_||present<present_||present-present_>3){
            active_=false;
        }
        return active_;
    }
};
}
