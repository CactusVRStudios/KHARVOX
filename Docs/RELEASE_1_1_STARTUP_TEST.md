# KHARVOX 1.1 Test 2 — Startup handoff

The supplied slow run sustains about 66 ms/frame; the subsequent run is about 11–15 ms/frame. Most extra time is outside Present. SFS sampled CPU work is similar, owner-fence retirement succeeds in gameplay, and render scale/FSR output match. The logs do not establish the exact engine/driver wait or desktop foreground state during slow gameplay.

Fix a startup activation gap: finish VR intro helper cleanup, then activate DOOM once after startup if the foreground belongs to KHARVOX, DOOM or the desktop. Preserve unrelated foreground applications; never activate windows on render threads or reclaim focus continuously. Make helper disposal idempotent. Add foreground PID to existing extended stall diagnostics.

This addresses a plausible background-throttling trigger, not a proven universal cause. First-launch gameplay confirmation is required. All 1.1 rendering optimizations, 1.03 movement fix and Weapon Wheel remain included.
