#include <windows.h>
#include "config.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "window.h"
#include "sync.h"

static void Startup(HMODULE self)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir = path;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    LogInit(dir);
    Log("fs25vr " __DATE__ " " __TIME__);
    LoadConfig();
    if (!g_config.enabled) {
        Log("disabled in fs25vr.ini; acting as a plain dinput8 proxy");
        return;
    }
    if (!InstallGamePatches()) Log("game patches failed: the Lua mod will report VR as unavailable");
    if (!InstallDxgiHooks()) Log("dxgi hooks failed: no VR output");
    InstallWindowHooks();
    InstallEyeSync();
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Startup(module);
    }
    return TRUE;
}
