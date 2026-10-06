#pragma once

#include "ProtubeShotPolicy.h"

// ProTubeVR ProVolver / ForceTube recoil output.
//
// Enabled when ForceTubeVR_API_x64.dll sits next to KharvoxLayer.dll and
// protube.ini (optional, same folder) does not set enabled=0. All SDK calls
// run on a private worker thread so Bluetooth latency can never stall a frame.

// Idempotent. Safe to call from the game's init path (not from DllMain).
void KharvoxProtubeStart();
// Called once per OpenXR frame with the current gameplay state.
void KharvoxProtubeUpdate(const kharvox::protube::Input& input);
// Non-blocking; safe to call from DLL_PROCESS_DETACH.
void KharvoxProtubeRequestStop();
