#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <cstdint>

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
}

int main(int argc, char** argv) {
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
    // Freeze ALL accepted 0.5 Beta vectors (global wrists and both physical hands
    // for all 14 weapon keys), not just one representative weapon. Comments
    // and platform newline conventions do not change this regression check.
    if (calibrationFingerprint(config / "hand_models_calibration_default.cfg")
        != 0xaa353e68u) return 18;
    if (calibrationFingerprint(config / "hud_flat_calibration_default.cfg")
        != 0x8bc58407u) return 19;
    if (calibrationFingerprint(config / "hud_profile_calibration_default.cfg")
        != 0x59b52852u) return 20;
    if (!readScalar(config / "hud_quad_scale_default.cfg", 16.75f)) return 21;
    return 0;
}
