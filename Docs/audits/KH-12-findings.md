# KH-12: FSR Recording Recovery

Audited from integrated PR #1/#2 commit `ec2faad`, 2026-09-19.
Scope: the FSR1 allocation, descriptor, EASU/RCAS and cached-output path used
by SPS, including its XR recording-failure caller. No AER-only changes.

## P1: Abandoned Commands Left FSR Layouts And Output Marked Valid

`Fsr1Upscaler::record` advances input/EASU/output initialized flags and the
processed revision while recording. `vkEndCommandBuffer` can subsequently fail.
The XR abort handler reset the eye/cache metadata but not the FSR metadata.
The next FSR call could therefore use old layouts never executed on the GPU;
a repeated revision could return an output whose producing commands were lost.

Added `discardRecordedFrame`, called by the shared XR command-recording abort
handler. It invalidates FSR content and recorded layout assumptions without
freeing GPU resources or adding waits. The next recording starts from UNDEFINED
and executes both passes for each eye. Normal successful cached reuse is unchanged.
This API is for abandoned recording, not retirement of pending GPU work.

## Verification

MSVC Release layer build passed with the SFS compiler disabled.
Extended the existing production FSR GPU test to discard a recording before each
of three rendered pairs, then repeat the same revisions. It checks four dispatches,
six fresh layout transitions, no duplicate work for a valid cache hit, distinct
eye images and pixel readback for RGBA/BGRA UNORM/SRGB formats.

On NVIDIA GeForce RTX 4090, `fsr1-gpu-current-eyes` passed all four formats.
Temporarily omitting invalidation made the test fail with
`Discarded output reused or valid output recomputed`; restoring it passed.
Raw diff and `git diff --check` passed. This exercises real FSR GPU work, not
in-game error injection or headset frametimes. No performance gain is claimed.

## P1 Follow-Up: Failed Copy Submission Kept Recorded FSR State

The recording-abort fix did not cover a successful `vkEndCommandBuffer` followed
by a failed `vkQueueSubmit`. A host/device OOM submission can leave GPU resources
untouched while FSR retains the revision and layouts advanced during recording.
A retry could then reuse output that the GPU never produced.

The copy-submit failure handler now calls `discardRecordedFrame` before cleanup
or the existing native fail-fast path. The handler lives in
`XrCopySubmitFailure.inc` so the GPU regression executes the production branch
with stubbed XR cleanup. Wait failures after successful submission keep their
existing recovery policy; this change adds no GPU waits or resource destruction.

The GPU test injects host and device OOM through the copy-submit dispatch after
successful command-buffer recording. Each retry uses the same source revision
and checks four EASU/RCAS dispatches, six UNDEFINED transitions, valid cache reuse
and both eye readbacks across RGBA/BGRA UNORM/SRGB. The injected call never reaches
the driver. Removing the production invalidation reproduced
`Discarded output reused or valid output recomputed` on the RTX 4090.

With invalidation restored, the SPS compiler-enabled MSVC x64 Release build and
all 131 CTests passed. This run used the PR #4 branch, without PRs #3 and #5-#7.
Build diagnostics were limited to macro redefinitions and test assertion
flag overrides. No live game OOM injection or headset validation was performed.
