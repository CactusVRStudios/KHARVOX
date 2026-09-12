#include "../src/hands/HandCalibrationPolicy.h"

#include <cassert>

int main() {
    using namespace kharvox::hands;

    bool plusWasDown{};
    auto mode=updateHandCalibrationMode(CalibrationMode::Rotation,true,plusWasDown);
    assert(mode==CalibrationMode::Position);
    mode=updateHandCalibrationMode(mode,true,plusWasDown);
    assert(mode==CalibrationMode::Position); // held key / second eye
    mode=updateHandCalibrationMode(mode,false,plusWasDown);
    mode=updateHandCalibrationMode(mode,true,plusWasDown);
    assert(mode==CalibrationMode::Rotation);
    plusWasDown=false;
    assert(updateHandCalibrationMode(CalibrationMode::None,true,plusWasDown)
        ==CalibrationMode::None);

    auto profile = selectHandCalibrationProfile(false, false,
        KharvoxWeaponKind::Shotgun, true);
    assert(profile.weaponSpecific && profile.handIndex == 1u);
    assert(profile.weaponIndex ==
        static_cast<std::size_t>(KharvoxWeaponKind::Shotgun));

    profile = selectHandCalibrationProfile(true, true,
        KharvoxWeaponKind::GaussCannon, true);
    assert(profile.weaponSpecific && profile.handIndex == 0u);

    profile = selectHandCalibrationProfile(true, false,
        KharvoxWeaponKind::Shotgun, false);
    assert(!profile.weaponSpecific && profile.handIndex == 0u);

    profile = selectHandCalibrationProfile(false, false,
        KharvoxWeaponKind::Unknown, true);
    assert(!profile.weaponSpecific);
    return 0;
}
