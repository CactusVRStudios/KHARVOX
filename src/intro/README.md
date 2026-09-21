# VR GameIntro

The launcher starts a dedicated instance of `KharvoxLauncher.exe --vr-intro` on
the first actual Launch Game action and after upgrading to a newer release. Native rendering and embedded assets live
in the existing `KharvoxLayer.dll`. The package also ships `KharvoxIntro.exe`
as an independently clickable demo with the same embedded assets and renderer.
Closing the preview or dismissing the intro requests teardown. The launcher waits
for the helper process to exit before starting DOOM, so the two applications do
not hold VR sessions concurrently. If teardown stalls, the launcher terminates
only its own intro helper after seven seconds and verifies process termination.
The desktop close button also continues to DOOM; the standalone demo simply exits.

Running `KharvoxLauncher.exe --vr-intro` without further arguments runs the demo independently. A button
press exits without launching DOOM or updating the launcher's seen-release marker.
The marker stores the highest dismissed release version; the same release and downgrades skip the intro. Legacy unversioned markers replay once. After dismissal, Disable VR-Intro appears above Enable Hands and defaults to unchecked. The intro plays on every launch until the user checks it; upgrades still play once.

The layer embeds the font, original MOD music, Copper_41 palette and credits.
It needs `openxr_loader.dll`, an available OpenXR headset/runtime and the Windows
and Visual C++ runtimes. The VR eyes use the runtime's recommended resolution;
the independent desktop preview is 1280x720 at up to 30 Hz. Nine text blocks
reveal their lines at 0.60-second intervals with 0.70-second effects and remain
fully visible for eight seconds. Only the footer responds to music beats.

## Build and validation

Build the CMake Release targets and the launcher project. CTest covers rendering,
line timing, embedded audio and ordered teardown. Run
`tools/test_vr_intro_handoff.ps1` for the launcher/process handshake and the
launcher's `--self-test` for startup error handling. Runtime-specific stability,
including Meta session exit, still requires headset testing; mocks do not prove
hardware compatibility. `%TEMP%/KHARVOX-VR-INTRO.log` records runtime identity, exit stages and
cleanup results for that diagnosis.

Every playable full release or prototype must also include both haptics bridges,
`bhaptics_library.dll` and `psvr2_toolkit_capi_loader.dll`. Set the CMake cache path
`KHARVOX_BHAPTICS_SDK_DLL` when the approved SDK is not in the local KHARVOX SDK
directory. Run `tools/verify_package_integrations.ps1 -Package <directory>`
before delivery; `tools/test_package_integrations.ps1` tests rejection of missing
or modified integration binaries. Never bundle the fake/incomplete test DLLs.

The CMake `KharvoxGameIntro` target produces the standalone `KharvoxIntro.exe`.
Double-clicking it runs the demo; a button press exits without starting DOOM
or changing the first-launch marker. Keep it beside `openxr_loader.dll`.
No executable is extracted from the launcher at runtime. Antivirus detection
is vendor-dependent; this packaging change does not guarantee fewer detections.
