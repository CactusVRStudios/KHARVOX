# KH-04: SFS Renderer State

Audited from integrated PR #1/#2 commit `ec2faad`, 2026-09-19.
Scope: SPS/SFS command interception, render-pass binding restoration, push
constants, shader/pipeline ownership, descriptors and frame retirement.

## P1: Push Replay Split Required Combined Stage Masks

`PushReplay` stored words separately per shader stage. Render-pass entry and
subpass changes replayed them using a single stage bit per call. For a pipeline
layout with a combined vertex/fragment push range, a valid original write to
both stages became two invalid commands. Vulkan requires each replayed byte's
stage mask to include all overlapping declared range stages
(`VUID-vkCmdPushConstants-offset-01796`). Value-only comparison tests missed it.

Reused the existing `NativePushReplay` helper, which retains original masks,
layouts and ranges, and removes fully superseded same-layout writes. The SFS
header is now an alias rather than a second implementation. Added `clear()`
to that helper so command-buffer reuse retains the outer vector capacity.

This preserves the original calls for partial/cross-layout overlaps rather than
inventing layout compatibility. Such histories can retain more commands than
the old per-stage word table; invalid commands are not an acceptable optimization.
Repeated identical range updates remain one retained write with reused storage.

## Verification

The combined-stage regression failed against the previous implementation with
`Replay split a combined push-constant stage mask` and passed after the fix.
Coverage includes 1,000 randomized partial/stage/layout/reset comparisons,
10,000 repeated combined-stage overwrites, and a single full-range replay call.
Moved this header-only test out of the optional shader-compiler CMake block.
The shared Native replay and production SFS owner-completion tests also passed.

MSVC Release layer build and `git diff --check` passed with the SFS compiler
disabled. The owner-completion test separately compiled the production SFS
runtime with shader-compiler stubs; no transformed GPU shader was exercised.

The existing microbenchmark compares against an older map/function design,
not this PR's immediate parent; its timings are not evidence of a game speedup.
Compiler-enabled packaging, live Vulkan validation and headset frametimes
remain untested.
