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


## PSVR2 / SteamVR / Radeon tester report (2026-09-18)

Input: user-supplied Logs.zip, current launch 09:04:15–09:05:14, VULKAN_SFS,
SteamVR/OpenXR 2.17.9, AMD Radeon RX 9070 XT. Both application and XR select the
same GPU model. The session reaches FOCUSED; shouldRender=1 and xrEndFrame
returns success for quad and gameplay projection. The process exits with code
0. The tester nevertheless sees SteamVR's environment rather than game imagery.
These logs alone do not establish compositor visibility.

Three defects were addressed:

1. Exactly 43 SFS shader compile failures contain `unimplemented op 261`, then
   an undefined result identifier. This is legacy OpGroupAll in the AMD lighting
   modules. The compiler now emits subgroup all/any votes with explicit scope
   checking and control-dependency tracking. Workgroup scope is rejected instead
   of being silently narrowed. Reference: [Khronos SPIR-V OpGroupAll](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpGroupAll).
2. The retained runtime callback isolated GDPA but still returned public-loader
   GIPA device commands, including vkCreateImage and vkCreateGraphicsPipelines.
   Those can re-enter the game device's SFS hooks. Both lookup paths now isolate
   runtime device commands below the game transforms; instance and physical
   commands retain their existing dispatch level. A generated allowlist from
   the bundled Vulkan headers classifies commands by first handle argument.
   Non-null GDPA alone is insufficient: an initial simulator test exposed a
   driver returning physical commands there. That attempt was stopped at its
   deadline, corrected, covered by regression tests, and retested successfully.
   Runtime-hook contamination is a plausible cause of missing compositor output,
   but is not proven as the sole cause on the tester's hardware.
3. Launcher startup monitoring repeatedly read only the first 1 MiB, whereas
   Frame 120 appears after the multi-megabyte shader diagnostics. It now reads
   this launch's prefix plus its recent tail. Tests cover oversized logs, stale
   prior-launch markers and device loss. OpenXR lifecycle diagnostics now include
   the actual submitted layer count instead of only a descriptive layer reason.

Validation: 108/108 CTests, launcher build/self-test, and 722/722 locally captured
original/profile shader translations pass. A minimal legacy Groups SPIR-V fixture
reproduces the opcode family and tests all/any plus unsupported-scope rejection;
the tester's original SPIR-V modules were not included, so their complete shader
set has not been replayed. A 45-second bounded Simulator test (PID 3748) reached
at least 1,800 successful quad frames with layers=1 and zero lifecycle violations
or end-frame errors, then was terminated at the deadline. This does not validate
PSVR2 hardware, Radeon gameplay, or SteamVR compositor visibility.

Complete local tester package: `out/beta096/psvr2-steamxr-fix1/`; archive
`out/beta096/KHARVOX-0.96-SFS-PSVR2-SteamXR-fix1.zip`. Mandatory approved x64
bHaptics and PSVR2 bridges/loaders are bundled and package-verified. Use the
launcher and select Vulkan Single-Frame Stereo (Test). The next useful tester
observations are menu visibility, campaign visibility, and fresh logs showing
runtime-device-downstream routing, submitted layers and any shader failures.
Local source logs and transformed game shaders remain outside Git/the report.

## PSVR2 image-quality follow-up / fix 2 (2026-09-18)

The tester's logs2.zip now accompanies a report of visible headset output and
120 fps, but very low detail and broken lighting, especially in the right eye.
The log confirms a 1280x720 game source against 2804x2860 OpenXR eye images at
100% scale. The per-eye crop is only 969x720. This is a confirmed source-resolution
mismatch; the log cannot measure shadow distance or prove the exact visual cause.

Changes:

- Native SFS VR now uses the existing headset-sized engine/surface path, including
  its scaled-window/core-window fallback. The non-VR probe and external provider
  retain their old behavior. For the tester's recommendation at 100%, the 16:9
  carrier becomes 5088x2862 per array layer. This increases GPU/memory cost; the
  previous 120 fps result does not predict performance at the corrected size.
- Promoted 2D comparison samplers read shared layer zero, matching the DOOM
  profile's mono shadow sampling. Ordinary scene-depth/color sampling remains
  per eye. This change does not globally disable stereo render passes.
- Recognized clustered-light lookups are mapped back to the centered camera
  before indexing its shared light-list buffers. The inverse uses the same
  projection uniform as geometry, including the depth-dependent eye translation.
  Valid cluster coordinates are clamped before conversion to buffer indices.
- Recognized deferred-lighting/fog world-position reconstruction receives the
  equivalent correction in its frustum basis. Existing profile corrections are
  removed when superseded, avoiding double displacement. These semantic anchors
  apply to original modules as well as hash-matched replacements. Unknown shader
  structures are left untouched; this is not universal shader compatibility.
- Once-per-compiled-module diagnostics record profile match, vertex projection,
  stereo compute, cluster correction and world correction counts. They make
  unmatched AMD variants visible in the next tester log.

Validation: Release build and 112/112 CTests pass, including actual local Vulkan
GPU readbacks for shared comparison shadows, distinct scene images, centered
light clusters/world positions, and asymmetric eye projections. Rewrite tests
cover profile replacement without double correction and untouched unknown code.
All 722 local original/profile modules compile; 56 receive cluster corrections,
14 world-position corrections, and 66 contain comparison samplers. The tester's
complete AMD shader set was not supplied, so this is not an AMD replay.

A bounded 45-second Simulator run (PID 13612) used recommendation 2064x2208,
scale 50%, source 1984x1116, and reached at least 1440 submitted quad frames with
zero logged lifecycle violations/end-frame errors, then stopped at the deadline.
This validates startup and resolution plumbing, not campaign lighting or PSVR2
hardware. The existing AER-derived weapon, hand, locomotion and source-pose paths
are retained. Real Radeon/PSVR2 visual verification is still required.

Complete tester package: out/beta096/psvr2-steamxr-fix2/ and
out/beta096/KHARVOX-0.96-SFS-PSVR2-SteamXR-fix2.zip. The runtime bundles both approved
integration DLLs and both bridges and must pass the mandatory package verifier.

## logs3.zip: shared shadow generation and oversized allocations / fix 3

The tester confirms that fix 2 at 80% renders the right eye normally, but an
object changes from lit to shadowed to lit as the player approaches. logs3.zip
records 4096x2304 source at 80%, 567 shader variants, **zero** profile matches,
236 projected vertex variants, 36 cluster corrections and six world corrections.
Thus the Radeon run uses the generic compiler throughout; its shader hashes do
not activate the supplied profile's per-pipeline exceptions. The logs do not
contain the AMD SPIR-V or a capture of the reported rock, so they cannot directly
measure its shadow coverage.

The Vk3DVision DOOM ShaderSwap files explicitly distinguish shadow and camera
variants of the same vertex shader. For example, a760518252dc0c5e variant
b299d288a841675b contains no stereo position adjustment, while dd9957bdd0bd9a41
does. Captured pipeline packets associate the former with biased depth-only
shadow work. Several other supplied variants have the same distinction. The
profile also has a deliberate biased-depth HUD replacement; exact replacements
must retain precedence over any general classification.

Fix 3 therefore adds a generic fallback for depth-only, depth-writing,
LESS_OR_EQUAL, depth-biased pipelines: keep the light-space vertex projection
unchanged. It applies only without a matching replacement. Camera depth passes
retain stereo projection, color/decal passes are excluded, and cached module
identity/diagnostics include the shared-shadow role. The shared comparison
sampling introduced in fix 2 is preserved.

The e671b5dec2461b5c material-atlas profile likewise leaves raster position
unshifted. Its original shader contains mvpmatrixw only for a varying, yet our
old member-presence heuristic shifted its atlas position. The compiler now
recognizes the explicit atlas-coordinate raster pattern and excludes it from
headset projection even without a profile match. Other uses of in_VmtrTC are
not classified as atlas rasterization merely by their input name.

The prior failed launch in logs3 contains imageBytes=154009616, flags=0x4,
poolBlockBytes=134217728. Native SFS now routes oversized image requests through
DOOM's existing bit-0 standalone allocation path instead of its fixed-size pool.
Other allocation flags and all arguments are preserved; ordinary images/buffers,
uninitialized capacities and AER behavior remain unchanged. The supported binary's
wrapper, flag extraction, branch and ownership-record bytes are checked before
enabling the hook. DOOM owns allocation/freeing and handles actual allocation
failure; this does not manufacture VRAM or remove hardware limits.

Validation: 114/114 CTests and 722/722 local shader translations pass. GPU
readbacks verify unshifted shadow raster coverage and, with the same vertex
module, shifted camera-depth coverage. Atlas/compiler tests cover the misleading
MVP member. Allocation tests cover the strict size boundary, flag preservation,
non-SFS and buffer exclusions. A 45-second Simulator test (PID 9028), with no
matching ShaderSwap files and scale 150%, used source 5888x3312. Seven real image
requests of 156762112 bytes bypassed the 128-MiB pool successfully; 24 vertex
variants used shared-shadow projection; no shader compile errors were recorded.
At frame 1080 it submitted projection layers with zero lifecycle violations or
end-frame failures, then was stopped at the deadline. This exercises the generic
path on the local NVIDIA GPU; it is not Radeon/PSVR2 visual verification.

Evidence: out/beta096/psvr2-tester-logs3/, fix3-simulator-unmatched.log,
fix3-shader-audit/result.json and fix3-ctest.log. Complete tester delivery:
out/beta096/psvr2-steamxr-fix3/ and
out/beta096/KHARVOX-0.96-SFS-PSVR2-SteamXR-fix3.zip. Compare the same rock and walk
at the previous 80% scale first; separately test 100% startup for the allocation
change. Fresh logs now expose sharedShadow=1 for unmatched shadow variants.


## Execution and presentation audit, 2026-09-18

Scope correction: the requested target is the provider's complete efficient
stereo workflow, including work sharing and presentation synchronization, adapted
to full VR. The user explicitly permits NVIDIA-only support for this renderer.
Do not trade that target for AMD compatibility or assume that generic multiview
alone establishes equivalent performance. Existing AER behavior remains a separate
compatibility requirement.

Static evidence below applies only to provider SHA-256
`b2ec1ac73a4bdb679c5b7a32286c5acdf3bd84d52e416d98ba2965fcd8646aba`.
The extended `tools/inspect_sfs_provider.py` reproduces the bounded disassembly
without loading the DLL. RVAs are relative to image base, not file offsets.

| Path | Evidence | Consequence |
| --- | --- | --- |
| Device dispatch | CreateDevice at 0x1a4380 stores resolved function pointers | Resolve downstream functions once per device, not on each command |
| QueueSubmit | 0x1ade80..0x1adee5 forwards arguments and tail-jumps through dispatch+0x20 | This wrapper itself does not repeat game submissions or insert waits |
| Descriptor binding | 0x1a3690..0x1a36e0 tail-jumps through dispatch+0x2c0 | It does not do KHARVOX's command-state replay bookkeeping in this wrapper |
| Normal acquire | 0x1a2811..0x1a2836 cycles a synthetic index over the vector at object+0x12d0; unchanged-size path returns success | The provider owns its source-image acquisition; it is not just our real desktop WSI acquire followed by XR |
| Present helper | 0x18eef0 computes frame counter modulo 5 and selects slot-specific objects at +0x11d0 and +0x1258 | A five-slot presentation work ring is observable; do not infer five-frame headset latency |
| Stereo copy | 0x18bdd0 constructs image-copy regions and calls the copy dispatch at 0x18bf55 | Output still involves a GPU image copy; zero-copy is not established |
| GPU interop | 0x18c03e writes sType 0x3b9beef8 (1000075000), fills acquire/release arrays, links it to submit at 0x18c094, submits at 0x18c0d0 | This is VkWin32KeyedMutexAcquireReleaseInfoKHR, with acquire key 1 and release key 2. The structure itself is KHR, not proof of a proprietary NVIDIA stereo primitive |
| Queue bridging | 0x18f160 compares queues; alternate branch submits a signal and appends a semaphore to the consumer waits | Queue dependency propagation is part of the implementation, not merely copying pixels |

The status/wait calls at 0x18ef86 and 0x18efb2 are also observable. Their
private-table mapping must be completed before asserting exact wait semantics;
this audit does not establish an entirely wait-free provider. The acquire
semaphore/fence signaling contract, external allocation and D3D/VR consumer
ownership still need to be traced. Copying only the synthetic acquire index would
break synchronization and is not an implementation of this transport.

Earlier speculative explanations are not supported by the current evidence:
- Both implementations broadly expand eligible 2D render targets/storage images.
- The supplied DOOM ComputeDispatch exception list is empty. A broad reduction
  of compute work in the reference has not been demonstrated.
- The additional KHARVOX device-idle wait is not a measured major bottleneck in
  the menu test below. Gameplay may differ.

Implemented in this audit:
- A typed, immutable per-device table resolves 59 downstream entry points during
  SFS initialization. Command recording and binding replay no longer repeatedly
  call vkGetDeviceProcAddr. Tables are not shared between game/runtime devices.
- Real GPU integration tests count resolutions and require zero additional
  lookups after initialization, including rendering and resource destruction.
- Optional KHARVOX_SFS_PROFILE_TIMING=1 reports device-idle and uniform-upload
  timings every 120 installed frames. It does not remove any retirement barrier.

Validation: Release build and 114/114 CTests passed. Approved bundled bridge/DLL
verification passed for out/beta096/native-runtime. Simulator PID 26776 ran for
45 seconds at 50% render scale on the local NVIDIA GPU and was stopped at its
owned deadline. This was a menu/quad run, not campaign or physical-headset
validation. At cycle 1800 there were zero lifecycle violations, discarded frames
or end-frame failures. Sampled idle means were about 0.022-0.026 ms, uniform upload
about 0.0011-0.0015 ms. At frame 1800 the existing copy-fence diagnostic was
1.5212 ms, and XR handling 3.4265 ms; those scopes include preceding GPU work and
CPU handling, not just GPU copy execution. They are not an A/B speedup claim.

Evidence: out/beta096/perf-reference/{manifest.json,*.asm,build.log,ctest.log,
probe.log,simulator.log}. Native test runtime is updated; fix3 tester archive is
unchanged. The complete virtual-source/interop-ring transport has NOT yet been
implemented. Next work must close its semaphore/fence and external-image ownership
contract, then replace the desktop-WSI coupling and measure the same gameplay
scene with matching per-eye resolution and quality. The original provider-only
heap-corruption reproduction still prevents a valid local provider A/B baseline.

## Owned-source transport test, 2026-09-18

The follow-up implements a native Vulkan/OpenXR adaptation of the provider's
owned-source acquisition. KHARVOX_SFS_SOURCE_RING=1 enables it only with native
SFS VR; the prepared launcher's sfs_source_ring marker selects it automatically.
The test selection is Vulkan SFS Source Ring (NVIDIA Test), requiring NVIDIA.
AER clears these settings and keeps its existing output path.

SourceRing.h allocates dedicated, device-local two-layer source images and
returns synthetic swapchain handles. Acquisition signals the application's
semaphore/fence through a real queue submission. Present consumes application
wait semaphores exactly once (XR copy or retirement) and fences source reuse.
Source images use GENERAL instead of PRESENT_SRC_KHR; render-pass, application
barrier and XR-copy transitions are translated together. Desktop WSI acquire
and present are bypassed. The DOOM desktop window is black by design; menus
continue through the existing XR quad path.

DOOM explicitly requires TWO swapchain images: after enumeration, executable
RVA 0x1904cb5 compares the count with 2 and 0x1904cba branches to the error path.
An initial five-source trial reproduced that error. The implementation now honors
the requested count (two in DOOM). The provider's modulo-five presentation work
ring is NOT its DOOM source-image count. This test does not implement that
five-slot work ring or claim five frames in flight.

For eligible SteamVR/VDXR projection frames, the owned-source path can use the
existing ordered early-release policy: submit XR copies, release/end the XR
frame, then wait for the private copy fence before reusing command buffers,
parameters or hand resources. Readbacks retain synchronous completion. This is
not a wait-free or fully pipelined renderer. The same-device OpenXR Vulkan
consumer does not need the reference's Vulkan-to-D3D keyed-mutex bridge.

The simulator additionally needs Windows messages pumped on its idle XR worker:
its preview window is created there, and a synchronous WM_ACTIVATE from DOOM
otherwise deadlocks while that worker waits for a new task. Pumping is restricted
to the simulator. Focus switching then continued without the previous hang.
Production runtime thread behavior is unchanged.

Validation: Release build, 116/116 CTests, launcher self-test. Two new real-GPU
tests repeatedly compare both layers' frame/eye-specific pixels, semaphore/fence
signaling, exhausted acquisitions and swapchain replacement. The second goes
through NativeSfs, including multiview render-pass and PRESENT-to-GENERAL layout
translation. Bounded simulator runs reached the menu without XR lifecycle errors.
Automatic campaign loading was unavailable (Unknown command 'loadGame').
Automated menu interaction also failed in the ordinary WSI baseline. Campaign
and physical-headset behavior remain unverified. Early release is covered by
policy tests, not a local SteamVR headset run.

Artifact: out/beta096/KHARVOX-0.96-SFS-Source-Ring-NVIDIA-test1.zip.
Evidence: out/beta096/source-ring-ctest.log, source-ring-probe*.log,
source-ring-baseline.log and the final packaged-runtime probe.
Equal performance or complete equivalence with Vk3DVision has NOT been
established. Comparable gameplay at matching per-eye resolution/quality and a
working reference-provider baseline remain necessary to establish that claim.
## Source Ring test 2: eye-sized rendering and GPU copy timing

The VDXR/RTX 4080 SUPER report used 100% scale, 4800x2700 per source
layer, versus 2496x2688 per XR eye. The legacy source sizing kept a 16:9
carrier and rounded height to multiples of 18 (2688 -> 2700 -> width 4800).
This was inherited from the AER camera carrier, not requested supersampling.

Source Ring now sizes each source layer to the maximum recommended eye width
and height, applies the selected linear scale and rounds to even pixels.
Its frame uniforms project directly into each eye's actual FOV; submission
keeps that same FOV and therefore no longer crops a wide intermediate image.
Ordinary SFS and AER retain their prior sizing/projection. For the reported
VDXR configuration this removes 48.23% of source pixels, not necessarily that
percentage of total GPU time. Physical-headset visual confirmation is pending.

Optional Vulkan timestamps in the XR command buffer measure its copy/hand/HUD
work independently of the CPU submit-to-completion interval. Results are read
without WAIT_BIT, only after the existing completion boundary; unsupported
queues skip timing. Source Ring also enables existing parameter/device-idle
CPU timings. These do NOT directly measure every DOOM render pass or establish
that device-idle is the bottleneck. No retirement fence was removed, and full
multi-frame resource pipelining remains unfinished.

Validation: 116/116 tests, including repeated real-GPU timestamp reads alongside
both-eye pixel checks and existing asymmetric projection tests. Packaged-runtime
simulator menu smoke test produced timestamp samples and successful XR frames.
This is not gameplay or VDXR hardware performance verification.
Artifact: out/beta096/KHARVOX-0.96-SFS-Source-Ring-NVIDIA-test2.zip.
## Source Ring test 3: CPU command-recording overhead

Performance work targets the measured layer cost without changing image quality,
source resolution or removing GPU lifetime protection. Changes:
- Per-thread weak device-dispatch caches, invalidated by registry generation at
  initialization/teardown, avoid the global device map lock on each command.
- Resource metadata uses shared/exclusive locking. Parallel Vulkan command
  recorders read metadata concurrently and modify only their own command state.
  Command entries are allocated before recording, never inserted by readers.
  Creation, destruction and metadata updates retain exclusive access.
- Push constants use reusable per-stage word/layout storage. Partial writes,
  differing layouts, stage masks and reset semantics are preserved. Contiguous
  ranges replay in one driver command instead of one captured callback per word.
- Pipeline and descriptor state use typed reusable storage, avoiding captured
  function/map allocation on each bind. Descriptor dynamic offsets are retained.
- Command-buffer reset retains push/descriptor storage capacity.

Sampled instrumentation (one hook in 64 per recording thread) reports lookup/lock
and hook-body time. Aggregate estimates sum durations across recording threads;
these are not CPU frame wall time or GPU time. Resource creation is not included.
The same local simulator/menu configuration showed roughly 3 ms aggregate hook
cost before shared recording versus around 0.7 ms afterward. Sampled lookup/lock
means fell from about 3 us to about 0.05 us. This is not a gameplay/VDXR benchmark
and does not establish Vk3DVision parity.

Validation: 117/117 CTests; 1000 deterministic mixed-layout/stage/partial push
write comparisons against a per-word reference; 256-byte replay reduced from
64 calls to one. An isolated 10,000-iteration CPU microbenchmark measured the old
map/callback pattern at 33.6 ms and the new storage at 0.69 ms. This number must
not be advertised as whole-game speedup. The real-GPU runtime test also records
1200 command buffers on four threads with separate pools while resource metadata
is created/destroyed. Existing before-renderpass pipeline/descriptor replay and
both-eye GPU pixel tests passed. The final package was smoke-tested in the
simulator and integration binaries verified.

Artifact: out/beta096/KHARVOX-0.96-SFS-Source-Ring-NVIDIA-test3.zip.
The frame-boundary GPU waits and shared parameter buffer remain. Deferring
completion across Present is not safe without also retaining borrowed game
images/views, hand framebuffers and readback ownership. No full multi-frame
resource ring or equal-performance claim is made. The next VDXR comparison should
use the same scene, render scale and settings as test 2 and inspect both Game
Latency and the new command CPU timing lines.
## Source Ring test 4: indirect compute and mixed-pass diagnosis

vkCmdDispatchIndirect is now intercepted. Generic compute pipelines with stereo
storage-image output have two fixed-eye variants compiled at pipeline creation.
Each variant executes the original GPU-owned grid and logical workgroup IDs;
counts are not read back or rewritten. The original compute pipeline is restored
before returning. Shared-buffer-only compute remains one dispatch. This closes
a definite unhandled path; it does NOT prove the reported blink light uses it.
Profile-replacement stereo compute does not yet have a verified fixed-eye rewrite;
if used indirectly, it stops explicitly instead of silently rendering one eye.
The observed game profiles in the current logs use generic compute variants.

Framebuffer creation reports mixed mono/stereo attachment details (bounded to
24 framebuffers), and frame counters report executed mixed passes and shared vs
stereo indirect dispatches. Mixed framebuffers are not blindly promoted: a shared
attachment can make that incorrect. These counters are diagnostic evidence, not
proof that any particular visible effect uses the recorded pass.

CPU work: image-barrier and clear-range scratch vectors are retained per command
buffer, removing repeated allocations in those hooks. Existing parallel metadata
access, typed pipeline/descriptor state and batched push constants remain.
No image quality reduction or GPU lifetime wait removal was made.

Validation: 118/118 tests. A new real-GPU integration test writes indirect counts
on the GPU, dispatches stereo image work, follows it with a direct dispatch to
verify pipeline restoration, and checks both eye layers over 20 frames. It covers
zero-sized dispatches and Z=3 logical grids. A shared-buffer-only shader uses an
atomic counter to prove its 48 invocations are not duplicated (and zero stays zero).
The packaged simulator menu run reports zero indirect/mixed passes in sampled
menu windows; this does not settle the campaign effect diagnosis. Hardware
verification of the reported particle/light defect and performance is pending.

Artifact: out/beta096/KHARVOX-0.96-SFS-Source-Ring-NVIDIA-test4.zip.
Fresh user logs must include [SFS-PATHS] and any [SFS-MIXED] entries. GPU completion
waits/full multi-frame resource pipelining remain separate unfinished work; this
package does not claim Vk3DVision performance parity or a confirmed visual fix.
## Source Ring test 5: Quad aspect and targeted diagnostics

The test 4 source was 2496x2688 per eye, but its full-frame Quad used 16:9
physical geometry. The shared menu/pause/Cinewindow path now derives height
from the submitted subimage extent. Invalid extents retain the legacy fallback.
Tests cover the reported source dimensions, legacy 16:9 and invalid extents.

Controller publication timestamps now remain attached to AerWeaponInput across
source-history latching. Sampled prop logs measure publication-to-prop age and
include source/current Present serials. This is not sensor-to-photon latency.
No unproven change to the coherent camera/weapon/prop binding was made.

Extended logging maps blended graphics pipeline variants to shader hashes and
depth/blend state at creation (bounded to 512 candidates). This identifies
candidate transparent passes, not a proven faulty window/reflection material.
The diagnostics neither add draw-path formatting nor change GPU waits.
Weapon latency and reflective materials still require targeted reproduction;
no performance gain or complete Vk3DVision equivalence is asserted.
## Source Ring test 6: cropped Quad, source poses and glass refraction

User requested a monitor-shaped crop after the aspect-preservation fix. The
submitted Quad now keeps its horizontal range and crops top/bottom centrally
to 16:9, preserving shorter authored images. Tests cover 2496x2688 -> 2496x1404,
legacy 1920x1080 and an already shorter image. Physical aspect follows the crop.

Test 5 logs contain the matched glass fragment profile
23d4de41fcd07b9b_dad36c6fdd6d4e44. It projected refraction into centered window
coordinates and applied only the fixed-display horizontal stereo adjustment.
Recognized globalpostowindow/scenemip refraction now uses the full homogeneous
per-eye transform before perspective division. The rewrite handles generic and
profile forms and removes the prior adjustment only with the complete anchors.
Numerical tests compare both UV axes against independently projected points at
multiple depths and asymmetric FOVs; the actual glass profile recompiles.
This is not a claim to fix every reflection/SSR variant.

SFS previously labelled its image from Acquire even when the CPU producer used
an older camera snapshot. A bounded source history now qualifies gameplay poses
against the existing read-only world producer observation. Uniform compatibility
is checked before replacing pose/controllers; geometry mismatch or ambiguity
withholds the pair. Missing observations retain acquired metadata. No additional
GPU wait is added. Source-history tests cover delayed poses, level mismatch,
ambiguity, incompatible FOV and stale history. Simulator logs show qualified
acquired/observed pairs such as 923/922 and 3007/3006. Headset confirmation of the
reported world wobble and glass artifacts remains pending.

Validation: 118/118 CTests, actual glass profile compilation, launcher self-test,
60-second bounded simulator run and approved bundled integration verification.
Additional shader audit (test 6 final):
- All 75 profile modules and 647 captured original modules compile: 722/722.
- Found incomplete previous-frame projection in velocity/temporal shaders,
  including compute. Previous world-to-window coordinates now use the previous
  installed per-eye projection and eye translation, after current-eye world
  reconstruction. Previous uniforms are latched at frame installation, not at
  prediction publication. Shader logs report temporal=1 when this applies.
- GPU regression checks stationary geometry yields zero artificial motion for
  both eyes, including asymmetric FOV. 118/118 CTests pass after this addition.
- All recognized world_pos/frustumVec, refr_tc and winPosPrev candidates in the
  captured audit have the corresponding correction. This is pattern coverage,
  not proof that every game material and temporal effect is visually correct.
## Source Ring test 7: GPU particles and moving weapon body anchors

User test 6 reports stick-movement weapon flicker in both eyes and monocular
sparks. Runtime path counters remain zero for indirect stereo and mixed mono
passes; duplicating those paths again is not supported by these logs.

The captured GPU particle VS 6fb890e92d8bf507 uses separate viewmatrix and
projectionmatrix rows rather than mvpmatrixw. Its active runtime variants were
projection=0. Generic projection recognition now includes active viewmatrixw +
projectionmatrixw and direct viewprojectionmatrixw. Declared but inactive rows
do not opt in; existing atlas/shared-shadow exclusions remain. A real-GPU
regression uses separate view/projection members and verifies per-eye pixel
coverage. Simulator confirms the original particle module now uses projection=1.

Weapon source transforms intentionally froze AER pairs by tracking ID. In SFS,
that ID can outlive a body simulation update during locomotion. The draw path
now receives the actual observed camera origin. Valid bounded origin changes
shift the historical body anchor, and the cached model/animation is rebased from
its recorded controller frame into the matching draw frame. Draw-only cached
snapshots now retain the metadata of their actual target transform. No new
controller prediction is introduced and AER retains its pair policy. Tests
cover repeated-ID translation, non-accumulating repeated draws and rejected large
origin discontinuities. The precise headset symptom still needs user validation.

119/119 tests pass, including source/weapon, GPU stereo and PSVR2/bHaptics suites.
Additional HUD fix:
Known DOOM UI shader families now preserve the reference screen/world split:
clip-W <= 8 keeps screen overlay geometry, while world UI keeps full eye
projection. This applies consistently to generic/profile variants, including
HUD mask shaders. Legacy TV shifts are removed before the single VR transform.
The prior unconditional world IPD shift could move flat overlays outside one
view. Level-start notification visibility still needs headset confirmation.
119/119 tests pass after this addition. Earlier broad audit compiled all 722
captured/profile shader modules; final startup checks exercise the UI variants.
## Test 8: asymmetric screen HUD and SSDO view-space projection (2026-09-18)

User reported binocular LOW AMMO duplication in test7 and a strong shading
artifact while moving the head over a grating. The live VDXR session was kept
running; its logs were saved as out/beta096/test7-user-hud-shadow.log and
out/beta096/test7-native-hud-shadow.log. Desktop capture was black as expected
for the source ring. CaptureEyes was false, so no live eye-image evidence was
available. The precise grating material and visual result remain unconfirmed.

The test7 UI guard wrongly skipped the asymmetric HMD FOV transform together
with world IPD for clip-W <= 8. Equal NDC coordinates do not describe equal rays
in asymmetric eye frusta. Apply the FOV transform for all known UI draws and
restrict only the IPD translation to the existing world-UI branch. Numerical
projection tests cover both sides of the boundary; UI-aware CLI shader audits
now exercise the same compiler option as the runtime.

The live log also loaded generic SSDO variant 2bae4272f89ac0d2_5bdf1a64ae6c74fc.
Its GetViewPos and GetWindowPos used centered packed projection coefficients on
per-eye depth buffers. The reference profile disables SSDO for a DIFFERENT exact
variant, d05ae7382167326c; that replacement does not cover this active generic
variant. The correction recognizes both complete helper bodies plus SSDO/depth
semantics, maps eye UV to centered ray UV for reconstruction, and maps hemisphere
sample UV back to the eye. Calculations remain eye-local, so no world IPD offset
is added to relative occlusion samples. Unknown/partial functions stay intact.
The runtime reports ssdo=1 when this correction is installed.

Validation: 119/119 CTests pass. The lighting GPU fixture checks independently
known reconstructed positions and projected samples in asymmetric frusta, not
only a round trip. Existing shadow-map and camera-depth tests remain green.
The new DLL does not change frame submission or duplicate geometry work. No
headset visual fix or performance gain is claimed from these automated tests.
A new simulator run is deliberately omitted while the user's DOOM is running.The final audit compiled 722 modules (647 captured originals and 75 profile
replacements) without errors, including runtime-equivalent UI options. Six
original SSDO modules matched the complete reconstruction/projection pair.
## Test 9: packed world depth incorrectly treated as HUD (2026-09-18)

User clarified the Argent Energy Tower grating becomes black only while standing
on it, with the dark region moving with the head. This does not establish a
shadow-map failure; live eye images remain unavailable. Test8 logs confirm the
SSDO correction installed, so that change alone did not resolve the report.

Found a separate concrete classification error: original shader family
5d8a0b69a38eb2a0 is packed virtual-textured geometry (vertexxyzscale, in_VmtrTC),
but exact replacement 37b4725b9f3bfe03 uses a UI vertex layout. The active generic
variant 673629e185c7ad71 inherited the family-wide screenUi flag and skipped IPD
below clip-W 8. This can mismatch near world depth against other material passes.
Classify the compiled source: packed virtual-textured geometry keeps full world
projection. The real UI replacement keeps the HUD exception; unshifted shadow
replacement 1613515c70b06cc6 and atlas exclusion remain unchanged.

119/119 tests passed, including a new packed-world compiler regression fixture.
All 23 affected original/profile UI-family modules compiled; inspected the real
5d8a generic output (unconditional IPD) and UI replacement (conditional IPD).
The specific grating scene still requires headset verification; no assertion
that this explains every black surface, and no measured performance change.
## Test 11: moving weapon candidate comparison and AMD source-ring access

Test9's grating correction was confirmed successful by the user. Residual
weapon flicker remained during walking. Saved logs: test9-user-before-test11.log
and test9-native-before-test11.log. Weapon draw diagnostics include status=4
(ambiguous target); status=0 also exists and is not assumed fixed by this change.

SFS previously compared root/prop target snapshots in their recorded world/body
anchors, before rebasing the selected result to the draw camera. Two equivalent
controller-relative poses recorded around a movement step could therefore be
rejected, returning the original draw pose for that invocation. Normalize each
candidate to the actual draw frame BEFORE comparing. Use the existing bounded
pose-roundoff tolerance for SFS comparisons, including derived candidates.
Actual ambiguity, source/level/epoch/calibration checks remain. AER keeps exact
pair comparison. Tests exercise movement-straddling root/prop snapshots,
repeat draws without accumulated motion and genuine animation disagreements.
119/119 CTests pass; complete absence of headset flicker is not yet established.

The source ring uses core Vulkan images, memory, fences and queue submissions.
Remove its NVIDIA vendor-ID rejection. Existing multiview and successful resource
creation checks remain; require at least two image array layers. Log GPU/vendor
and capability-based admission. Existing AMD legacy-group-vote compiler handling
and generic lighting/projection transformations remain. Earlier tester logs show
RX 9070 XT, but no AMD device is available here to validate the new source ring.
Launcher label/description now expose NVIDIA/AMD test status without declaring
AMD hardware compatibility. No imported Vk3DVision DLL or NV-only source-ring
command is introduced. Launcher build identifies test11.

Cinematics and Glory Kills already enter the shared presentation/camera policy.
SFS FramePose includes cinematic/scripted/gameplay contexts and enables world
projection for all three; menu/quad context remains mono on the virtual screen.
Immersive and other-cinematics-in-quad options, Glory Kill exceptions and boss/
HUD-movie guards remain active. This source review is not per-sequence headset QA.
Test 12: preserve held weapon source metadata
- Repeated root evaluations can receive a newer body camera under the same tracking pose ID. hold() kept the first world transform but its caller passed the newer camera to child props. SFS draw rebasing could therefore disagree between root and prop while walking.
- Return the actual held frame with the held transform; propagate it to the root/child binding and report actual held prop metadata in traces. Immutable AER pair behavior and strict draw ambiguity checks remain intact.
- Regression reproduces the mixed-camera failure before the fix and verifies stable movement correction across repeated draws afterward. Headset confirmation of remaining flicker is pending.
- Render Scale and optional FSR1 already support valid SFS stereo pairs; FSR1 requires scale below 100 percent and is bypassed for quad presentation. Latest user run used 100 percent, FSR disabled.
Test 13: current animated grip at the completed weapon prop
- Supplied stereo video VirtualDesktop.Android-20260918-174032-0.mp4 shows a one-frame whole-weapon displacement at about 23.056 seconds relative to a comparatively stable custom hand/world. User reproduces it on gratings/pipes, not smooth ground. This favors a pose/animation discontinuity over missing material coverage, but does not prove its exact CPU cause.
- The existing root computes its mount from animatedGripPivot, sampled only after originalUpdateHandsTransform returns. Child transforms inside that update can already reflect a newer animated joint.
- For SFS only, at the verified final prop-axis caller, query the current grip joint and cancel its world-space residual against the controller from the root's held source. Use the actual held root origin/axis and configured grip adjustment. Apply once before child source capture; presentation reuses the corrected child and does not apply the correction again.
- Reset root-source validity at SFS hands-update entry so a skipped root cannot lend the previous update's source to a child. Keep AER, child rotation/internal animation, source-epoch guards, cinematics, and collectible handling intact. No temporal smoothing, shader changes, added GPU work, or NVIDIA extension.
- Regression covers alternating terrain-like joint offsets with rotated/scaled roots and grip calibration, invalid input and oversized corrections. Runtime logs [SFS-WEAPON-GRIP] report bounded correction samples for the next hardware run. Headset elimination of flicker is unverified until retested.
Test 14: coherent SFS draw views and rendered laser poses
- Test13 user reports uneven weapon motion even when standing, plus laser jitter during hand/player movement. Latest logs show approximately 120 SFS frames per second and roughly 12-17 ms controller-publication-to-prop age; neither establishes the number of distinct drawn controller poses. No fixed 60 Hz divider was found in SFS publication (phase zero is refreshed every XR frame).
- Camera history permits repeated publication under one tracking pose ID. SFS draw alignment previously read the mutable body-to-view offset for every model draw; asynchronous depth/material jobs could therefore use different mappings when the body/stance changed. Latch the first aligned source frame by tracking/scene ID, exact real view origin and basis, epoch and generation. Different tracking samples/views still advance independently; AER is unchanged.
- Laser was sampled at the animated prop, before the draw-time correction. Record its source model transform, match it to the verified drawn weapon pose, transform the muzzle/direction through that exact correction, and prefer the draw-bound sample when compositing the same SFS source. Existing raw-source fallback remains and is counted rather than hidden. No independently newer controller sample, interpolation or extra GPU work.
- New [SFS-WEAPON-CADENCE] logs distinct draw views and controller publication samples per measured interval; [SFS-LASER-SOURCE] counts draw-bound, animation-fallback and missing samples. Counts are CPU diagnostics, not hardware proof of smooth 120 Hz motion.
- Regressions cover camera republishing during one view, 120 independent tracking IDs with a slower body update, changed actual camera basis, resets, and rotated/scaled muzzle correction with safe invalid-matrix rejection. Headset confirmation remains pending.
Test 15: separate mono auxiliary pipelines from headset stereo pipelines
- Test14 logs include 240 drawn views and up to 240 changed controller poses per 2000 ms; they do not support a blanket 60 Hz weapon cap. User suspects glass/reflections. Causation of the visible weapon flicker remains unconfirmed.
- NativeSfs graphics() compiled one stereo-adjusted stage set and used it for BOTH original mono and multiview render passes. Consequently a one-layer auxiliary pass could receive left-eye IPD/FOV, clustered-light, refraction and reconstruction corrections intended for the headset.
- Mono pipelines now compile the original game modules with descriptor-array promotion but without headset projection/lighting corrections or external stereo profile replacements. Multiview pipelines retain their existing profile and eye corrections. Shader cache keys explicitly distinguish mono variants. This does not disable reflections, change weapon smoothing or add render passes.
- GPU lighting/SSDO tests now draw the same pipeline into stereo and one-layer framebuffers and read back both results: stereo values remain unchanged; auxiliary output matches the native centered result. Vertex regressions check that the mono variant does not contain headset projection. All 119 tests pass. All 612 captured original graphics modules compile in mono mode (test15-mono-audit/results.json).
- Headset confirmation at the reported windows/reflective surfaces is still required. CPU pose recognition already requires exact observed main-camera pose/FOV; no speculative widening of reflection-camera matching was added.
Test 16: use the rendered source camera as the SFS weapon body origin
- User describes a one-frame whole-weapon displacement in walking direction, including pipes/gratings, and an apparent hand-calibration mismatch. Test15 shader isolation did not eliminate it.
- CameraHook reconstructed the weapon body origin from independently published player physics plus filtered stance height, while SFS custom hands are composed relative to the captured render camera. A physics tick ahead of that camera translates the weapon forward (or vertically at steps), independently of controller motion. Per-view latching cannot remove this offset between successive frames.
- Capture the actual native source-view origin before physics reconstruction and before HMD translation modifies the camera in place. SFS weapon snapshots use that origin; existing draw alignment then follows the actual rendered view without adding room-scale motion twice. AER retains its existing physics anchor. Physics, stance, global body state and saved calibration files are not changed.
- Regression simulates walking with intermittent one-tick physics lead, pipe-height discontinuities, camera copies and room-scale offset. The grip must remain fixed relative to the rendered camera; the AER branch retains the physics origin.
- [SFS-WEAPON-ANCHOR] excludedPhysicsDelta logs the discrepancy being excluded from the SFS mount. User test15 hand_models.cfg and hand_models_calibration_default.cfg match test14 hashes; their packaged calibration was not changed.
- This fixes the identified coordinate-source mismatch; confirmation of the reported transient flicker and hand alignment still requires a headset run.