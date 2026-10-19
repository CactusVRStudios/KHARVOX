# KHARVOX 1.4 — GOG startup fix 1

File version: 1.4.0.1401. Use this package instead of the initial 1.4 archive.

## Fixed GOG startup failure

The initial GOG adaptation could stop shortly after creating the OpenXR session
with exit code `0x4E535452` and `native render CVar identity changed` in the native
log. A GOG CVar address that was not in the translation table could numerically
equal the Steam address of a different, protected CVar. The fallback incorrectly
treated that number as a valid Steam identity.

Unmapped GOG objects and callers now have an unknown canonical identity. They
cannot match Steam controls or HUD/camera ownership rules. Known GOG addresses
still map to their verified Steam identities, and the CVar name guard remains
enabled. Regression tests cover the collision and both executable profiles.

## GOG and administrator checks

GOG launches do not inspect or require the Steam client's privilege state.
An unrelated Steam instance running as administrator does not block GOG.
GOG failure messages also no longer advise changing Steam's permissions.
KHARVOX's and the selected DOOM executable's own elevation checks remain active.
Steam launches continue to use the Steam checks.

## Validation

The complete GOG installation now starts successfully using the SFS renderer
with the saved settings. The launch watchdog reports a healthy start, and the
OpenXR log confirms successful frame submissions beyond the previous failure.
This startup check does not establish complete campaign or headset compatibility.

The launcher self-test, build-profile tests against both real executables, and
render-startup-control tests pass. Required bHaptics and PSVR2 integrations are
included and checked before packaging. Ko-fi, SFS and AER are retained; Peanut
is not included.

Extract into a new folder and use its `KharvoxLauncher.exe`. Do not mix DLLs
with the original 1.4 package. The supported game executable is `DOOMx64vk.exe`.
