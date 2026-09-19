# Test 31 - Arm HUD ownership and near-camera guard

Video inspected: VirtualDesktop.Android-20260919-093855-0.mp4 (31.89 s).
Life/Ammo and small challenge/objective symbols are visible during arm motion.
A single-eye recording cannot establish binocular disparity by itself.

The old hand-panel selector used only caller BDCF54, 512x300 and scale 83/100.
Native RTTI identifies manager vtables 2240978 as Hud_BottomLeft, 2240888 as
Hud_WeaponInfo, and 22425D0 as Hud_Objective. Constructor code at BDC305 and
BDC2A3 installs the first two; each then assigns the manager scale at +90.
Require both the exact manager and previous surface signature for hand placement
and sequence suppression. Other HUD managers continue through normal HUD layout.

The UI vertex correction enables metric eye translation only above clip-W 8.
An arm canvas can cross this threshold per vertex, unlike distant headlocked UI.
Preserve a minimum forward depth of max(10 game units, 0.15 m) for all four
corners using the current central render camera and final panel axis/extents.
Translate only along camera forward; do not alter calibration or patch shaders
shared by other UI. This deliberately limits how close the panel can approach.

Regression coverage: exact owners, objective/bottom/unknown rejection with the
same dimensions/scales, tilted corners, unchanged distant panel and invalid axes.
Headset confirmation of both reported symptoms remains pending.

## Test 32 follow-up
SFS no longer sets or protects r_antialiasing by default. DOOM owns the choice.
Explicit Debug AA-off remains effective; AER keeps its spatial SMAA default.
SSDO temporal history is separate and remains disabled. Both HUD fixes above
are included unchanged. Native CVar forwarding and launcher argument tests pass.

