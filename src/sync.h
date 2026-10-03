#pragma once

// Holds simulation time still on second-eye frames so both eyes of a pair show the same moment.
// Same two-phase scheme as InstallGamePatches (see game.h). Safe to call repeatedly.
bool InstallEyeSync(bool final);
