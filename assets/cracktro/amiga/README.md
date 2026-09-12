# Second launcher intro

Reference: https://www.wab.com/?screen=316 (Chris/CODEF, Skid Row Ghost Battle Final).
`cop.png`, `font32.png`, `eq.png`, and `possessed.sid2` are reference assets.
`logo.png` is a source-style reference only and is not embedded. No BRAINWALKER asset is used.

`kharvox-amiga.png` was made with the built-in Imagegen tool, using the existing
KHARVOX logo as the identity reference and the Skid Row logo as the style reference.
Prompt: retain the KHARVOX wordmark and orbit emblem; remove subtitle; use chunky
beveled icy-blue/silver chrome, deep navy extrusion, sharp pixel highlights and
Amiga dithering on #000021; no other words or credits. Banner for 640x112 strip animation.

The renderer rebuilds the original iterative copper scroller and 16-strip logo
bounce at 60 Hz. The existing intro text slots are concatenated in display order.
The 800x600 window scales the 640x480 scene. ESC closes either intro. Each normal
footer click selects the next intro, beginning with C64; Shift-click remains Dev Mode.

## Music

`node tools/amiga-music-build/compile.cjs` converts the original SIDMON II module
to a complete 76.8-second loop of Paula register events and sample-memory edits.
The 25 KB gzip sequence is embedded, with four sample voices synthesized in C#
at 44100 Hz. It contains no recorded PCM audio and requires no external player.

`powershell.exe -NoProfile -File tools/test_amiga_cracktro.ps1` checks rendering,
audio, alternating windows, replacement cleanup and ESC.
`node tools/amiga-music-build/verify.cjs tmp/amiga-test/sid-test.wav` compares
the generated C# samples against the original Flod mixer (10 seconds, 441000 samples).
Flod build-time source notices are retained in their original files.
