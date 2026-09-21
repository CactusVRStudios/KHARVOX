# KHARVOX v1.1 test 3

- Closing the VR intro window now dismisses the intro and continues to DOOM.
- DOOM starts only after the intro process has ended and released its resources.
- A stuck intro teardown is bounded; the launcher terminates only its own helper and verifies termination before continuing.
- Retains the v1.1 rendering optimizations and startup focus recovery.

The first-start low-FPS cause is not yet confirmed. This test removes overlapping intro/game VR sessions as a potential trigger. Headset validation is still required.
