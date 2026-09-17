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
  No shader mutation occurs in the live game. Capture exceptions never escape
  into the Vulkan application. Captured module memory and new SPIR-V file data
  each have a 128 MiB per-process budget; pipeline records are limited to 100,000.
- `KharvoxSfsProfileAudit`: compiled-profile resolution against captured pipeline
  identities. No external provider code executes.

## Evidence

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

## Work still required for the requested renderer

The resource core is exercised by tests, not yet enabled in DOOM. Enabling it
alone would mismatch existing shaders, image views and descriptor bindings.
Complete the common shader transformation, mono/stereo descriptor-view handling,
uniform binding and per-frame buffer lifetime together. Apply render-pass and
compute exceptions, and expand image barriers/copies to both layers where needed.
Then connect actual producer eye resources and synchronization to KHARVOX's
OpenXR submission, using headset FOV/IPD and one shared predicted frame pose.
Hand pose timing and menu transitions require real runtime validation afterward.

The original external provider's startup heap corruption is historical evidence;
fixing or loading that DLL is no longer a prerequisite for this native path.
