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
