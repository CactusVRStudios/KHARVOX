# KH-07 Camera Hooks

Scope: camera paths used by SPS (SFS in code), based on `ec2faad`.

## Finding 1: Readers could combine different body-camera publications

Severity: P1, pose consistency and visible stability.

`patchCamera` updated the body origin and basis as individual relaxed atomic
stores, followed by a release store to an already-true validity flag.
`KharvoxCameraGetBodyPose` independently loaded those fields and the mutable
body-anchor offset. A render worker could begin the next publication while XR,
weapon or HUD readers were copying the previous one. Atomics prevented scalar
data races, not a mixed basis, origin or anchor. Clearing validity at a level
transition also did not prevent an already-running producer from setting it
again afterward.

Fix: publish origin, basis, anchor offset and anchor owner as one short,
mutex-protected snapshot. Capture a generation before producing the pose and
reject publication after a level invalidation. Native physics calls and pose
math stay outside the snapshot lock. Live translation still uses the current
physics position, but only with the owner and offset from the same body pose.

Validation: `body-camera-snapshot` checks invalidation and stale publication,
plus four concurrent readers against 100,000 complete publications with 1,000
invalidations. Release tests retain assertions through the repository's
existing `/UNDEBUG` configuration. The Release layer builds successfully.

This fixes the body-pose publication boundary, not every atomic camera input.
The cached physics tuple and native pointer lifetime are reviewed separately.
No headset frame-time improvement is claimed; live game validation remains
necessary for level transitions, crouching and weapon/HUD alignment.

## Finding 2: Optional physics reads could fault outside their guard

Severity: P1, level-transition crash risk.

`readLivePlayerPhysicsOrigin` retained a player address across updates. Its
VirtualQuery checks did not keep the physics object or vtable alive between
validation and dereference. The existing exception handler covered only the
virtual call, leaving vtable lookup and all reads of the returned coordinates
unguarded. The native capture hook duplicated the unguarded physics lookup.

Fix: both paths use one guarded resolve/call/copy operation. Preserve the
existing memory and executable-page checks, copy coordinates into a local
candidate once, validate that candidate, and only then publish the output.
MSVC structured exception handling covers the complete optional probe. Failure
keeps the body-camera fallback; no retry loop, wait or allocation is added.

Validation: `physics-origin` compiles the production helper and tests valid
results, nulls, NaN/infinity in each component, an explicitly faulting callback,
and inaccessible object/vtable/code/result pages. A partial result straddling
a readable and inaccessible page verifies that failure leaves output untouched.
Test memory predicates intentionally accept non-null addresses, reproducing
memory revoked after validation rather than merely exercising a precheck.
All five camera CTests and the Release layer build pass.

The exception handler does not establish ownership of game objects, detect
every logically stale allocation, or serialize engine updates. The native
hook's original player/view call retains its engine-owned lifetime contract.
Non-MSVC builds retain their existing lack of SEH protection. Live level-load
stress is still required; this is not a claim of complete game-memory safety.

## Finding 3: Cached physics publication mixed positions and owners

Severity: P1, pose consistency and level-transition stability.

The native capture hook published physics XYZ, player owner, capture Present
and validity separately. Body-anchor calibration, XR room-scale movement and
weapon positioning could observe a mixture of consecutive captures. A capture
already running at invalidation could also republish its old owner as valid.

Fix: extend the camera pose state with a complete physics snapshot, sharing the
body snapshot's generation and short mutex. Read that tuple once per consumer.
Reject captures that began before invalidation and reject a live physics query
from a different generation than its body anchor. Invalidate calibration under
the existing stance mutex, which prevents a stale calibration from surviving
the reset. Neither mutex is held across native physics callbacks.

Validation: extend `body-camera-snapshot` with four concurrent physics readers,
100,000 coherent captures and 1,000 invalidations. Check owner/Present/XYZ
consistency, rejection of stale body and physics publications, and atomic
reset of both snapshots to the same generation. Release layer and all five
camera CTests pass. Runtime locks cover copies only, except for the existing
stance-calibration lock; no GPU waits are changed. Headset timing and native
engine lifetime guarantees remain outside these regression tests.
