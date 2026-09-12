#pragma once
namespace kharvoxnative::input_hooks {
// Bounded, read-only diagnostic for the MonoTracked milestone (see project
// plan Phase 2.2): confirms whether/how DOOM consumes Win32 Raw Input mouse
// deltas, as a prerequisite for injecting synthesized look input from OpenXR
// head-orientation deltas. Never writes game state.
bool install();
void uninstall();
}
