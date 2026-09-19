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

## P1: Descriptor Restoration Changed Binding Order

SFS cached the last graphics descriptor per set slot, then restored slots in
numeric order at every render-pass/subpass entry. Bind an older set 1 with
layout A, then set 0 with an incompatible layout B: set 0 is valid. Replaying
0 then 1 reverses the relevant commands and disturbs set 0, so a subsequent
draw can consume undefined descriptors despite the original command sequence
being valid. Dynamic offsets do not repair a disturbed binding.

Fix: retain the order of the latest bind to each slot and restore in that order
at multiview subpass entry. Storage and work remain bounded by bound set slots,
not command history. Descriptor values and per-set dynamic offsets use the
existing cache. Command-buffer begin clears the ordering alongside that cache.

Do not replay non-pipeline state for mono render passes: Vulkan preserves it.
Reissuing binds there can itself disturb previously valid bindings. Still bind
the correct mono/stereo graphics pipeline on every pass transition. This also
removes redundant descriptor, vertex, dynamic-state and push-constant commands
from mono passes; no measured GPU or headset speedup is claimed.

Verification: `sfs-descriptor-order` compiles the production SFS implementation
with shader stubs and checks actual begin/next-subpass restoration order,
dynamic offsets, mono pipeline rebinding without descriptor replay, and command
buffer reuse. Its reference model reproduces numeric-order disturbance and
compares defined descriptor values across 1,000,000 randomized binding and
multiview-reset transitions, including multi-set writes and incompatible push
constant ranges. Repeated overwrites retain only the affected slots.
The descriptor, push-replay, native push-replay and owner-completion tests pass.

Rules checked against Khronos Vulkan specification sections
[Pipeline Layout Compatibility](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html#descriptors-compatibility)
and [Render Pass](https://docs.vulkan.org/spec/latest/chapters/renderpass.html):
multiview resets non-render-pass state at each subpass; mono does not.
These tests use dispatch mocks, not a validation-layer GPU workload or a DOOM
capture. They preserve required defined descriptors, not the exact contents of
unused descriptor slots that were already undefined in the application.
