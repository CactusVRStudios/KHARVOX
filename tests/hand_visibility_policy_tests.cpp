#include "../src/hands/HandVisibilityPolicy.h"
#include "../src/hands/HandSceneDrawPolicy.h"
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    using namespace kharvox::hands;
    const HandAssetAvailability all{true, true, true, true};

    auto output = selectHandVisibility(
        {false, false, false, all, true, true});
    require(output.left == HandModelKind::Fist
        && output.right == HandModelKind::GunHolding,
        "right-handed mode uses free left fist and right gun pose");

    output = selectHandVisibility({true, false, false, all, true, true});
    require(output.left == HandModelKind::GunHolding
        && output.right == HandModelKind::Fist,
        "left-handed mode mirrors weapon and free-hand poses");

    output = selectHandVisibility({false, true, false, all, true, true});
    require(output.left == HandModelKind::GunHolding
        && output.right == HandModelKind::GunHolding,
        "right-handed support grip displays both gun poses");

    output = selectHandVisibility({true, true, false, all, true, true});
    require(output.left == HandModelKind::GunHolding
        && output.right == HandModelKind::GunHolding,
        "left-handed support grip displays both gun poses");

    for (bool leftHanded : {false, true}) {
        for (bool grabbed : {false, true}) {
            for (bool show : {false, true}) {
                for (bool gameplay : {false, true}) {
                    output = selectHandVisibility(
                        {leftHanded, grabbed, false, all, show, gameplay});
                    const auto support = grabbed
                        ? HandModelKind::GunHolding : HandModelKind::Fist;
                    require(output.left == (!show || !gameplay ? HandModelKind::None
                        : leftHanded ? HandModelKind::GunHolding : support),
                        "left hand respects grip, handedness and presentation");
                    require(output.right == (!show || !gameplay ? HandModelKind::None
                        : leftHanded ? support : HandModelKind::GunHolding),
                        "right hand respects grip, handedness and presentation");
                }
            }
        }
    }

    output = selectHandVisibility({false, true, false,
        {true, true, false, true}, true, true});
    require(output.left == HandModelKind::None
        && output.right == HandModelKind::GunHolding
        && output.preserveNativeOffHand,
        "missing support gun asset retains native fallback without a fist overlap");
    output = selectHandVisibility({true, true, false,
        {true, true, true, false}, true, true});
    require(output.left == HandModelKind::GunHolding
        && output.right == HandModelKind::None && output.preserveNativeOffHand,
        "left-handed missing support gun asset retains native fallback");

    output = selectHandVisibility({false, true, true, all, true, true});
    require(output.left == HandModelKind::Fist
        && output.right == HandModelKind::Fist,
        "berserk overrides handedness and two-hand grab");

    output = selectHandVisibility({false, false, false, all, false, true});
    require(output.left == HandModelKind::None
        && output.right == HandModelKind::None,
        "Show Hands defaults to an inactive renderer policy");

    output = selectHandVisibility({false, false, false, all, true, false});
    require(output.left == HandModelKind::None
        && output.right == HandModelKind::None,
        "cinematics suppress both custom hands");
    output = selectHandVisibility({false, true, true, all, true, false});
    require(output.left == HandModelKind::None
        && output.right == HandModelKind::None,
        "cinematic suppression has priority over berserk and two-hand grab");

    output = selectHandVisibility({false, false, false,
        {true, false, false, true}, true, true});
    require(output.left == HandModelKind::Fist
        && output.right == HandModelKind::GunHolding,
        "missing unrelated pose assets do not hide available models");

    output = selectHandVisibility({false, false, false,
        {false, true, true, true}, true, true});
    require(output.left == HandModelKind::None
        && output.preserveNativeOffHand,
        "missing free-hand fist retains the native off-hand fallback");

    HandPose tracked{};tracked.valid=true;
    const HandPose lost{},noLaser{};
    for(bool leftHanded:{false,true}){
        const auto both=selectHandVisibility({leftHanded,true,false,all,true,true});
        require(handSceneHasGeometry(both,tracked,tracked,noLaser),
            "two tracked hands keep scene integration in either handedness mode");
        require(handSceneHasGeometry(both,tracked,lost,noLaser)
            &&handSceneHasGeometry(both,lost,tracked,noLaser),
            "losing either controller must retain the other tracked hand");
        require(!handSceneHasGeometry(both,lost,lost,noLaser),
            "losing both controllers must skip borrowing scene attachments");

        const auto missing=selectHandVisibility({leftHanded,true,false,{},true,true});
        require(!handSceneHasGeometry(missing,tracked,tracked,noLaser),
            "native fallback with no custom assets needs no custom scene pass");
        require(handSceneHasGeometry(missing,lost,lost,tracked),
            "laser-only integration must survive absent hand assets and tracking");
    }
    const auto leftOnly=selectHandVisibility({false,false,false,
        {true,false,false,false},true,true});
    require(handSceneHasGeometry(leftOnly,tracked,lost,noLaser)
        &&!handSceneHasGeometry(leftOnly,lost,tracked,noLaser),
        "a tracked controller cannot substitute for the missing opposite-hand model");
    const auto rightOnly=selectHandVisibility({true,false,false,
        {false,true,false,false},true,true});
    require(handSceneHasGeometry(rightOnly,lost,tracked,noLaser)
        &&!handSceneHasGeometry(rightOnly,tracked,lost,noLaser),
        "right-hand-only availability follows that controller's tracking");
    for(bool allowPresentation:{false,true}){
        const auto hidden=selectHandVisibility({false,false,false,all,false,allowPresentation});
        require(!handSceneHasGeometry(hidden,tracked,tracked,noLaser),
            "hiding hands must skip their pass even when controllers remain tracked");
        require(handSceneHasGeometry(hidden,tracked,tracked,tracked),
            "the Show Hands option must not disable a separately requested laser");
    }
    const auto cinematic=selectHandVisibility({false,true,false,all,true,false});
    require(!handSceneHasGeometry(cinematic,tracked,tracked,noLaser),
        "cinematic visibility must suppress a stale tracked hand pose");
    return 0;
}
