# Native SFS reconstruction, 2026-09-17

This is an implementation milestone, **not a playable VR renderer**. Branch:
`codex/beta-0.96-vulkan-single-frame-stereo`. The launcher no longer configures
the external provider; its third renderer remains unavailable until the native
path is integrated. Menus retain the planned AER/quad behavior.

## Reference and recovered behavior

Local reference: Vk3DVision 4.25.5.608, SHA-256
`B2EC1AC73A4BDB679C5B7A32286C5ACDF3BD84D52E416D98BA2965FCD8646ABA`.
RVAs below are for this exact x64 image, preferred base `0x180000000`.

| Routine | RVA | Recovered behavior |
| --- | --- | --- |
| CreateImage | `1a62a0` | One-layer 2D color/depth attachment or storage image becomes two layers. Ordinary sampled-only textures and 3D images are excluded. |
| CreateImageView | `1a6620` | A tracked stereo image's one-layer 2D view becomes a two-layer 2D array view. |
| CreateRenderPass | `1a8540` | Keeps a mono pass and creates a multiview alternative, view and correlation masks `3`. |
| CreateFramebuffer | `1a59c0` | Unwraps the provider's render-pass handle before downstream creation. |
| CmdDispatch | `1a37e0` | Multiplies only dispatch Z, according to the bound pipeline's classification. |
| Dispatch policy | `1cc6f0` | Default stereo multiplier two, shader exceptions for shared mono compute work. |
| Profile hash | `1cc5f0` | Murmur-like 64-bit rounds with multiplier `0x5bd1e995`, shift 47, default seed `0x1000193`; not standard Murmur64A. |
| Pipeline seed | `1ca2b0` | Hashes a 272-byte packet of selected fixed-state scalar values, excluding pointers. |
| Stage selection | `1e4060` | Hashes the original stage SPIR-V with that pipeline seed in HashV1 mode. The seed resets for each stage (`1e4738`); stages are not chained. |

The two hashes in a replacement filename are the original shader identity and
its pipeline variant. The second hash is not the other stage's shader hash.
The 272-byte packet also excludes actual vertex attributes, color blend
attachment equations, sample masks and dynamic-state arrays. Consequently it
must **not** be reused as a general pipeline-cache identity.

`tools/inspect_sfs_provider.py` produces bounded, version-checked static
disassembly without loading the DLL. Chained unwind regions mean that the
first unwind entry's end is not necessarily the end of the function.

## Implemented components

- `src/sfs/StereoResources.h`: image/view transformations, device-owned tracking,
  multiview render-pass plan and checked dispatch-Z expansion. Allocation
  failures do not silently fall back to mono while marking an image stereo.
  PREINITIALIZED stereo images are rejected rather than losing initial data.
- `src/sfs/ShaderIdentity.h` and `PipelineIdentity.h`: native profile identities.
- `src/sfs/ShaderProfile.h`: exact variant selection before generic replacement,
  bounded SPIR-V loading, instruction framing and execution-model checks.
  This is not a full SPIR-V validator or the runtime injection stage.
- `src/sfs/ShaderCapture.h` and layer hooks: opt-in original shader/pipeline
  capture, successful shader-module lifetime tracking and device cleanup.
  Capture alone does not mutate game shaders. Capture exceptions never escape
  into the Vulkan application. Captured module memory and new SPIR-V file data
  each have a 128 MiB per-process budget; pipeline records are limited to 100,000.
- `KharvoxSfsProfileAudit`: compiled-profile resolution against captured pipeline
  identities. No external provider code executes.

## Historical resource-core evidence

1. Twenty hash fixtures match direct calls to the reference's isolated pure hash
   routine, covering all tails and unaligned input. DLL initialization was not
   invoked for those calls. Only that import-free routine was called.
2. Bounded DOOM starts using only KHARVOX captured 647 original shader modules.
   The latest run recorded 714 graphics stage records, 565 unique stage/variant
   identities and 85 pipeline state packets. It reached the 30-second deadline
   without natural process exit; the owned process was then stopped. This is
   startup evidence, not a gameplay or headset test.
3. **All 69/69 variant-specific profile files match the captured game identities.**
   The C++ profile audit resolves 73 distinct compiled replacements. The other
   two profile files are compute shaders, outside this graphics-pipeline audit.
4. All 75 locally supplied replacements compile with an independent glslang
   compiler targeting Vulkan 1.1. This is **pre-injection** compilation: the
   provider's common injection and binding transformations are not reproduced
   just by compiling ShaderSwap files.
5. Four new CTests pass. The real GPU test uses the own resource/multiview code
   and confirms one render-pass depth clear writes both layers on RTX 4080 SUPER.
   It does not test distinct-eye geometry, headset output or hand latency.
6. Full native build and **101/101 CTests** pass, including existing PSVR2 tests.
   Launcher build and self-tests pass. The local diagnostic runtime passes
   `verify_package_integrations.ps1`: both bridges and approved x64 bHaptics /
   PSVR2 Toolkit DLLs are present. No playable package is delivered.

Local evidence: `out/beta096/doom-pipeline-capture/`, `profile-compiled/`,
`hash-oracle.json`, `ctest-sfs-core.log`. Game shader files are not committed.

## Reproduction

Use a runtime containing the current layer and all mandatory integration DLLs:

```powershell
tools/capture_sfs_profile.ps1 -Runtime out/beta096/runtime -Game 'D:\Games\dampf\steamapps\common\DOOM\DOOMx64vk.exe' -Output out/sfs-capture
python tools/compile_sfs_profile.py --profile 'D:\DoomVR\vk3\Profiles\DOOM\ShaderSwap' --compiler 'D:\DoomVR\build\shader-tools\glslang-main\bin\glslang.exe' --output out/sfs-profile-compiled
out/beta096/native/Release/KharvoxSfsProfileAudit.exe out/sfs-capture out/sfs-profile-compiled
```

## Native integration update (2026-09-18)

The historical resource-only evidence above is superseded by a working native
prototype in `NativeSfs.cpp`, `ShaderCompiler.cpp` and `FrameProjection.h`:

- The game renders both eyes through Vulkan multiview and two-layer resources;
  typed shader conversion preserves cube/3D textures and clamps mono-array
  sampling. Compute outputs split by eye, with profile exceptions retained.
- Descriptor bindings 30/31 carry profile parameters and predicted frame
  transforms. Command state is replayed after multiview subpass boundaries.
  Descriptor-pool reset/destruction clears bookkeeping. Shared frame uniforms
  currently use device-idle retirement, which is conservative and unoptimized.
- OpenXR receives both actual eye layers from one render. The source uses a
  centered enclosing FOV; final copies crop separately to each asymmetric FOV.
  Legacy profile expressions are converted to an affine depth/offset form, so
  pure IPD works even with zero projection slope. The earlier direct asymmetric
  shader projection disagreed with postprocessing and visibly distorted scenes.
- The Simulator session and frame calls share the window-owning worker thread.
  This fixes an observed cross-thread Win32 message deadlock. Runtime Vulkan
  commands use the downstream dispatch rather than game shader hooks.
- All 647 captured originals and 75 profile modules pass transformation and
  compilation. All 105 CTests pass, including real GPU layer sampling, runtime
  descriptor/state replay and zero-slope affine IPD output. These fixtures do
  not establish correctness of every DOOM render pass.
- A bounded campaign run reached 24,840 OpenXR frames with zero recorded
  end-frame failures or lifecycle violations at that checkpoint. Pre-compositor
  left/right PNGs from frame 1521 show coherent corridor/weapon geometry with
  differing eye perspectives. Capture metadata explicitly reports no matched
  raw source images. Its first capture exposed an omitted asymmetric crop and
  a 640x540 subrect inside 640x700; SFS now enables the crop/full-surface path
  independently of the legacy resolution marker.

Evidence remains local: `out/beta096/shader-audit-current.json`, CTest output,
`out/beta096/native-probe-current.log` and `%LOCALAPPDATA%/Temp/`
`KHARVOX-EyeCaptures/19244-126290421/`. Captured game shaders are not committed.

## Remaining validation

The final FOV crop was verified in campaign capture
`%TEMP%/KHARVOX-EyeCaptures/37884-127727828/`: both submission rectangles are
`[0,0,640,700]`, with distinct asymmetric crops and 64 mm eye separation.
The SFS frame path no longer automatically activates the desktop window:
a subsequent stalled run exposed a render-thread synchronous Win32 message
wait. User interaction may have contributed; disabling that automatic focus
path avoids this blocking operation. The restarted campaign produced the capture.

Still validate real-headset tracking,
controller/weapon timing, shadows, particles, UI depth and level transitions.
The current analytic projection rejects canted eye orientations and longitudinal
eye offsets; such devices require a fuller view-space reconstruction. No PSVR2
headset compatibility is claimed. Toolkit bridges/protocol tests remain intact.
RenderPass2, synchronization2 and indirect compute paths are not covered by this
DOOM-specific prototype. The compiler SDK has not yet been rebuilt from fully
pinned glslang/SPIRV-Tools sources. The launcher now permits the explicit SFS test
choice with the matching compiler-enabled DLL manifest, full local profile and
licenses. This package check does not certify headset compatibility.

The original external provider's startup heap corruption is historical evidence;
fixing or loading that DLL is no longer a prerequisite for this native path.

## Shared AER gameplay integration (2026-09-18)

SFS now passes Vulkan resource, render-pass and command observations through the
same KHARVOX HUD/depth/lifetime hooks as AER. Its multiview producer remains
single-frame: the centered CPU camera publishes a source-bound weapon/body
snapshot on every frame, using the existing AER attachment history without
holding inputs for a second CPU eye. This removes the previous live-input versus
rendered-camera mismatch; walking jitter still requires a tracked-controller test.

Hands and the laser can render into each SFS eye with its own scene depth.
Cached single-layer attachment views and private depth copies select array layer
0 or 1 explicitly, preserving the game's depth/stencil data. Laser placement
uses the completed frame's body transform and weapon source identity. Raw eye
captures now select the corresponding source layer as well. Launcher options
for weapon tracking, hand visibility, handedness, turning, laser, gunstock and
render scaling continue through the shared configuration unchanged.

Validation: the complete Release build, all **106 CTests**, and launcher
self-tests pass. GPU fixtures check right-layer depth copying, source stencil
preservation and cached eye attachment views. A bounded five-minute campaign
run recorded successful scene-depth hand integration for both eyes. Capture
`%TEMP%/KHARVOX-EyeCaptures/11404-131093640/`, frame 787, has matched raw sources
for both eyes and full 640x700 submission rectangles. The test ended at its
deadline. The updated local runtime passes the integration package verifier.

This run does **not** validate weapon latency or removal of native animated
arms: the Simulator session was not focused, so the shared AER input guard
withheld the controller weapon pose (source resolution reason 6, invalid input).
The user reported that focusing the Simulator instead opens DOOM's pause menu.
The focus guard has not been bypassed. Real-headset weapon movement, walking,
laser placement, HUD behavior and menu/level transitions remain to be checked;
complete behavioral parity and hardware compatibility are not yet established.


## Headset-independent gameplay hardening (2026-09-18)

- SFS now uses the same published controller yaw as AER. Action processing can
  advance smooth-turn input after the camera is published; using that newer
  angle for the weapon/laser mixed two coordinate frames. Both now use the
  camera's captured angle. Native replay retains its existing policy.
- AER cache teardown at checkpoint and inactive-pair transitions no longer
  disables a centered camera already prepared by the native/SFS producer.
  This includes the warm-up frame on return from menus/scripted scenes.
- SFS records the installed render pose per acquired swapchain image. A new
  pending prediction cannot relabel another image, and an unacquired or recreated
  image cannot inherit an old source identity. Repeated image enumeration
  preserves existing acquisitions. Both Vulkan acquire entry points provide
  the actual swapchain/image index. The conservative GPU retirement remains.
- SteamVR compatibility logging no longer describes SFS as alternating eyes.

The full Release build and **107/107 CTests** pass. A new 600-frame synthetic
contract test exercises centered camera refresh, walking/body-yaw catch-up,
future controller publications, tracking loss/recovery, calibration/epoch
changes and rejection of previous level/menu/cinematic sources. The GPU fixture
also checks acquisition ownership, pending versus installed poses, repeated
swapchain enumeration and destroyed/recreated handles. These are automated
contract checks, not measurements of real controller latency or headset comfort.

A bounded Simulator run (PID 42924) reached over 3,000 OpenXR frames with zero
recorded end-frame failures or lifecycle violations at frame 3000, then exited
normally with code 0. Its controller input remained unfocused; no new claim is
made about native-arm hiding or real weapon tracking. The last two transition
condition corrections were subsequently compiled and checked by the full suite;
they have not had a further live campaign test. Evidence is in local
`out/beta096/build-gameplay-parity.log`, `ctest-gameplay-parity.log`, and
`probe-gameplay-parity.log`. The refreshed native-runtime package passed the
mandatory integration verifier. AER-compatible controls and settings are still
shared; the remaining headset/visual validation listed above is outstanding.
