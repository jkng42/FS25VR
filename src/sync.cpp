// Eye synchronisation: both images of a stereo pair must show the same moment of the world.
//
// The engine runs each frame through one function, RunFrame(this, dt), and every simulated
// system (game logic, physics stepping, animation) takes its time step from that dt. On frames
// that render the second (right) eye we pass a near-zero dt, so the world holds still while the
// camera moves to the other eye; the held-back time is added to the next first-eye frame, so the
// game still runs at real-time speed. (Not exactly zero: game scripts divide by dt.)
#include "sync.h"
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "profile.h"
#include "xr.h"

#include <windows.h>
#include <vector>

namespace {

using PFN_RunFrame = uintptr_t (*)(void* self, float dt);
PFN_RunFrame o_RunFrame = nullptr;

float  g_carry = 0.0f;
int    g_logged = 0;

uintptr_t Hook_RunFrame(void* self, float dt)
{
    if (g_logged < 5) {
        Log("eye sync: RunFrame dt=%f", dt);
        g_logged++;
    }
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    uintptr_t r;
    if (vr::NextFrameIsSecondEye()) {
        g_carry += dt;
        if (g_carry > 250.0f) g_carry = 250.0f;  // never release more than a quarter second at once
        r = o_RunFrame(self, dt * 0.001f);
    } else {
        float total = dt + g_carry;
        g_carry = 0.0f;
        r = o_RunFrame(self, total);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    prof::RunFrameTime((double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart);
    return r;
}

BYTE* FindPattern(const char* pattern)
{
    auto base = (BYTE*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    BYTE* start = nullptr;
    size_t size = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (memcmp(sec->Name, ".text", 5) == 0) start = base + sec->VirtualAddress, size = sec->Misc.VirtualSize;
    std::vector<int> bytes;
    for (const char* c = pattern; *c;) {
        if (*c == ' ') { c++; continue; }
        if (*c == '?') { bytes.push_back(-1); c += (c[1] == '?') ? 2 : 1; continue; }
        bytes.push_back((int)strtoul(c, nullptr, 16));
        c += 2;
    }
    BYTE* found = nullptr;
    for (BYTE* p = start; p && p + bytes.size() <= start + size; p++) {
        size_t i = 0;
        while (i < bytes.size() && (bytes[i] < 0 || p[i] == bytes[i])) i++;
        if (i == bytes.size()) {
            if (found) return nullptr;  // ambiguous
            found = p;
        }
    }
    return found;
}

} // namespace

bool InstallEyeSync()
{
    if (!g_config.syncEyes) {
        Log("eye sync: disabled in fs25vr.ini");
        return false;
    }
    // RunFrame prologue (15 relocatable bytes) followed by its distinctive body start.
    BYTE* fn = FindPattern(
        "48 8B C4 48 89 58 18 48 89 70 20 55 57 41 56 48 8D A8 88 FE FF FF 48 81 EC 60 02 00 00 "
        "0F 29 70 D8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 40 01 00 00 0F 28 F1 48 8B D9 33 F6 "
        "40 38 B1 95 02 00 00");
    if (!fn) {
        Log("eye sync: RunFrame not found (game version changed?); eyes will not be synchronised");
        return false;
    }
    constexpr size_t kPrologue = 15;
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(tramp, fn, kPrologue);
    BYTE jmp[14] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0] ; dq target
    BYTE* back = fn + kPrologue;
    memcpy(jmp + 6, &back, 8);
    memcpy(tramp + kPrologue, jmp, sizeof(jmp));
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    o_RunFrame = (PFN_RunFrame)tramp;
    bool ok = WriteJump(fn, (void*)Hook_RunFrame);
    Log("eye sync: RunFrame at exe+%llx hooked %s", (unsigned long long)(fn - (BYTE*)GetModuleHandleW(nullptr)),
        ok ? "ok" : "FAILED");
    return ok;
}
