# KH-08: Weapon Hooks

Audited from integrated PR #1/#2 commit `ec2faad`, 2026-09-19, for SPS gameplay.

## P1: Tracking Changes Rewrote Executing Branch Instructions

`setMuzzleFireAxisOverride` changed a six-byte JNE into NOP/JMP and back with
`memcpy`, driven by controller validity from the XR thread. The game firing
thread does not take that thread's controller mutex. VirtualProtect and
FlushInstructionCache do not suspend instruction execution during the write.
The branch at RVA `0xD5EFF3` is also unaligned. A tracking transition could expose
a partially modified instruction stream, not merely an old tracking state.

The existing MinHook dependency now installs one signature-checked detour with
its thread suspension and instruction relocation. Runtime toggles only exchange
an aligned LONG. No protection changes, instruction copies, or cache flushes
occur during tracking changes. Disabling retains the original JNE condition;
enabling selects its original taken destination. The executable stub preserves
RAX, RFLAGS and RSP and does not call C++ from the middle of the game function.

The 50-byte stub saves flags/RAX, reads the data flag, restores RAX, and either
restores flags before an unconditional taken jump or restores flags and applies
the original JNE. Both exits use register-preserving RIP-relative absolute jumps.
It is written on an RW page, changed to RX and flushed before installation.
The hook and its data owner live for the process lifetime, like existing game
hooks. Failed setup leaves the native branch unchanged and does not enable the
override. The Native engine installer already accepts initialized MinHook state.

## Verification

MSVC Release layer build passed with the SFS compiler disabled. `muzzle-branch`
executes a synthetic x64 JNE through the production hook and bundled MinHook.
It checks signature rejection, all native/forced branch outcomes, live RAX and
ZF preservation, idempotent toggles, restored original bytes on test teardown,
and unchanged installed code while four threads execute 800,000 calls against
100,000 concurrent tracking toggles. The test passed.

No live DOOM tracking-loss/firing stress test or headset frametime measurement
was performed. Removal of runtime code-patching calls is established by code
inspection and the immutable-byte assertion, not a claimed measured FPS gain.
