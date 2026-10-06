#include <iostream>
#include <vector>

#include "../src/protube/ProtubeShotPolicy.h"

namespace {

using namespace kharvox::protube;
using K = KharvoxWeaponKind;
using A = KharvoxWeaponAmmoState;

int failures{};

void require(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAILED: " << name << '\n';
        ++failures;
    }
}

Input held(std::uint64_t now, K weapon, bool fire = true,
    A ammo = A::Usable, bool gameplay = true) {
    return {now, gameplay, fire, weapon, ammo};
}

// Runs a simulated 90 Hz frame loop and returns the times of kicks.
std::vector<std::uint64_t> simulate(ShotPolicy& policy, K weapon,
    std::uint64_t fromMs, std::uint64_t toMs, bool fire = true,
    A ammo = A::Usable, std::vector<Command>* commands = nullptr) {
    std::vector<std::uint64_t> times;
    for (std::uint64_t t = fromMs; t < toMs; t += 11) {
        const auto c = policy.step(held(t, weapon, fire, ammo));
        if (c.valid()) {
            times.push_back(t);
            if (commands) commands->push_back(c);
        }
    }
    return times;
}

void testSemiAutoRepeat() {
    ShotPolicy policy;
    std::vector<Command> commands;
    const auto shots = simulate(policy, K::Shotgun, 0, 2000, true,
        A::Usable, &commands);
    // Press at 0, then roughly every 900 ms: 0, ~900, ~1800.
    require(shots.size() == 3, "shotgun held 2s fires three times");
    require(!commands.empty() && commands[0].kick == 230, "shotgun full kick");
    require(commands[0].rumbleMs == 70, "shotgun rumble length");
}

void testAutoFireCapsKick() {
    ShotPolicy policy;
    std::vector<Command> commands;
    const auto shots = simulate(policy, K::HeavyAssaultRifle, 0, 1000, true,
        A::Usable, &commands);
    require(shots.size() >= 9 && shots.size() <= 11, "HAR ~10 shots per second");
    for (const auto& c : commands)
        require(c.kick <= tempoKickCap(100), "HAR kick respects solenoid tempo");
}

void testChaingunSpinUp() {
    ShotPolicy policy;
    const auto shots = simulate(policy, K::Chaingun, 0, 2000);
    require(shots.size() > 4, "chaingun fires");
    const auto early = shots[1] - shots[0];
    const auto late = shots.back() - shots[shots.size() - 2];
    require(early > late, "chaingun interval shrinks while spinning up");
}

void testReleaseAndRepress() {
    ShotPolicy policy;
    require(policy.step(held(0, K::Pistol)).valid(), "pistol press kicks");
    require(!policy.step(held(50, K::Pistol)).valid(), "no kick inside interval");
    require(!policy.step(held(60, K::Pistol, false)).valid(), "release is silent");
    require(policy.step(held(80, K::Pistol)).valid(), "fast re-press kicks again");
}

void testGating() {
    ShotPolicy policy;
    require(!policy.step(held(0, K::Shotgun, true, A::Empty)).valid(),
        "empty weapon does not kick");
    require(!policy.step(held(0, K::Shotgun, true, A::Unavailable)).valid(),
        "unavailable weapon does not kick");
    require(policy.step(held(0, K::Shotgun, true, A::Unknown)).valid(),
        "unknown ammo state still kicks (older game builds)");
    ShotPolicy menu;
    require(!menu.step(held(0, K::Shotgun, true, A::Usable, false)).valid(),
        "no kick outside gameplay");
    ShotPolicy fists;
    require(!fists.step(held(0, K::Fists)).valid(), "fists do nothing");
    require(!fists.step(held(0, K::Unknown)).valid(), "unknown weapon does nothing");
}

void testBfgCharge() {
    ShotPolicy policy;
    const auto spool = policy.step(held(0, K::Bfg));
    require(spool.valid() && spool.kick == 0 && spool.rumble > 0,
        "BFG press starts a rumble-only charge");
    require(!policy.step(held(100, K::Bfg, false)).valid(),
        "nothing during charge after release");
    require(!policy.step(held(700, K::Bfg, false)).valid(), "still charging");
    const auto blast = policy.step(held(760, K::Bfg, false));
    require(blast.kick == 255, "BFG fires full kick after charge even when released");
    require(!policy.step(held(800, K::Bfg, false)).valid(), "BFG fires once");

    ShotPolicy cancelled;
    cancelled.step(held(0, K::Bfg));
    cancelled.step(held(100, K::Bfg, false, A::Usable, false));
    require(!cancelled.step(held(800, K::Bfg, false)).valid(),
        "leaving gameplay cancels the charge");
    ShotPolicy swapped;
    swapped.step(held(0, K::Bfg));
    swapped.step(held(100, K::Pistol, false));
    require(!swapped.step(held(800, K::Pistol, false)).valid(),
        "switching weapon cancels the BFG charge");
}

void testChainsawContinuous() {
    ShotPolicy policy;
    std::vector<Command> commands;
    simulate(policy, K::Chainsaw, 0, 1000, true, A::Usable, &commands);
    require(commands.size() >= 10, "chainsaw rumble refreshes while held");
    for (const auto& c : commands)
        require(c.kick == 0 && c.rumble > 0, "chainsaw is rumble only");
}

void testPowerScale() {
    ShotPolicy half(defaultProfiles(), 50);
    const auto c = half.step(held(0, K::RocketLauncher));
    require(c.kick == 127, "power scale halves kick");
    ShotPolicy off(defaultProfiles(), 0);
    require(!off.step(held(0, K::RocketLauncher)).valid(), "0% power sends nothing");
}

void testTempoTable() {
    require(tempoKickCap(250) == 255, "slow fire is uncapped");
    require(tempoKickCap(100) == 178, "100 ms matches ProTube table");
    require(tempoKickCap(62) == 82, "62 ms matches ProTube table");
    require(tempoKickCap(31) == 41, "faster than table scales down");
}

void testNoCatchUpBurst() {
    ShotPolicy policy;
    policy.step(held(0, K::HeavyAssaultRifle));
    // A 1 second hitch must produce one kick, not ten.
    require(policy.step(held(1000, K::HeavyAssaultRifle)).valid(), "kick after hitch");
    require(!policy.step(held(1011, K::HeavyAssaultRifle)).valid(),
        "no catch-up burst after hitch");
}

} // namespace

int main() {
    testSemiAutoRepeat();
    testAutoFireCapsKick();
    testChaingunSpinUp();
    testReleaseAndRepress();
    testGating();
    testBfgCharge();
    testChainsawContinuous();
    testPowerScale();
    testTempoTable();
    testNoCatchUpBurst();
    if (failures == 0) std::cout << "protube shot policy tests passed\n";
    return failures == 0 ? 0 : 1;
}
