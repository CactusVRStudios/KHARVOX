# KH-02: Swapchain, Present And Startup Lifetime

Audited from integrated PR #1/#2 commit `ec2faad`, 2026-09-19.
Scope: SPS (called SFS in code), Vulkan acquire/present dispatch, startup
swapchain replacement, shared XR image-cache retirement and queue ownership.

## P1: Internal Retirement Waits Bypassed Queue Serialization

`KharvoxXRSwapchainDestroyed` called the raw downstream `queueWaitIdle` during
FSR startup retirement without the queue-access callbacks. `ensureStereoCache`
did the same before replacing shared copy images and FSR resources, including
the initial loading/cinematic cache used by SPS. The XR state mutex only
serializes XR state; engine submit wrappers use a different queue mutex.
Concurrent engine submission could therefore violate Vulkan's queue host
synchronization requirements during these waits.

Fixed by routing standalone XR queue-idle calls through `waitForQueueIdle`,
using the existing queue-access scope and rejecting missing dispatch/callbacks.
Session/device teardown uses the same helper. The copy-submit fallback remains
inside its existing uninterrupted submit-and-wait queue lease.

No GPU wait was removed. Cache replacement still requires successful completion
before freeing resources. This is a stability fix, not a measured frametime win.

## Verification

MSVC Release layer build passed with `KHARVOX_BUILD_SFS_COMPILER=OFF`.
All four selected CTests passed: `xr-queue-idle`, `game-image-lifetime`,
`swapchain-image-state`, and `sfs-owner-completion`. `git diff --check` passed.
Optional PSVR2 headers and bHaptics DLL were absent; packaging was not tested.

The `xr-queue-idle` test includes the production queue helper. A second thread
checks that downstream idle cannot overlap queue access, success/device-loss
results are preserved, the lease is released after failure, and missing queue,
dispatch or callbacks cannot produce a false completion or unlocked call.

Live DOOM startup churn, Vulkan validation and headset timing remain necessary
to establish hardware behavior. No in-game crash or speedup is claimed.
