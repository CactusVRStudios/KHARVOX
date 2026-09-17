# 0.96: Vulkan Single-Frame Stereo

Status: **development branch builds; provider startup blocked; no playable SFS/VR release**.

Branch: `codex/beta-0.96-vulkan-single-frame-stereo`, based on `0cd5f29`.
The unsuccessful 3DTV branch is retained at `ae00eba`; its closing report is
`experiments/3dtv/RESULT.md` on that branch. Its second CPU world-view hooks are
not part of this experiment.

## What the provider does

The user-supplied Vk3DVision 4.25.5.608 is a Vulkan layer, not an OpenGL renderer.
The public repository distributes binaries; it does not provide the renderer
source needed to incorporate or repair its internals directly.

The DOOM profile enables Single Frame Stereo. Vertex/fragment shaders select an
eye with `gl_ViewIndex`; compute shaders split dispatch depth between eyes.
2D textures/images become arrays. The profile contains 75 shader replacements
and injection rules for projection, clustered lighting and postprocessing.
GLSL in these files is compiled to Vulkan SPIR-V; it does not imply OpenGL.
Shared buffers, 3D textures and shadow/compute work need separate classification.

Its fixed separation/convergence projection is not yet an OpenXR projection:
correct headset IPD, per-eye FOV, frame pose, controller timing and menu handoff
still require a bridge. Merely splitting an SBS image does not establish those.
The intended menu behavior remains the existing AER/quad presentation.

## Implemented here

- Third launcher choice `VULKAN_SFS`, labelled **Vulkan Single-Frame Stereo (Test)**,
  with settings restoration and a clear early refusal while startup is broken.
- Process-local provider configuration: environment variable `Vk3DVision` points
  to the parent of the extracted `Vk3DVision/` directory. Without it the DLL
  returns loader error 1114. No provider GUI or global registry installation is needed.
- Proposed layer order: producer first, KHARVOX second, driver last. This lets
  KHARVOX use downstream dispatch for its own allocations. The loader confirmed
  this order, but completed stereo-image transfer is **not verified**.
- Isolated profile preparation, retaining the external provider's identity and
  attribution. User game shaders are not committed or distributed with the source.
- Two compute-shader fixes for hash `53feb817281c0c4e`: retain `sampler3D`
  `textureLod` semantics and explicitly convert the vector-field uint flag to bool.
- Bounded provider-only reproduction script, child PID ownership and captured exit
  code. The user's original provider/profile are not edited.
- Diagnostic swapchain array-layer logging. Independent desktop scaling is
  disabled for provider bring-up. OpenXR session creation is withheld in this mode;
  ordinary AER must not consume an unverified stereorized image.

An exploratory SBS-copy patch was not retained in the compiled renderer because
the actual image contract is unverified. In particular, the downstream swapchain
requests **two array layers**, so assuming a single packed image from its width
would be unjustified.

## Observations, 2026-09-17

Machine: Windows 11, RTX 4080 SUPER; provider log reports driver 616.64.
Provider SHA-256:
`B2EC1AC73A4BDB679C5B7A32286C5ACDF3BD84D52E416D98BA2965FCD8646ABA`.

1. DLL loaded successfully after supplying its root environment variable.
2. Original profile failed to compile the particle shader: invalid sampler3D
   array conversion. Fixing that exposed a uint-to-bool assignment error.
3. After both targeted corrections, these compilation errors disappeared from
   the observed startup log. This is not validation of every shader in the game.
4. Combined and **provider-only** starts exited with signed code `-1073740940`
   (`0xC0000374`, STATUS_HEAP_CORRUPTION), before usable presentation was established.
5. Disabling both shader caching and shader-cache optimization still produced
   that exit. No exact corrupting instruction or root cause has been identified.

Thus this failure does not require KHARVOX's OpenXR consumer or the old 3DTV hooks.
It does not establish that Vulkan SFS is fundamentally impossible. The binary,
profile, driver and game combination needs further isolation first.

Validation: native build and launcher build succeeded; 97/97 existing CTest tests
passed, including PSVR2 backend/protocol tests. Launcher self-tests passed. The
local runtime passed `tools/verify_package_integrations.ps1` with approved x64
bHaptics and PSVR2 DLLs and both bridges. None of this proves headset compatibility.

Local evidence is under `out/beta096/`, particularly
`repro-evidence/result.json`, `repro-evidence/loader.log`,
`repro-evidence/provider.log`, `provider-only-no-cache-loader.log` and
`provider-only-no-cache.log`. The checked-in reproduction script also reproduced
the same natural exit from a freshly prepared provider directory.
No release archive is delivered; the third renderer is deliberately blocked
before any launcher startup side effects.

## Reproduction and next milestone

Prepare a new local directory (never reuse the source profile):

```powershell
tools/prepare_sfs_provider.ps1 -ProviderRoot D:\DoomVR\vk3\Vk3DVision -DoomProfile D:\DoomVR\vk3\Profiles\DOOM -Destination D:\KHARVOX\out\sfs-provider-new
tools/probe_sfs_provider.ps1 -GameExe 'D:\Games\dampf\steamapps\common\DOOM\DOOMx64vk.exe' -ProviderRoot D:\KHARVOX\out\sfs-provider-new -OutputDirectory D:\KHARVOX\out\sfs-evidence-new
```

First milestone: a stable provider-only DOOM menu and gameplay image. Then capture
both actual eye resources, identify their layouts/layers and synchronization,
and calibrate projection against OpenXR. Only then enable the third renderer for
gameplay and validate menu transitions, hand motion and headset performance.

Sources: [provider releases](https://github.com/helifax/Vk3DVision-Public/releases),
[BSD-3-Clause license](https://github.com/helifax/Vk3DVision-Public/blob/main/LICENSE),
the user-supplied `HowToAddSupportForSingleFrameStereo.pdf`, DOOM `README.pdf`,
profile and runtime logs. The provider's notice is embedded through KHARVOX's
existing third-party notices. The separate game-shader collection remains local.
