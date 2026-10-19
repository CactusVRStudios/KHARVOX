# KHARVOX 1.4 — Steam / GOG compatibility

This release adds an experimental adapter for the supplied GOG DOOM (2016)
Vulkan executable while retaining the existing Steam adapter. The launcher
identifies the executable automatically; no distribution switch is needed.

- Separate, verified address profiles for Steam 2024-03-21 and GOG 2025-02-25.
- Camera, weapon, HUD, engine-size, memory-guard and AER engine-observer hooks
  select the matching profile. Signature checks remain enabled.
- GOG launches do not require the Steam client and do not receive Steam AppID
  overrides. Unknown or modified executables are rejected before hook setup.
- The launcher retains the clickable Ko-fi support image.
- SFS and AER remain the renderer choices. The discarded Peanut experiments
  are not part of this release.
- The complete bHaptics and PSVR2 Toolkit runtime integrations are bundled.

## Installation and testing status

Extract the entire archive into a new folder and start `KharvoxLauncher.exe`.
Select a **complete** DOOM (2016) installation containing `DOOMx64vk.exe`.
Do not copy KHARVOX into the game folder or mix files from older test packages.

**GOG support remains experimental.** The complete GOG installation now passes
local startup checks, and the user confirmed the gameplay fix. This package
includes the GOG CVar identity collision fix and completed OpenXR control/weapon
address adaptation from GOG Fix 2. Steam privilege checks apply only to Steam
launches; GOG does not depend on the Steam client's administrator state.
Broader campaign, shader and level-transition coverage remains unverified.

The Ko-fi button is now smaller and aligned with the centered Launch Game button.
Reduced logo and footer spacing makes the launcher window 84 pixels shorter at standard scaling. Launcher file version: 1.4.0.1404. Native runtime file version: 1.4.0.1402.
The native runtime is the same build used in the successful GOG Fix 2 test.

`DOOMx64.exe` uses OpenGL and is not supported. Neither game executable nor any
game assets are included in this package. The legacy hidden Native backend is
not enabled for GOG.

See `compatibility/BINARY-VALIDATION.md` for exact executable identities and the
scope of the checks. No new headset or bHaptics hardware compatibility is claimed.
