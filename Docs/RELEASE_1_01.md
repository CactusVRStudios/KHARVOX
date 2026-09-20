# KHARVOX 1.01 — Weapon Wheel

- Centers the owned weapon-selection canvases on the current camera pose, three meters ahead.
- Keeps the panel camera-aligned with a fixed three-meter width independent of RenderScale and general HUD calibration.
- Keeps the world stereoscopic and preserves offhand Life, Ammo and ProgMeter placement.
- Retains native weapon selection and input handling.

Local test package. Headset confirmation of wheel centering, framing, selection indicators and high-resolution behavior is still required. The owner identities were verified against the supported Steam executable RTTI; automated tests do not establish visual correctness in a headset.

## Test 2: integrated audits

Includes PRs #2â€“#8: SFS owner-fence retirement, OpenXR lifecycle/queue locking, swapchain retirement, FSR recovery, SFS push/descriptor restoration, weapon tracking branch safety and coherent camera/physics snapshots. Weapon Wheel changes are retained. CPU timing gains and live headset stability are not yet measured.
