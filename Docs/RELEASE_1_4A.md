# KHARVOX 1.4a

- Improved SFS smooth turning when using the OFXR bridge (PR #15).
- Includes the Steam and GOG compatibility from version 1.4.

PR #15 was integrated unchanged. Its author reports testing with Pimax Crystal Super OLED / SteamVR and Quest 3 / VDXR. No independent OFXR headset test has been performed for this release.

Known limitation: the render prediction is capped at 100 ms while input integration is capped at 50 ms. Long update stalls may therefore cause a turn correction; this was demonstrated in an isolated calculation, not confirmed in headset testing.

Windows file version: 1.4.1.1410. Launcher display version: 1.4a.
