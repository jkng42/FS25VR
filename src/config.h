#pragma once

struct Config {
    bool  enabled         = true;   // master switch; when false the DLL is a pure dinput8 passthrough
    bool  forceNoVsync    = true;   // the HMD paces the game, the desktop mirror must not
    int   presentLag      = 0;      // frames between a Lua camera update and the Present that shows it
    float worldScale      = 1.0f;   // >1 makes the world feel smaller
    float menuDistance    = 2.0f;   // metres to the flat menu screen
    float menuWidth       = 2.6f;   // metres
    bool  fitWindow       = true;   // shrink a window taller than the screen, keep rendering at full size
    bool  syncEyes        = true;   // freeze the simulation on second-eye frames
    int   syncPhase       = 0;      // 1 = freeze the other parity (if the engine pipelines physics)
    bool  symmetricFrustum = true;  // centred per-eye projection (screen-space effects assume it)
    bool  clipMouse       = true;   // keep the cursor inside the game window while it is focused
    bool  showCursor      = true;   // draw the mouse pointer into the headset image
    bool  asyncSubmit     = true;   // headset frame loop on its own thread; the game never waits for it
    bool  profile         = false;  // GPU timestamps + per-frame CSV
    bool  forceRender     = false;  // debug: do the full VR copy even when the headset is idle
    bool  deferPatches    = false;  // debug: skip the load-time engine scan (simulates a DRM-wrapped exe)
    bool  debugLog        = false;  // verbose per-frame logging
};

extern Config g_config;
void LoadConfig();
