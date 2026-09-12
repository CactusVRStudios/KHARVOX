#pragma once
namespace kharvox::native {
constexpr bool renderScene(bool quad, bool gameplay, bool immersiveCinematic) {
    return !quad && (gameplay || immersiveCinematic);
}
// Both gameplay and immersive cinematics use same-frame Native pairs. A pair
// from the other camera context cannot cross an entry/exit boundary.
constexpr bool useCurrentPair(bool pairValid, bool quad, bool cinematic, bool pairCinematic=false) {
    return pairValid && !quad && cinematic==pairCinematic;
}
}
