# Test 37: independent ProgMeter calibration

The requested object is the five-circle `prog_meter` group in `swf/ws_0.swf`, with `upg01` through `upg05`. `ChallengeItems` and mission objective text are not targeted.

The native WeaponInfo frame establishes SWF ownership. Only a matching sprite encountered during its actual draw traversal is redirected. A separate native GUI/model pair owns the geometry, so a rotation does not mutate Ammo's shared render entity. The existing render-bound offhand pose drives the new panel. Geometry allocation capture measures the group's initial visible bounds for its pivot; the cached pivot does not follow individual fill animations. Original materials and SWF animation remain active.

The optional path is gated on native function signatures. Unsupported binaries keep the original grouping. Missing tracking or unsafe near-eye depth suppresses the independent panel. Each owner frame clears its geometry and hides it when not drawn; world-generation changes discard stale native references.

Config v3 contains six rows: the existing four rows followed by ProgMeter normal and left. Readers accept v1/v2 without changing existing rows. The third pose starts above the first calibrated panel and is independently adjustable; its initial pose is a starting point, not a new headset-validated default.

Debug: Life/Ammo/ProgMeter calibration. Num0 cycles Life, Ammo, ProgMeter; Num+ switches rotation/position; Num4/6, Num2/8, Num7/9 adjust axes; multiply/divide adjusts size; Shift uses finer steps; Num5 resets only the selected handedness/element. All six rows are saved automatically.

Validation: Release builds; 123 CTests including v2-to-v3 migration, v3 round trip, truncated-file rejection and ProgMeter origin policy; launcher self-test. Simulator startup verifies the packaged Test37 DLL is loaded and the new owned-frame/geometry hooks install. The actual five-circle draw, headset appearance, independent rotations, both handedness modes, visibility after sequences, and level transitions require a loaded-game test. No AMD/PSVR2 hardware validation is claimed.
