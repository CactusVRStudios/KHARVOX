#include "../src/openxr/MovementDirectionPolicy.h"

#include <cmath>

namespace {
bool near(float left, float right) {
    return std::abs(left - right) < 0.001f;
}
}

int main() {
    using namespace kharvox;

    if (offHandForMovement(false) != MovementDirectionHand::Left) return 1;
    if (offHandForMovement(true) != MovementDirectionHand::Right) return 2;

    auto yaw = resolveMovementDirectionYaw(
        MovementDirectionMode::Head, 25.0f, true, -1.0f, 0.0f, 0.0f, 0.0f);
    if (yaw.usedOffHand || !near(yaw.degrees, 25.0f)) return 3;

    yaw = resolveMovementDirectionYaw(
        MovementDirectionMode::OffHand, 10.0f, true, 0.0f, -1.0f, 0.0f, 0.0f);
    if (!yaw.usedOffHand || !near(yaw.degrees, 0.0f)) return 4;

    yaw = resolveMovementDirectionYaw(
        MovementDirectionMode::OffHand, 10.0f, true, -1.0f, 0.0f, 30.0f, 10.0f);
    if (!yaw.usedOffHand || !near(yaw.degrees, 70.0f)) return 5;

    const auto rotated = rotateMovementStickForDirection({0.0f, 1.0f}, 90.0f);
    if (!near(rotated.x, -1.0f) || !near(rotated.y, 0.0f)) return 6;

    yaw = resolveMovementDirectionYaw(
        MovementDirectionMode::OffHand, -15.0f, false, -1.0f, 0.0f, 0.0f, 0.0f);
    if (yaw.usedOffHand || !near(yaw.degrees, -15.0f)) return 7;
    yaw = resolveMovementDirectionYaw(
        MovementDirectionMode::OffHand, -15.0f, true, 0.01f, 0.01f, 0.0f, 0.0f);
    if (yaw.usedOffHand || !near(yaw.degrees, -15.0f)) return 8;
    yaw = resolveMovementDirectionYaw(
        MovementDirectionMode::OffHand, -15.0f, true, NAN, -1.0f, 0.0f, 0.0f);
    if (yaw.usedOffHand || !near(yaw.degrees, -15.0f)) return 9;

    for(float angle:{0.f,45.f,90.f,180.f,270.f}){
        for(float radius:{0.f,.2f,.5f,.7f,.9f,1.f}){
            auto stick=rotateMovementStickForDirection({0,radius},angle);
            auto fixed=fullTravelMovementStick(stick);
            float expected=radius<=.5f?radius:std::min(1.f,.5f+(radius-.5f)*1.25f);
            if(!near(std::hypot(fixed.x,fixed.y),expected))return 10;
            if(!near(stick.x*fixed.y-stick.y*fixed.x,0))return 11;
        }
    }
    for(int x:{-32768,-1,0,1,32767})for(int y:{-32768,-1,0,1,32767}){
        auto packed=packMovementAxes(int16_t(x),int16_t(y));
        if(movementAxisX(packed)!=x||movementAxisY(packed)!=y)return 12;
    }
    if(fullTravelMovementStick({NAN,1}).y!=0)return 13;
    return 0;
}
