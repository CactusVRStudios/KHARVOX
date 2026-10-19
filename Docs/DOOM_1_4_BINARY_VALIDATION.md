# DOOM 2016 binary comparison for KHARVOX 1.4

The initial binary comparison read inputs as data. The subsequent GOG Fix 2
startup check used the complete GOG installation with SFS and OpenXR.

| Build | SHA-256 | PE timestamp | Image size |
| --- | --- | --- | --- |
| Steam Vulkan | `3d8b4393649f80bdae85c19ae2c985c45ec9ac54f74ab04a18816e30ead63c21` | 1711036533 | `0x14161000` |
| GOG Vulkan | `a32df8ffa042090f14fe0a200f1c5d7dddf9c947fac223916c252f826f1ecf11` | 1740486338 | `0x1415f000` |
| GOG OpenGL (unsupported) | `5f5db61bcb437824182bda392c61daedfb8ac7b0ae98a85f887a0d7f5d662038` | 1740485043 | `0x12a6e000` |

## Method and results

The two Vulkan executables cannot use a single fixed address delta. Functions,
data records and vtables move by different amounts. The adapter contains 274
reviewed address pairs, with per-address evidence labels in
`doom-build-addresses.json`. The source version is
`src/common/DoomBuildData.json`.

The analysis uses identical-function anchors, normalized instruction bodies,
instruction references, MSVC RTTI, reflected field names/types/offsets/sizes,
and enum label/value pairs. Normalization is used only to locate candidates;
runtime hooks retain exact profile-specific checks. Steam RVAs remain canonical
for caller classification and saved HUD policies. Returned GOG addresses are
converted back where those policies need them.

The source/binary verifier checked 250 literal profile lookups, 61 byte guards,
32 mapped RTTI/vtables and the player virtual slots at `0x368`, `0x370`, `0x660`.
All checks passed. Reflected camera/collectible field offsets and enum values
were matched by metadata, rather than applying a guessed section displacement.
The generated profile retains 1,451 exact guard-byte substitutions and six
hook-body fingerprints. No executable payload is distributed.

The C++ fixture test maps each executable into a byte buffer without loading or
executing it. It exercises the production build detector and hook-body hashes,
and checks that a modified code byte is rejected. Neither executable has base
relocations inside its `.text` section, so its code fingerprint is ASLR-stable.
The launcher separately checks the full-file SHA-256 and correctly rejects the
GOG OpenGL executable.

The initial 1.4 Release build passed all 129 CTest checks, including the SFS compiler
and isolated Vulkan GPU tests, plus the launcher's self-test. These tests do not
run the GOG game or establish headset compatibility. Fix 2 passed all 129 checks again and the expanded launcher self-test. Its GOG
startup watchdog reported healthy, with successful XR frames. Sixteen mapped
control/weapon CVars were checked by name and value in the live GOG process, and
the adaptive-participant hook installed successfully.

## Reproducing the binary checks

The Python tools need `pefile` and Capstone 5.0.9. Supply your own legally
obtained executable files; none are stored in the repository or release.

```text
python tools/audit_doom_builds.py --steam STEAM_EXE --gog GOG_EXE --output audit.json
python tools/generate_doom_build_profile.py --steam STEAM_EXE --gog GOG_EXE --audit audit.json --output src/common/DoomBuildData.inc
python tools/verify_doom_build_profiles.py --steam STEAM_EXE --gog GOG_EXE --report validation.json
KharvoxDoomBuildProfileTests.exe STEAM_EXE GOG_EXE
KharvoxLauncher.exe --verify-game GOG_EXE
```

The launcher writes the inspection result to `%TEMP%\KHARVOX-game-build-check.log`.
These checks establish binary/profile consistency, not GOG gameplay correctness.
The full GOG installation is now available and its startup is tested. On 2026-10-19, the user confirmed that the GOG fix works after the gameplay test. Further
gameplay checks are still needed for broad SFS shader coverage, saves, camera
tracking and level transitions.
