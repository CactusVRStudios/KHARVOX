#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <cstdint>
#include "../src/hud/OffhandHudPolicy.h"

namespace {
std::uint32_t calibrationFingerprint(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::uint32_t hash = 2166136261u;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        for (unsigned char value : line + '\n') hash = (hash ^ value) * 16777619u;
    }
    return hash;
}
bool readScalar(const std::filesystem::path& path, float expected) {
    std::ifstream input(path);
    float value{};
    return input >> value && std::fabs(value - expected) < 0.0001f;
}
bool originalPraetorHud(const std::filesystem::path& path) {
    std::ifstream input(path);
    kharvox::OffhandHudConfig hud;
    if(!kharvox::readOffhandHudConfig(input,hud)||!hud.enabled||hud.watchReading
        ||std::fabs(hud.armourGapCm-2.f)>.0001f)return false;
    // Original six modes: Life/Ammo/ProgMeter, physical left/right. Restore
    // every offset, angle and scale. Clearance is perpendicular to the native
    // pixel plane; adding Grip-local Z does not guarantee escaping the armor.
    constexpr float original[6][7]{
        {-9.5f,-5,24.5f,-65,90,15,.4f},{-10,-14,0,-75,-65,20,.4f},
        {-4.5f,20.5f,17.5f,-65,60,15,.4f},{-13,12.5f,11,-55,-100,0,.4f},
        {-11.5f,7.5f,18,-130,95,-50,.4f},{-18,-1,6,-85,-65,30,.4f}};
    for(int i=0;i<6;++i){
        const auto& mode=hud.modes[i];
        for(int j=0;j<3;++j){
            if(std::fabs(mode.centimeters[j]-original[i][j])>.0001f)return false;
            if(std::fabs(mode.degrees[j]-original[i][j+3])>.0001f)return false;
        }
        if(std::fabs(mode.scale-original[i][6])>.0001f)return false;
    }
    return true;
}
}

int main(int argc, char** argv) {
    if(argc==3&&std::string(argv[1])=="--original-praetor-hud")
        return originalPraetorHud(argv[2])?0:22;
    if (argc != 2) return 1;
    const std::filesystem::path config(argv[1]);
    if (!readScalar(config / "weapon_yaw_default.cfg", -8.0f)) return 2;
    if (!readScalar(config / "weapon_roll_default.cfg", -1.0f)) return 3;
    if (!readScalar(config / "weapon_pitch_default.cfg", -3.0f)) return 4;

    std::ifstream profiles(config / "two_hand_weapon_profiles_default.cfg");
    std::set<std::string> weapons;
    int version{};
    std::string weapon, mode;
    float x{}, y{}, z{}, radius{}, maximumWeight{};
    while (profiles >> version >> weapon >> mode >> x >> y >> z >> radius >> maximumWeight) {
        if (version != 2) return 5;
        if (mode != "barrel" && mode != "side-grip") return 6;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return 7;
        if (std::fabs(radius - 0.2f) > 0.0001f || std::fabs(maximumWeight - 1.0f) > 0.0001f) return 8;
        if (!weapons.insert(weapon).second) return 9;
    }
    const std::set<std::string> expected{
        "shotgun", "heavy_assault_rifle", "plasma_rifle", "rocket_launcher",
        "super_shotgun", "gauss_cannon", "chaingun", "bfg", "chainsaw"
    };
    if (weapons != expected) return 10;

    std::ifstream handModels(config / "hand_models.cfg");
    std::unordered_map<std::string, std::string> handValues;
    std::string line;
    while (std::getline(handModels, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto equals = line.find('=');
        if (equals != std::string::npos)
            handValues[line.substr(0, equals)] = line.substr(equals + 1);
    }
    if (handValues["left_fist"] != "assets/3D/DOOM_LEFT_HAND_FIST.glb")
        return 11;
    if (handValues["right_fist"] != "assets/3D/DOOM_RIGHT_HAND_FIST.glb")
        return 12;
    if (handValues["left_gun"] != "assets/3D/DOOM_LEFT_HAND_GUN.glb"
        || handValues["right_gun"] != "assets/3D/DOOM_RIGHT_HAND_GUN.glb")
        return 13;
    if (handValues["left_scale"] != "0.2464"
        || handValues["right_scale"] != "0.2464") return 14;
    if (handValues["left_rotation"] != "0.000 180.000 0.000"
        || handValues["right_rotation"] != "0.000 180.000 0.000") return 15;
    if (handValues["version"] != "3") return 16;
    if (handValues["left_fist_mirror_x"] != "0"
        || handValues["right_fist_mirror_x"] != "0"
        || handValues["left_gun_mirror_x"] != "0"
        || handValues["right_gun_mirror_x"] != "0") return 17;
    // Freeze ALL accepted 1.0 / Test38 vectors (global wrists and both physical hands
    // for all 14 weapon keys), not just one representative weapon. Comments
    // and platform newline conventions do not change this regression check.
    if (calibrationFingerprint(config / "hand_models_calibration_default.cfg")
        != 0x8ef3e620u) return 18;
    if (calibrationFingerprint(config / "hud_flat_calibration_default.cfg")
        != 0x8bc58407u) return 19;
    if (calibrationFingerprint(config / "hud_profile_calibration_default.cfg")
        != 0x59b52852u) return 20;
    if (!readScalar(config / "hud_quad_scale_default.cfg", 16.75f)) return 21;
    if (!originalPraetorHud(config / "offhand_hud_default.cfg")) return 22;
    return 0;
}
