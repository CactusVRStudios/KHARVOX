# OldschoolEngine2 SID subset

Source: https://github.com/cadaver/oldschoolengine2
Revision: 4b295b1c7a1cbaf37616a68f71384d2a8164b650
Files: Assets/Scripts/SID.cs, MOS6502.cs, RAM64K.cs

MIT notices are retained in each source file and embedded in the launcher's
existing license viewer via assets/cracktro/NOTICES.txt.

Local adaptations remove Unity dependencies, replace diagnostic logging with
System.Diagnostics.Debug, and use a fixed 985248 Hz PAL clock with 44100 Hz PCM
instead of Unity's adaptive audio buffer timing. Nullable analysis is disabled
for these imported files. The managed PSID host and Windows audio output live
in launcher/KharvoxLauncher/SidAudioPlayer.cs. This host intentionally supports
the embedded Coco Intro PSID, rather than claiming general RSID compatibility.
