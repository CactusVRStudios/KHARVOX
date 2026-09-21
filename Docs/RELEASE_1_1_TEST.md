# KHARVOX 1.1 Render Performance Test

Local test based on 1.03, including its confirmed movement fix and centered Weapon Wheel.

Adapted from KHARVOX ARGENT, without DLSS:
- Store replayable Vulkan bindings as values with retained storage instead of allocating map nodes and owning callbacks on each bind.
- Release the SFS metadata lock around native graphics/compute pipeline creation; publish results under the lock.
- Forward buffer/memory-only pipeline barriers without acquiring the SFS metadata lock or looking up command-buffer state. Image barriers keep stereo translation.

No draw/effect removal, resolution reduction, shader-profile change or GPU-retirement relaxation. Existing Source Ring and qualified XR owner-fence retirement are retained. Broader TLS recording/descriptor/pipeline caches are not included in this first port because lifetime and invalidation need separate validation for the 2016 hooks.

Compare the same scene, resolution, runtime and logging settings against 1.03. Check both eyes, particles, shadows, menus/cinematics, fast head turns and level changes. Automated tests and synthetic allocation measurements do not establish a headset frametime improvement.
