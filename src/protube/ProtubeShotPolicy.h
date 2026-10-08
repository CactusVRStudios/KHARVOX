#pragma once

// ProTubeVR (ProVolver / ForceTube) recoil policy for KHARVOX.
//
// DOOM (2016) does not expose a per-shot event to KHARVOX, so recoil is
// synthesized from the same signals the PSVR2 adaptive-trigger path already
// trusts: primary fire held during real gameplay, the current weapon kind and
// its ammo state. Each weapon has a profile describing how its kicks are
// paced. Everything in this header is pure and deterministic so it can be
// unit tested without hardware or the game.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "../weapon/WeaponHook.h"

namespace kharvox::protube {

enum class FireMode : std::uint8_t {
    None = 0,     // no feedback (fists, unknown)
    Repeat = 1,   // kick on press, then every intervalMs while held
    Charge = 2,   // rumble on press, single kick after chargeMs (BFG)
    Continuous = 3, // rumble only while held (chainsaw)
};

struct ShotProfile {
    FireMode mode{FireMode::None};
    std::uint8_t kick{};        // 0..255 solenoid power
    std::uint8_t rumble{};      // 0..255 motor power
    std::uint16_t rumbleMs{};   // per-shot rumble length, clamped to 25..500
    std::uint16_t intervalMs{}; // Repeat: time between automatic shots
    std::uint16_t rampMs{};     // Repeat: spin-up time from 2x interval to interval
    std::uint16_t chargeMs{};   // Charge: delay from press to the kick
};

struct Command {
    std::uint8_t kick{};
    std::uint8_t rumble{};
    std::uint16_t rumbleMs{};
    bool valid() const { return kick != 0 || rumble != 0; }
};

constexpr std::size_t weaponKindCount =
    static_cast<std::size_t>(KharvoxWeaponKind::Count);
using ProfileTable = std::array<ShotProfile, weaponKindCount>;

constexpr std::uint16_t minimumRumbleMs = 25;
constexpr std::uint16_t maximumRumbleMs = 500; // SDK limit
constexpr std::uint16_t minimumIntervalMs = 40;

// Timings are approximations of DOOM (2016) fire rates and are tunable in
// protube.ini without rebuilding.
inline ProfileTable defaultProfiles() {
    ProfileTable t{};
    auto set = [&t](KharvoxWeaponKind kind, ShotProfile profile) {
        t[static_cast<std::size_t>(kind)] = profile;
    };
    using K = KharvoxWeaponKind;
    using M = FireMode;
    //                         mode         kick rumble ms  interval ramp charge
    set(K::Pistol,            {M::Repeat,    150,  90,  40,  330,     0,   0});
    set(K::Shotgun,           {M::Repeat,    230, 180,  70,  900,     0,   0});
    set(K::HeavyAssaultRifle, {M::Repeat,     80, 120,  40,  100,     0,   0});
    set(K::PlasmaRifle,       {M::Repeat,     40, 140,  50,   95,     0,   0});
    set(K::RocketLauncher,    {M::Repeat,    255, 200,  90,  900,     0,   0});
    set(K::SuperShotgun,      {M::Repeat,    255, 255, 100, 1300,     0,   0});
    set(K::GaussCannon,       {M::Repeat,    255, 160,  80, 1500,     0,   0});
    set(K::Chaingun,          {M::Repeat,     40, 160,  40,   70,   600,   0});
    set(K::Bfg,               {M::Charge,    255, 220, 120,    0,     0, 750});
    set(K::Chainsaw,          {M::Continuous,  0, 200, 100,   90,     0,   0});
    set(K::AssaultRifle,      {M::Repeat,    160, 110,  40,  110,     0,   0});
    set(K::ArcCannon,         {M::Continuous,  0, 150,  80,   75,     0,   0});
    set(K::MancubusGland,     {M::Repeat,    200, 150,  80,  800,     0,   0});
    // Fists and Unknown stay FireMode::None.
    return t;
}

// Maximum usable kick for automatic fire, from ProTubeVR's published
// rate-of-fire table: the solenoid needs time to retract or kicks are skipped.
inline std::uint8_t tempoKickCap(std::uint16_t intervalMs) {
    struct Point { std::uint16_t ms; std::uint8_t power; };
    static constexpr std::array<Point, 10> table{{
        {62, 82}, {66, 92}, {71, 105}, {77, 120}, {83, 135},
        {91, 156}, {100, 178}, {111, 206}, {125, 242}, {250, 255}}};
    if (intervalMs == 0 || intervalMs >= table.back().ms) return 255;
    if (intervalMs <= table.front().ms)
        return static_cast<std::uint8_t>(
            table.front().power * intervalMs / table.front().ms);
    for (std::size_t i = 1; i < table.size(); ++i) {
        if (intervalMs <= table[i].ms) {
            const auto& a = table[i - 1];
            const auto& b = table[i];
            const int span = b.ms - a.ms;
            const int offset = intervalMs - a.ms;
            return static_cast<std::uint8_t>(
                a.power + (b.power - a.power) * offset / span);
        }
    }
    return 255;
}

inline std::uint8_t scalePower(std::uint8_t value, int percent) {
    const int scaled = value * std::clamp(percent, 0, 200) / 100;
    return static_cast<std::uint8_t>(std::clamp(scaled, 0, 255));
}

struct Input {
    std::uint64_t nowMs{};
    bool gameplay{};
    bool fireDown{};
    KharvoxWeaponKind weapon{KharvoxWeaponKind::Unknown};
    KharvoxWeaponAmmoState ammo{KharvoxWeaponAmmoState::Unknown};
};

class ShotPolicy {
public:
    explicit ShotPolicy(ProfileTable profiles = defaultProfiles(),
        int powerPercent = 100)
        : profiles_(profiles), powerPercent_(powerPercent) {}

    // Call once per frame. Returns a command to send, or an invalid command.
    Command step(const Input& in) {
        // A BFG charge that already started finishes even if the trigger is
        // released, matching the game; it is cancelled only by leaving
        // gameplay or switching weapons.
        if (chargePending_) {
            if (!in.gameplay || in.weapon != chargeWeapon_) {
                chargePending_ = false;
            } else if (in.nowMs >= chargeFireAtMs_) {
                chargePending_ = false;
                const auto& p = profile(chargeWeapon_);
                return make(p.kick, p.rumble, p.rumbleMs);
            }
        }

        const bool ammoOk = in.ammo != KharvoxWeaponAmmoState::Empty
            && in.ammo != KharvoxWeaponAmmoState::Unavailable;
        const bool active = in.gameplay && in.fireDown && ammoOk;
        if (!active || in.weapon != heldWeapon_) {
            held_ = false;
            heldWeapon_ = in.weapon;
            if (!active) return {};
        }

        const auto& p = profile(in.weapon);
        const bool pressEdge = !held_;
        if (pressEdge) {
            held_ = true;
            pressMs_ = in.nowMs;
        }

        switch (p.mode) {
        case FireMode::None:
            return {};
        case FireMode::Repeat: {
            if (pressEdge) {
                nextShotMs_ = in.nowMs + currentInterval(p, 0);
                return make(cappedKick(p, 0), p.rumble, p.rumbleMs);
            }
            if (in.nowMs < nextShotMs_) return {};
            const auto heldMs = in.nowMs - pressMs_;
            const auto interval = currentInterval(p, heldMs);
            // Stay on the cadence grid but never burst to catch up after a
            // long frame hitch.
            nextShotMs_ = std::max(nextShotMs_ + interval, in.nowMs + interval / 2);
            return make(cappedKick(p, heldMs), p.rumble, p.rumbleMs);
        }
        case FireMode::Charge:
            if (pressEdge && !chargePending_) {
                chargePending_ = true;
                chargeWeapon_ = in.weapon;
                chargeFireAtMs_ = in.nowMs + p.chargeMs;
                const auto spool = static_cast<std::uint16_t>(
                    std::clamp<int>(p.chargeMs, minimumRumbleMs, maximumRumbleMs));
                return make(0, static_cast<std::uint8_t>(p.rumble / 2), spool);
            }
            return {};
        case FireMode::Continuous: {
            const auto interval = std::max<std::uint16_t>(p.intervalMs, minimumIntervalMs);
            if (pressEdge || in.nowMs >= nextShotMs_) {
                nextShotMs_ = in.nowMs + interval;
                return make(0, p.rumble, p.rumbleMs);
            }
            return {};
        }
        }
        return {};
    }

    void reset() {
        held_ = false;
        chargePending_ = false;
    }

    const ShotProfile& profile(KharvoxWeaponKind kind) const {
        const auto index = static_cast<std::size_t>(kind);
        static const ShotProfile none{};
        return index < profiles_.size() ? profiles_[index] : none;
    }

private:
    static std::uint16_t currentInterval(const ShotProfile& p, std::uint64_t heldMs) {
        const auto base = std::max<std::uint16_t>(p.intervalMs, minimumIntervalMs);
        if (p.rampMs == 0 || heldMs >= p.rampMs) return base;
        // Linear spin-up from double the interval down to the interval.
        const auto remaining = p.rampMs - heldMs;
        return static_cast<std::uint16_t>(base + base * remaining / p.rampMs);
    }

    static std::uint8_t cappedKick(const ShotProfile& p, std::uint64_t heldMs) {
        if (p.kick == 0) return 0;
        return std::min(p.kick, tempoKickCap(currentInterval(p, heldMs)));
    }

    Command make(std::uint8_t kick, std::uint8_t rumble, std::uint16_t ms) const {
        Command c{};
        c.kick = scalePower(kick, powerPercent_);
        c.rumble = scalePower(rumble, powerPercent_);
        c.rumbleMs = c.rumble
            ? std::clamp(ms, minimumRumbleMs, maximumRumbleMs) : 0;
        return c;
    }

    ProfileTable profiles_{};
    int powerPercent_{100};
    bool held_{};
    KharvoxWeaponKind heldWeapon_{KharvoxWeaponKind::Unknown};
    std::uint64_t pressMs_{};
    std::uint64_t nextShotMs_{};
    bool chargePending_{};
    KharvoxWeaponKind chargeWeapon_{KharvoxWeaponKind::Unknown};
    std::uint64_t chargeFireAtMs_{};
};

} // namespace kharvox::protube
