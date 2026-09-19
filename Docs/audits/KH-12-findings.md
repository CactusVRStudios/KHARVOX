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
