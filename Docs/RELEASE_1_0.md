# KHARVOX 1.0 Release Candidate

Based on Test38, with the accepted in-game HUD and hand calibration as the new defaults.

- With SteamVR active, launcher RenderScale is disabled. Adjust resolution within SteamVR. The saved launcher value is preserved for other runtimes; SteamVR starts with neutral launcher scaling and no launcher FSR downscaling.
- SFS is the default renderer; AER remains available as fallback.
- Life, Ammo and the five-circle ProgMeter have independent offhand calibration for Normal and Left Mode.
- Updated global/per-weapon hand calibration is included as the default, without requiring a saved override file.
- In-game calibration remains available in Debug. It is disabled initially in this release.
- bHaptics and PSVR2 bridge executables and approved loader/SDK DLLs are bundled.

Extract the complete package into a new directory and start KharvoxLauncher.exe. Works only with a legal Steam version of DOOM 2016. Game files are not included.

The renderer is unchanged from Test38. The new defaults are the user's accepted Test38 settings; this release does not claim additional hardware validation. External compiled shader-profile provenance is recorded in THIRD_PARTY_NOTICES.md. Other bundled components retain their notices.
