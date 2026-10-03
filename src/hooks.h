#pragma once

// Patches the game's dxgi imports so we see every factory, swap chain and Present.
bool InstallDxgiHooks();

// Writes an absolute jump at 'target' to 'detour' (target must have >= 12 patchable bytes).
bool WriteJump(void* target, void* detour);
