# Test 38: keep ProgMeter extraction inside the owning HUD frame

Test37 feedback: ProgMeter still follows Ammo. The gameplay log contains hook installation and saved ProgMeter edits but no independent GUI creation or subtree-detached event.

The native SWF dispatch at 0x161CC90 can enqueue work through 0x161B660. Successful enqueue returns before drawing; the actual sprite traversal then occurs outside Test37's thread-local WeaponInfo frame scope. That invalidates both its SWF ownership and source pose checks. Test37 therefore silently retains the original Ammo grouping.

The new enqueue wrapper declines only the exact owned SWF/GUI pair when its hand-bound source context is active. Native code at 0x161CE86 already supports declined jobs and falls through to the synchronous draw at 0x161F0F0. Updates/animations are not repeated. Other SWFs continue using native job dispatch. This keeps the geometry extraction and world-object update in the same manager frame.

New signature check covers the complete enqueue prologue. Tests cover matching ownership, unrelated SWFs/entities, disabled calibration, absent context and other surface types. Existing handedness/config migration tests remain intact.

Look for these events in the next loaded-game log:
- scoped queue hooks installed
- owned SWF drawn inline
- independent native GUI created
- five-circle subtree detached

Startup and policy tests do not prove headset appearance. Loaded-game validation of the independent panel and its lifecycle is still required. Calibration values are preserved; Num5 on the selected ProgMeter target resets its experimental Test37 adjustments if necessary.
