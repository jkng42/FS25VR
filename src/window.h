#pragma once
#include <windows.h>

// Lets the game render at a resolution taller than the monitor (e.g. 2448x2448 per eye):
// the real window is shrunk to fit the screen and DXGI stretches the image into it, while
// window-size queries and mouse coordinates are translated so the engine still sees a window
// of the full render size.
bool InstallWindowHooks();

// Called when the game creates/resizes its swap chain with the size it wants to render at.
void FitWindow(HWND hwnd, UINT renderW, UINT renderH);

// Size the game asked for (0,0 if not active); used to keep ResizeBuffers at render size.
void VirtualSize(HWND hwnd, UINT& w, UINT& h);

// Mouse position in backbuffer pixels when the OS cursor is visible over the game window.
bool CursorInBackbuffer(UINT bbW, UINT bbH, float& x, float& y);

// Keeps the cursor inside the game window while it is focused (clipMouse=1).
void UpdateMouseClip();
