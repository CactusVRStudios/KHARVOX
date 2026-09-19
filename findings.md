# Native Replay Audit Findings

Reviewed 2026-09-19 at `17b2d01f5f3866a751f90000ee68b9fa88d3f654`, starting from `AI_REPO_MAP.md`.

Scope: native frame ownership, replay recording, descriptor/snapshot lifetime, resource retirement, image layout tracking, and their Vulkan/OpenXR callers. The findings below apply when the Native backend is installed. This was a source audit with focused CPU tests, not a DOOM/headset profiling session. Performance impact in milliseconds remains unmeasured.

## Correctness And Stability

### 1. [P1] Serialize the completion-time device wait with queue access

Location: `src/native/NativeStereo.cpp:427`.

`completed()` calls `real_device_wait_idle()` without taking the queue-access callbacks. Vulkan requires externally synchronized host access to all queues belonging to the device during `vkDeviceWaitIdle`. The engine submit wrappers take `queueAccessMutex`, but that protects nothing against this unlocked caller.

The call path confirms there is no enclosing queue lease: `vkQueuePresentKHR` calls `KharvoxXRPresent` and then `native::completed()` at `src/vulkan/KharvoxLayer.cpp:1288-1289`. XR's copy-submit lease ends inside `KharvoxXRPresent` (`src/openxr/OpenXRBootstrap.cpp:5208-5222`). Other engine submit threads or XR queue operations can therefore overlap the device wait. The game-image lifetime lease protects image retirement, not queue host access.

Impact: invalid concurrent Vulkan host access, with driver-dependent crashes, hangs, or validation errors. This requires concurrent queue activity; no live crash was reproduced here.

Recommendation: use the existing queue-access lease around this device wait, following `waitForMirrorRetirement()` at `src/native/NativeStereo.cpp:600-605`. Audit other raw device-idle callers for the same rule. Preserve the completion check and failure handling.

Verification: exercise owner completion concurrently with a worker queue submit under Vulkan thread-safety validation. A stub-dispatch test should also assert that completion invokes device-idle only while the shared queue lease is held.

### 2. [P2] Retire replay payloads when a command pool destroys its buffers

Locations: `src/native/upstream/src/hooks/vulkan_hooks/09_present.inc:5526-5530`; `src/native/NativeShaderAccess.inc:16-20`; `src/native/NativeFinalReplay.inc:329-335`.

`vkDestroyCommandPool` implicitly frees its command buffers. Its hook only calls `releaseStorageBindingBookkeeping()`, which clears storage masks and the snapshot batch cache. It never erases the corresponding `replayStates` entries. Those entries own command callbacks, push-constant byte vectors, descriptor payloads, and captured initial/final command lists.

The only replay-entry erasures are explicit command-buffer begin, reset, and free. Vulkan does not call the layer's `vkFreeCommandBuffers` wrapper when destroying a pool. If subsequent pools receive different buffer handles, the old allocations remain for the process lifetime. Handle reuse followed by begin happens to reclaim an entry; it is not a retirement policy.

Impact: retained CPU memory across pool churn, particularly level/resource rebuilds. This is a lifetime leak, not a demonstrated execution of freed command buffers.

Recommendation: record pool membership in the existing allocation hook and erase only that pool's replay entries after destruction. Share the cleanup with explicit command-buffer free; do not clear unrelated live command-buffer state.

Verification: repeatedly allocate, record, and destroy pools without explicit command-buffer frees. After each destruction, replay-entry and owned-payload counts should return to baseline. Include a second surviving pool to check isolation.

### 3. [P2] Track stencil state per face rather than by the face-mask value

Locations: `src/native/NativeFinalReplay.inc:533-534,542`; restoration at `src/native/NativeFinalReplay.inc:394,438` and `src/native/NativeEyeBindings.h:15-18`.

The stencil wrappers key saved state by `base + faceMask`. A `FRONT_AND_BACK` write and a later `FRONT` write remain as two independent entries. Restoration iterates the map in numeric order, so the older combined-face entry (mask 3) executes after the newer front entry (mask 1).

Concrete sequence: set both faces to 1, then set front to 2. The live state is front=2/back=1, but replay restores front=1/back=1. Compare masks, references, and write masks all share this defect. Both right-eye inherited-state restoration and final-pass initial/post-pass restoration use these saved entries.

Impact: incorrect stencil tests/writes in replayed rendering whenever the command stream mixes combined and individual face updates. The helper-level failure is reproduced; occurrence in a captured DOOM frame is not established.

Recommendation: expand combined-face writes into independent front/back saved values, while forwarding the original live command once. Preserve the original ordered commands inside the final-pass history.

Verification: a temporary C++ harness using the production `recordReplayCommand` and `restoreEyeBindingsOnce` helpers reproduced the stale override for all three key bases. Add regression cases for combined-then-front, combined-then-back, and individual-then-combined updates.

## Frametime Opportunities

### 4. [P2] Save current push-constant state instead of replaying its entire history

Locations: `src/native/NativeFinalReplay.inc:507-510`, `394`, and `438`.

Each `replayPush()` allocates an owned byte vector and appends a callback to `pushState`, including writes that replace exactly the same bytes and stages. At final-pass entry, the code copies the entire history into `initial`; it replays that history before the right final pass and again during post-pass restoration. History lasts until the command-buffer replay entry is erased.

For N writes to one range before a final pass, restoration performs N push commands where only the latest value is needed. It also copies the captured byte vectors when copying callbacks into `initial`. This adds command-recording work and allocation pressure proportional to earlier scene activity, rather than to the small amount of live push-constant state.

Recommendation: retain a compact current-state representation for initial/post-pass restoration. Keep ordered pushes in `r.commands` for actual final-pass draws. The repo already has `src/sfs/PushReplay.h`, which handles partial, stage-specific, and layout-tagged writes; assess reuse before adding another implementation. Do not replace the history with a single last-write value, which would lose partial updates.

Verification: compare effective push bytes/stages/layouts before and after replay, and count allocations plus emitted commands for thousands of same-range writes. The existing `sfs_push_replay_tests.cpp` passed its 1,000 partial/mixed-stage/layout/reset comparisons and demonstrated 64 calls reduced to 1 for its synthetic 256-byte case. That result validates the existing helper, not a measured Native speedup.

### 5. [P2] Replace the per-frame whole-device drain only after proving resource-specific completion

Locations: `src/native/NativeStereo.cpp:413-452`; `src/native/NativeGpuInputCopies.inc:40-49`; `src/native/NativeUniformSnapshots.inc:153-176`.

Every outstanding Native frame ends with `vkDeviceWaitIdle`, even after the owner XR copy fence has completed. This waits for unrelated device queues as well as stereo consumers and puts that work on the Present critical path. The normal snapshot path also submits and waits for a private copy fence for each eye with captured inputs; `gpuInputCopies=true` is a production default (`src/common/RuntimePaths.h:9-10`), despite the copy file's experimental wording.

These waits serve real lifetime requirements. The per-eye waits prevent the CPU from rewriting source inputs while copies still read them, and the device drain currently authorizes descriptor resets and deferred resource frees. Removing them without replacement would introduce corruption or use-after-free.

Recommendation: first measure the existing `DeviceIdle`, `GpuInputWait`, and `XrCopyCompletion` timing categories during steady gameplay and resource streaming. If device-idle contributes to tail latency, retire frame-private resources against the owner fence and retain completion tracking for any other queue that uses deferred resources. Keep the conservative drain for cases without a proven completion dependency. Treat the two input-copy waits as a separate measured comparison; the existing CPU-copy alternative is not automatically faster on write-combined mappings.

Verification: compare p50/p95/p99 frametimes and wait categories on the same scene, excluding capture frames and warmup. Exercise level loads, quality rebuilds, unrelated queue work, empty XR frames, and failed submissions. All consumers must complete before pool reset/free, and source writes must remain ordered after snapshot reads. No speedup estimate is justified by this source review alone.

## Implementation Follow-Up

Each finding has a separate commit on `odev/native-replay-audit-fixes`:

| Finding | Result |
| --- | --- |
| 1 | `6c5c120`: shared queue serialization for frame completion, mirror retirement, and descriptor arena teardown; missing callbacks fail closed. |
| 2 | `b319eee`: track command-pool ownership; retire pool-specific replay payloads on destruction and successful resets; preserve other pools and release all bookkeeping at device teardown. |
| 3 | `862e39c`: save stencil state independently for front/back faces while retaining one original command in live/final-pass streams. |
| 4 | `d052a9b`: compact fully superseded same-layout push writes, keep partial writes and original multi-stage masks, and retain immutable final-pass history. The SFS helper emits individual stage writes, so Native does not reuse it for arbitrary multi-stage layout ranges. |
| 5 | Extend the existing pacing analyzer with p99/max values and the ten slowest frame intervals with their individual wait spans. Device-idle removal remains deferred pending live completion/lifetime evidence; no GPU wait was removed. |

The new C++ regressions cover serialized device waits, command-pool churn/isolation/failures, stencil restoration and ordered final draws, and push replay ownership/partial writes/stage masks. CPU tests cannot establish multi-queue GPU lifetime safety.

Follow-up validation: MSVC Release `KharvoxLayer.dll` built with the default SFS-compiler setting (off); all 39 non-GPU `native-*` CTest cases and all six pacing-analyzer Python tests passed. The GPU depth-seed test, headset validation, and in-game profiling were not run. Optional PSVR2 Toolkit headers and the bHaptics SDK DLL were absent, so this was not a release-packaging validation.

For finding 5, launch with `KHARVOX_EXTENDED_LOGGING=1` and the existing `profile_native_frame_pacing` marker in the runtime directory. The existing recorder skips 120 ready frames and collects up to 4,096 consecutive rows. Analyze its `%TEMP%/native_pacing_sequence-<pid>.tsv` with:

```powershell
python tools/analyze_native_pacing_sequence.py "$env:TEMP/native_pacing_sequence-<pid>.tsv" --output native-wait-profile.json
```

Compare `withoutKnownDiagnosticNeighbors.fields` median/p95/p99 and `slowestFrames` across matching scenes. These are nested CPU wall spans, not additive GPU timings. Repeat with level loads, quality rebuilds, empty XR frames, and other queue activity before changing retirement dependencies.

## SPS/SFS Owner-Fence Follow-Up

The user narrowed the follow-up to SPS, named SFS in this repository. The
Native replay backend audited above is separate from SFS and from AER. This
change targets `src/sfs/NativeSfs.cpp::beginFrame`, not Native replay retirement
or alternate-eye rendering. The original Native finding 5 remains deferred.

SFS previously called `vkDeviceWaitIdle` before every frame-uniform upload,
even after the XR owner-copy fence had completed. It now records completion
evidence containing the device, queue, fence, rendered frame, uniform generation,
and submission sequence. XR captures it under the queue lock immediately before
the copy submit, then publishes it only after successful submission and fence
completion, on both synchronous and early-release paths. `xrEndFrame` success
does not authorize retirement.

SFS pair generation retains the acquired uniform-frame identity even when source
qualification selects an older camera pose. Completion checks use that generation,
not the independently qualified presentation pose.

All game command-buffer submissions are conservatively treated as uniform
readers. The layer observes `vkQueueSubmit`, `vkQueueSubmit2`, and its KHR alias,
including instance-proc lookup. Other-queue work, failed or later submissions,
stale frame/generation identity, swapchain changes, missing callbacks, and paths
without a valid stereo copy fence retain device-idle retirement. Empty source-ring
semaphore submissions do not read uniforms and do not invalidate the evidence.
The completion check, fallback wait, and uniform upload share the queue lock.
No deferred engine-resource frees or descriptor-pool resets use this evidence.

With `KHARVOX_SFS_PROFILE_TIMING=1`, the existing parameter timing log also reports
`ownerFenceRetirements` and `deviceDrains` per 120 uploads. The
`retirementMeanMs`/`retirementMaxMs` fields measure the retirement span, including
queue-lock acquisition; they are not GPU execution times. Compare these counters
and matching-scene frametimes before claiming a performance improvement.

The new `sfs-owner-completion` regression compiles the production SFS runtime
with stub Vulkan dispatch and unused shader-compiler stubs. It checks successful
fence retirement, failed waits/submits, missing/stale identities, delayed queue
submissions, swapchain invalidation, and exclusion of submissions during upload.
This is not a live multi-queue GPU or headset validation.

Validation: the Release layer build succeeded with the shader compiler disabled;
the new test separately compiled and exercised `NativeSfs.cpp`. All 43 selected
non-GPU CTest cases passed (four SFS and 39 Native). A compiler-enabled production
SFS package, live Vulkan synchronization validation, and headset frametime
measurements were not run.

## PR Review Follow-Up

### P1: Synchronization2 Alias Dispatch

Confirmed: a Vulkan 1.2 device may return a non-null core `vkQueueSubmit2`
pointer that the application must not call. The KHR entry point now retains and
calls its own downstream `vkQueueSubmit2KHR` pointer, without core/KHR fallback.
Both wrappers share the existing submission tracking and queue lease. The
`queue-host-dispatch` regression includes the production wrappers and simulates
a forbidden non-null core pointer alongside a valid KHR pointer, missing aliases,
submission failure, and empty submissions.

### P2: Queue Host Synchronization

Confirmed: game `vkQueueWaitIdle` and `vkQueueBindSparse` previously bypassed
the queue lease used by device retirement. Both now hold that lease, as does
application `vkDeviceWaitIdle`. Instance-proc lookup also routes submit/present
through the same wrappers. Queue debug-label, performance-configuration/hint,
and out-of-band notification commands declared by the bundled Vulkan headers
are serialized when downstream exposes them. Foreign processes and auxiliary
runtime devices still bypass interception.

Sparse binds conservatively invalidate SFS completion evidence. The production
wrapper regression checks lock exclusion from another thread, result propagation,
proc mapping, and sparse-bind invalidation. This closes application entry-point
gaps; it is not live Vulkan synchronization validation.

### P3: Retirement Timing Labels

Confirmed: the old device-idle timing labels included fence qualification and
queue-lock acquisition, not just device-idle calls. They are now
`retirementMeanMs` and `retirementMaxMs`; existing path counters still distinguish
owner-fence retirements from device drains. No timing calculation changed, and
no repository parser consumes the old labels.

Review-fix validation: a clean detached checkout built the Release layer with
MSVC and the SFS shader compiler disabled. All seven selected CTest cases passed:
queue-host dispatch, foreign-process isolation, Native queue completion, and four
SFS tests. All six pacing-analyzer Python tests also passed. This is local
validation, not GitHub CI. Compiler-enabled SFS packaging, live synchronization
validation, and headset profiling remain untested. PR #2 is still open; rebasing
and re-auditing its overlapping XR changes remain a post-merge step.

## Audit-Time Verification And Limits

- Nine existing tests compiled and passed with MinGW g++ in C++20 mode: descriptor arena, deferred memory, resource retirement, storage mirror budget, snapshot batch, eye bindings, source layouts, image plans, and SFS push replay.
- `native_bind_scratch_tests.cpp` compiled but aborted under MinGW. It passed under the project's MSVC toolchain, including its allocation-count checks. The MinGW failure was not localized; this is not counted as a Native runtime failure or a clean cross-toolchain result.
- The stencil reproduction confirmed the mismatch using production recording/restoration helpers and the wrappers' actual mask/key scheme. It did not execute Vulkan draws.
- No full layer build, Vulkan validation session, headset run, or in-game timing capture was performed. The existing passing helper tests do not cover completion locking or implicit command-pool replay retirement.
- The original audit made no runtime source changes. The follow-up above records the implementation; device-idle replacement still needs profiling and GPU lifetime validation.
