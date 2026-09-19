# Test 36 - AMD HUD projection investigation

Input: user-supplied logs5.zip. The recorded build is Test22, GPU Radeon RX
9070 XT (vendor 4098), SteamVR/OpenXR. It predates the arm-HUD changes.

KHARVOX.log contains 1129 shader compilation records, 214 projected vertex
variants, zero matched profile replacements and zero screenUi=1 records.
Native GUI discovery nevertheless reports ordinary HUD surfaces and a native
HUD depth of 6. These logs support missing HUD shader classification; they do
not contain eye images or original AMD SPIR-V, so cannot prove the exact
on-screen symptom or certify a hardware fix.

Previously only five module hashes enabled the HUD-specific projection rule.
Add reflected recognition for DOOM UI vertex layouts: vec4 position at location
0; only supported UV/color/tangent inputs; set0/binding0 consists of four named
vec4 MVP rows and optionally fontfxdegamma. Packed-world and atlas exclusions,
shadow/mono role handling and existing hash compatibility remain intact.

The rule selects the same projection correction without depending on vendor
module bytes. Test35 perspective-vs-flat handling remains active so arm panels
retain metric stereo close to the viewer. No global eye-offset reduction.

Regression fixture uses a previously unknown shader with renamed variable
identifiers and no UI hint. It must receive the UI correction; mono stays mono.
Existing packed-world/atlas tests still reject UI treatment. Hardware validation
requires retesting the affected HUD on AMD; screenUi=1 now provides evidence of
classification in the new log, not proof of visual correctness.

Validation: 123/123 CTests, launcher self-test and all 1395 local shader variants
compiled successfully. This corpus is local reference data, not the missing AMD capture.
