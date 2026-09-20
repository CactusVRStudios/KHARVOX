# KHARVOX 1.03 Movement Diagnostic

Local diagnostic build, not a public release. Movement behavior is unchanged.

A movement-diagnostic-PID.csv file is written automatically into the KHARVOX log directory at up to 20 samples per second. It records OpenXR input, transformed/published input, the latest values returned by XInput, poll counts, physics position and horizontal speed, direction and gameplay state. Physics speed is -1 when no valid consecutive sample exists. Latest delivered input is asynchronous and may precede the published sample. Speed includes collision/script effects and is not the internal DOOM movement command.

Compare forward/back/strafe and diagonals on flat ground, then the problem location. Keep full stick deflection for several seconds and note approximate elapsed time when slowdown occurs. Repeat facing another direction. Send the CSV and regular logs. A connected physical gamepad and the selected Head/Offhand movement mode should be reported.

Logging adds small CPU/file-I/O overhead and is not intended for performance benchmarking. No movement normalization or direct engine-input bypass is enabled.

CSV location: %TEMP%\KHARVOX-Diagnostics\movement-diagnostic-PID.csv. Collect this file separately along with the ordinary KHARVOX logs. Start through the launcher included in this package.
