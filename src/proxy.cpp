// Forwards the dinput8.dll exports to the real system DLL.
#include <windows.h>
#include <unknwn.h>

static HMODULE g_real = nullptr;

static FARPROC Real(const char* name)
{
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dinput8.dll");
        g_real = LoadLibraryW(path);
        if (!g_real) return nullptr;
    }
    return GetProcAddress(g_real, name);
}

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto fn = (Fn)Real("DirectInput8Create");
    return fn ? fn(hinst, ver, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    using Fn = HRESULT(WINAPI*)();
    auto fn = (Fn)Real("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
{
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto fn = (Fn)Real("DllGetClassObject");
    return fn ? fn(clsid, riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_DllRegisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    auto fn = (Fn)Real("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    auto fn = (Fn)Real("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

void* WINAPI Proxy_GetdfDIJoystick()
{
    using Fn = void*(WINAPI*)();
    auto fn = (Fn)Real("GetdfDIJoystick");
    return fn ? fn() : nullptr;
}

}
