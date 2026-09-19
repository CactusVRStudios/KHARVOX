# KHARVOX SFS (0.96)

SFS is the default renderer for all launcher profiles and first launch. AER is
available as fallback. This is the local main baseline; branch promotion does
not publish a release or change the remote repository.

## Rendering

KHARVOX owns the Vulkan interception, stereo resource tracking, multiview
rendering, compute expansion, shader corrections and OpenXR submission.
It does not load an external stereo provider DLL. Menus use the shared quad
presentation; the launcher controls cinematic presentation and FSR.

SFS has no NVIDIA-only launcher restriction. GPU/runtime compatibility still
requires hardware testing; vendor independence is not proof of shader provenance.

Life/Ammo panels follow the offhand, with independent calibration for each
panel and handedness. Debug's Live/Ammo calibration enables numpad editing.
The Test 29 secondary-fire pose fix and Test 30 render-bound HUD fix are included.
Their latest headset behavior remains subject to user testing.

## Build and validation

Build with `KHARVOX_BUILD_SFS_COMPILER=ON`, supplying
`KHARVOX_SPIRV_CROSS_SOURCE`, `KHARVOX_SPIRV_CROSS_LIB` and
`KHARVOX_GLSLANG_ROOT`. See `src/sfs/CMakeLists.txt` for the compiler dependencies.
Run CTest and the launcher `--self-test` after rebuilding.

A playable runtime needs the matching native build stamp, local compiled shader
profile, compiler notices, OpenXR loader, launcher/assets and both integration
bridges with approved SDK/loader DLLs. Never copy test SDK stubs into a release.
Run `tools/verify_package_integrations.ps1 -Package <runtime-directory>` before
delivery. `tools/probe_native_sfs.ps1` provides a bounded local runtime probe;
simulator evidence alone does not establish headset compatibility.

## Shader profile and provenance

`NativeSfs.cpp` selects profile replacements through `ShaderProfile.h` before
`ShaderCompiler.cpp` applies KHARVOX's corrections. Local packages currently
contain 75 compiled profile files; those files are not tracked in this source
repository. Rebuilding the layer does not replace these external inputs with
independently authored shaders. Preserve the profile notice in
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) and compiler-library licenses.

Legacy parameter names in the compatibility matcher are an input-format
contract. Renaming them without migrating profile inputs would break correction
of the existing shaders. They are not renderer branding.

The retired provider deployment/probe and binary-inspection scripts were removed
from the active tree. Earlier investigation is retained separately in
[archived development history](archive/SFS_DEVELOPMENT_HISTORY.md).
