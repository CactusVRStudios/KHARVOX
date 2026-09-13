# KHARVOX / DOOM VR Mod Self-Help FAQ

This FAQ collects common fixes for startup, performance, image quality, and configuration problems.

## Poor Performance or Major FPS Drops

### Possible cause: OpenXR Toolkit is still active

If you previously enabled OpenXR Toolkit for another game, such as Gunman Contracts, it can severely hurt performance with this mod.

### Fix

- Disable OpenXR Toolkit.
- Restart the game, launcher, SteamVR/VDXR, and preferably your PC.

## Game Starts, But VR Shows a Black Screen

### Possible cause: outdated NVIDIA driver

If the launcher starts, VDXR initializes, then you only get a black screen and the launcher gets stuck on:

```text
Doom started. Verifying VR image...
```

this may be caused by an outdated NVIDIA driver.

### Fix

- Update your NVIDIA graphics driver.
- Restart your PC.
- Launch the mod again.

## Luke Ross Mod Is Still Installed

If the Luke Ross mod is still installed, this mod will not work correctly.

### Fix

- Fully remove the Luke Ross mod.
- Verify the game files through Steam if needed.
- Reinstall or relaunch KHARVOX.

## Laptops or PCs with Multiple Graphics Cards

Systems with multiple GPUs, such as gaming laptops with both integrated graphics and an NVIDIA/AMD GPU, may cause problems.

### Possible symptoms

- Game does not start correctly.
- VR stays black.
- Very poor performance.
- Wrong GPU is used.

### Fix

- Force the game and launcher to use the dedicated GPU in Windows Graphics Settings.
- Also select the high-performance GPU in the NVIDIA/AMD control panel.
- If possible, make sure your headset/display is connected to the dedicated GPU.

## Do Not Install on an HDD

Installing the game or mod on an HDD can cause long loading times, stutters, or streaming issues.

### Recommended

- Install the game and mod on an SSD.
- NVMe SSD is preferred.

## Background Overlays Can Cause Issues

Overlays can cause performance issues, crashes, or VR hook conflicts.

### Examples

- MSI Afterburner
- RivaTuner Statistics Server
- ReShade
- Discord Overlay
- GeForce Experience Overlay
- Steam Overlay
- OpenKneeboard

### Fix

- Disable all overlays for testing.
- Do not allow OpenKneeboard to access SteamVR.
- Restart your VR runtime and the game.

## Quest Pro / Virtual Desktop: Blurry Image

### Possible cause: Foveated Streaming in Virtual Desktop

On Quest Pro, enabled Foveated Streaming can cause a blurry image.

### Fix

- Open Virtual Desktop.
- Disable Foveated Streaming.
- Restart Virtual Desktop Streamer on your PC.
- Launch the game again.

## Launcher Says It Is Still Running as Administrator

If the launcher says it is still running as administrator, Windows UAC may be disabled.

### Fix

- Enable Windows UAC.
- Restart your PC.
- Start the launcher normally, not as administrator.

## ReShade / OpenXR API Layer Conflict

An old or active ReShade OpenXR API layer can prevent VR from starting correctly.

### Fix

1. Press `Windows + R`.
2. Type `regedit`.
3. Go to:

```text
HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit
```

4. Look for an entry similar to:

```text
C:\ProgramData\ReShade\ReShade32_XR.json
```

5. Either change its value to `1` or delete the entry.

```text
1 = disabled
0 = enabled
```

6. Restart Virtual Desktop Streamer on your PC.

This can help if the game does not start directly in VR or if there is a ReShade/OpenXR conflict.

## Pancake Mode After Playing in VR

You can close the game after playing in VR and later open it again in normal pancake mode.

### Known behavior

- Pancake mode works.
- You may need to adjust your resolution settings again after returning to the normal game.

## Can Controls Be Edited?

Yes, but only in a limited way.

### Notes

- Controls can be edited, but the options are limited.
- The default control scheme works well for many players.
- Small personal adjustments are possible.

## HUD Is Offset or Partly Not Visible

### Symptoms

- Health bar is hard to see.
- Armor bar is hard to see.
- Weapon stats are partly hidden.
- HUD appears too low or slightly too far to the right.

### Status

- This is a known issue.
- Try adjusting resolution, VR scaling, or display settings if available.
- More HUD improvements may come later.

## Quick Troubleshooting Checklist

1. Disable OpenXR Toolkit.
2. Remove the Luke Ross mod.
3. Update your NVIDIA/AMD graphics driver.
4. Disable overlays.
5. Check for ReShade OpenXR API layer conflicts.
6. Disable Virtual Desktop Foveated Streaming.
7. Install the game and mod on SSD/NVMe.
8. Force dedicated GPU on laptops/multi-GPU systems.
9. Restart Virtual Desktop Streamer / SteamVR / PC.
10. Launch the mod again.
