#include "window.h"
#include "config.h"
#include "log.h"
#include <windowsx.h>

namespace {

HWND    g_hwnd = nullptr;        // window being fitted (only set when it is shrunk)
HWND    g_gameHwnd = nullptr;    // the game's swap chain window, always known
bool    g_gameClip = false;      // the game has its own cursor clip active (mouse look)
bool    g_weClipped = false;
UINT    g_vw = 0, g_vh = 0;   // client size the engine believes it has (= render size)
double  g_scale = 1.0;        // real client size / virtual client size
WNDPROC g_origProc = nullptr;
bool    g_applying = false;

using PFN_GetClientRect = BOOL(WINAPI*)(HWND, LPRECT);
using PFN_ScreenToClient = BOOL(WINAPI*)(HWND, LPPOINT);
using PFN_ClientToScreen = BOOL(WINAPI*)(HWND, LPPOINT);
using PFN_SetWindowPos = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using PFN_GetWindowRect = BOOL(WINAPI*)(HWND, LPRECT);
using PFN_GetMonitorInfoW = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using PFN_SetWindowsHookExW = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
using PFN_GetMonitorInfoA = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);

PFN_GetClientRect  o_GetClientRect = nullptr;
PFN_ScreenToClient o_ScreenToClient = nullptr;
PFN_ClientToScreen o_ClientToScreen = nullptr;
PFN_SetWindowPos   o_SetWindowPos = nullptr;
PFN_GetWindowRect  o_GetWindowRect = nullptr;
PFN_GetMonitorInfoW o_GetMonitorInfoW = nullptr;
PFN_GetMonitorInfoA o_GetMonitorInfoA = nullptr;
PFN_SetWindowsHookExW o_SetWindowsHookExW = nullptr;
BOOL(WINAPI* o_SetCursorPos)(int, int) = nullptr;
BOOL(WINAPI* o_ClipCursor)(const RECT*) = nullptr;
HOOKPROC g_engineCallWndProc = nullptr;
HOOKPROC g_engineCallWndRetProc = nullptr;
SIZE               g_frame = {0, 0};  // border + caption around the client area
SIZE               g_realOuter = {0, 0};  // window size we keep the real window at

bool Active(HWND h) { return h && h == g_hwnd && g_scale < 0.999; }

LPARAM ScaleMouseLParam(LPARAM lp)
{
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    x = (int)(x / g_scale + 0.5);
    y = (int)(y / g_scale + 0.5);
    return MAKELPARAM((short)x, (short)y);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (Active(hwnd)) {
        switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) lp = MAKELPARAM(g_vw, g_vh);
            break;
        case WM_GETMINMAXINFO: {
            // the engine claims a minimum size of the full render size, which Windows enforces
            LRESULT r = CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {160, 120};
            mm->ptMaxTrackSize.x = std::max(mm->ptMaxTrackSize.x, g_realOuter.cx);
            mm->ptMaxTrackSize.y = std::max(mm->ptMaxTrackSize.y, g_realOuter.cy);
            return r;
        }
        case WM_WINDOWPOSCHANGING: {
            // let the engine see its request, then hold the real window at the fitted size
            auto* pos = (WINDOWPOS*)lp;
            LRESULT r = CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
            if (!(pos->flags & SWP_NOSIZE) && !IsIconic(hwnd) && !IsZoomed(hwnd)) {
                pos->cx = g_realOuter.cx;
                pos->cy = g_realOuter.cy;
            }
            return r;
        }
        case WM_WINDOWPOSCHANGED: {
            // report the full-size window; DefWindowProc derives WM_SIZE from this copy
            WINDOWPOS wpos = *(WINDOWPOS*)lp;
            if (!(wpos.flags & SWP_NOSIZE)) {
                wpos.cx = (int)g_vw + g_frame.cx;
                wpos.cy = (int)g_vh + g_frame.cy;
            }
            return CallWindowProcW(g_origProc, hwnd, msg, wp, (LPARAM)&wpos);
        }
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEHOVER:
            lp = ScaleMouseLParam(lp);
            break;
        default:
            break;
        }
    }
    return CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
}

BOOL WINAPI Hook_GetClientRect(HWND h, LPRECT r)
{
    BOOL ok = o_GetClientRect(h, r);
    if (ok && Active(h)) *r = {0, 0, (LONG)g_vw, (LONG)g_vh};
    return ok;
}

BOOL WINAPI Hook_GetWindowRect(HWND h, LPRECT r)
{
    BOOL ok = o_GetWindowRect(h, r);
    if (ok && Active(h)) {
        r->right = r->left + (LONG)g_vw + g_frame.cx;
        r->bottom = r->top + (LONG)g_vh + g_frame.cy;
    }
    return ok;
}

// Message hooks see messages before the window procedure, so translate WM_SIZE for them as well.
LRESULT CALLBACK WrapCallWndProc(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0 && lp) {
        CWPSTRUCT c = *(CWPSTRUCT*)lp;
        if (c.message == WM_SIZE && Active(c.hwnd) && c.wParam != SIZE_MINIMIZED) {
            c.lParam = MAKELPARAM(g_vw, g_vh);
            return g_engineCallWndProc(code, wp, (LPARAM)&c);
        }
    }
    return g_engineCallWndProc(code, wp, lp);
}

LRESULT CALLBACK WrapCallWndRetProc(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0 && lp) {
        CWPRETSTRUCT c = *(CWPRETSTRUCT*)lp;
        if (c.message == WM_SIZE && Active(c.hwnd) && c.wParam != SIZE_MINIMIZED) {
            c.lParam = MAKELPARAM(g_vw, g_vh);
            return g_engineCallWndRetProc(code, wp, (LPARAM)&c);
        }
    }
    return g_engineCallWndRetProc(code, wp, lp);
}

HHOOK WINAPI Hook_SetWindowsHookExW(int id, HOOKPROC proc, HINSTANCE mod, DWORD thread)
{
    Log("engine SetWindowsHookExW id=%d thread=%lu", id, thread);
    if (id == WH_CALLWNDPROC && proc) {
        g_engineCallWndProc = proc;
        proc = WrapCallWndProc;
    } else if (id == WH_CALLWNDPROCRET && proc) {
        g_engineCallWndRetProc = proc;
        proc = WrapCallWndRetProc;
    }
    return o_SetWindowsHookExW(id, proc, mod, thread);
}

// The engine clamps its render size to the monitor's work area; report one that fits the full size.
void EnlargeMonitor(LPMONITORINFO mi)
{
    if (!g_hwnd || g_scale >= 0.999 || !mi) return;
    LONG needW = (LONG)g_vw + g_frame.cx, needH = (LONG)g_vh + g_frame.cy;
    for (RECT* r : {&mi->rcWork, &mi->rcMonitor}) {
        if (r->right - r->left < needW) r->right = r->left + needW;
        if (r->bottom - r->top < needH) r->bottom = r->top + needH;
    }
}

BOOL WINAPI Hook_GetMonitorInfoW(HMONITOR m, LPMONITORINFO mi)
{
    BOOL ok = o_GetMonitorInfoW(m, mi);
    if (ok) EnlargeMonitor(mi);
    return ok;
}

BOOL WINAPI Hook_GetMonitorInfoA(HMONITOR m, LPMONITORINFO mi)
{
    BOOL ok = o_GetMonitorInfoA(m, mi);
    if (ok) EnlargeMonitor(mi);
    return ok;
}

BOOL WINAPI Hook_ScreenToClient(HWND h, LPPOINT p)
{
    BOOL ok = o_ScreenToClient(h, p);
    if (ok && Active(h)) {
        p->x = (LONG)(p->x / g_scale + 0.5);
        p->y = (LONG)(p->y / g_scale + 0.5);
    }
    return ok;
}

// Deliberately NOT scaled: SDL measures the window as ClientToScreen(GetClientRect corners), so
// an unscaled mapping keeps it at the full render size. Cursor warps and clip rectangles that SDL
// derives from it are mapped back into the real window in SetCursorPos/ClipCursor below.
BOOL WINAPI Hook_ClientToScreen(HWND h, LPPOINT p)
{
    return o_ClientToScreen(h, p);
}

POINT RealOrigin()
{
    POINT o = {0, 0};
    o_ClientToScreen(g_hwnd, &o);
    return o;
}

POINT VirtualScreenToReal(POINT p)
{
    POINT o = RealOrigin();
    return {o.x + (LONG)((p.x - o.x) * g_scale + 0.5), o.y + (LONG)((p.y - o.y) * g_scale + 0.5)};
}

BOOL WINAPI Hook_SetCursorPos(int x, int y)
{
    if (Active(g_hwnd)) {
        POINT r = VirtualScreenToReal({x, y});
        x = r.x;
        y = r.y;
    }
    return o_SetCursorPos(x, y);
}

BOOL WINAPI Hook_ClipCursor(const RECT* rc)
{
    g_gameClip = rc != nullptr;
    if (rc && Active(g_hwnd)) {
        POINT a = VirtualScreenToReal({rc->left, rc->top});
        POINT b = VirtualScreenToReal({rc->right, rc->bottom});
        RECT r = {a.x, a.y, b.x, b.y};
        return o_ClipCursor(&r);
    }
    return o_ClipCursor(rc);
}

// Border + caption size of the window around its client area.
SIZE FrameSize(HWND h)
{
    RECT wr, cr;
    GetWindowRect(h, &wr);
    o_GetClientRect(h, &cr);
    return {(wr.right - wr.left) - cr.right, (wr.bottom - wr.top) - cr.bottom};
}

BOOL WINAPI Hook_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    if (Active(h) && !g_applying) {
        SIZE f = FrameSize(h);
        if (!(flags & SWP_NOSIZE)) {
            // the engine sizes its window for the full render size; keep it fitted to the screen
            cx = (int)((cx - f.cx) * g_scale + 0.5) + f.cx;
            cy = (int)((cy - f.cy) * g_scale + 0.5) + f.cy;
        }
        if (!(flags & SWP_NOMOVE)) {
            // the engine centres the window for the size it believes in, which pushes the real
            // (smaller) window off screen; keep the real window inside the work area
            int w = cx, hgt = cy;
            if (flags & SWP_NOSIZE) {
                RECT wr;
                GetWindowRect(h, &wr);
                w = wr.right - wr.left;
                hgt = wr.bottom - wr.top;
            }
            MONITORINFO mi{sizeof(mi)};
            o_GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
            x = std::max((int)mi.rcWork.left, std::min(x, (int)mi.rcWork.right - w));
            y = std::max((int)mi.rcWork.top, std::min(y, (int)mi.rcWork.bottom - hgt));
        }
    }
    if (h == g_hwnd) Log("engine SetWindowPos -> %d,%d %dx%d flags 0x%x", x, y, cx, cy, flags);
    return o_SetWindowPos(h, after, x, y, cx, cy, flags);
}

bool PatchIat(const char* dll, const char* func, void* hook, void** original)
{
    auto base = (BYTE*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, func) != 0) continue;
            void** slot = (void**)&addrs->u1.Function;
            if (*slot == hook) return true;  // already hooked (safe to call again)
            DWORD old;
            VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
            if (!*original) *original = *slot;  // keep the first original if re-applied
            *slot = hook;
            VirtualProtect(slot, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

} // namespace

bool InstallWindowHooks()
{
    if (!g_config.fitWindow) return false;
    bool ok = PatchIat("user32.dll", "GetClientRect", (void*)Hook_GetClientRect, (void**)&o_GetClientRect) &&
              PatchIat("user32.dll", "ScreenToClient", (void*)Hook_ScreenToClient, (void**)&o_ScreenToClient) &&
              PatchIat("user32.dll", "ClientToScreen", (void*)Hook_ClientToScreen, (void**)&o_ClientToScreen) &&
              PatchIat("user32.dll", "SetWindowPos", (void*)Hook_SetWindowPos, (void**)&o_SetWindowPos) &&
              PatchIat("user32.dll", "GetWindowRect", (void*)Hook_GetWindowRect, (void**)&o_GetWindowRect) &&
              PatchIat("user32.dll", "GetMonitorInfoW", (void*)Hook_GetMonitorInfoW, (void**)&o_GetMonitorInfoW) &&
              PatchIat("user32.dll", "GetMonitorInfoA", (void*)Hook_GetMonitorInfoA, (void**)&o_GetMonitorInfoA) &&
              PatchIat("user32.dll", "SetWindowsHookExW", (void*)Hook_SetWindowsHookExW, (void**)&o_SetWindowsHookExW) &&
              PatchIat("user32.dll", "SetCursorPos", (void*)Hook_SetCursorPos, (void**)&o_SetCursorPos) &&
              PatchIat("user32.dll", "ClipCursor", (void*)Hook_ClipCursor, (void**)&o_ClipCursor);
    Log("window fit hooks %s", ok ? "installed" : "FAILED");
    return ok;
}

void FitWindow(HWND hwnd, UINT renderW, UINT renderH)
{
    if (hwnd) g_gameHwnd = hwnd;
    if (!g_config.fitWindow || !o_GetClientRect || !hwnd || renderW == 0 || renderH == 0) return;
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!(style & WS_CAPTION) && !(style & WS_THICKFRAME) && !(style & WS_BORDER)) {
        // borderless fullscreen: the window is the monitor, nothing to fit
        if (hwnd == g_hwnd) g_scale = 1.0;
        return;
    }

    MONITORINFO mi{sizeof(mi)};
    o_GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int workW = mi.rcWork.right - mi.rcWork.left;
    int workH = mi.rcWork.bottom - mi.rcWork.top;

    if (hwnd == g_hwnd && g_vw && renderW < g_vw && renderH < g_vh) {
        Log("ignoring resize to %ux%u (smaller than render size %ux%u)", renderW, renderH, g_vw, g_vh);
        renderW = g_vw;
        renderH = g_vh;
    }
    if (hwnd != g_hwnd) {
        g_hwnd = hwnd;
        g_origProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
    }
    g_vw = renderW;
    g_vh = renderH;
    g_scale = 1.0;  // measure the frame with the hooks off
    SIZE f = FrameSize(hwnd);
    g_frame = f;
    double s = std::min(1.0, std::min((double)(workW - f.cx) / renderW, (double)(workH - f.cy) / renderH));
    if (s >= 0.999) {
        Log("window %ux%u fits the screen, no scaling", renderW, renderH);
        return;
    }
    int cw = (int)(renderW * s + 0.5), ch = (int)(renderH * s + 0.5);
    // arm the translation first so every message caused by the resize already sees the full size
    g_scale = (double)cw / renderW;
    g_realOuter = {cw + f.cx, ch + f.cy};
    g_applying = true;
    o_SetWindowPos(hwnd, nullptr, mi.rcWork.left + (workW - cw - f.cx) / 2, mi.rcWork.top, cw + f.cx, ch + f.cy,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    g_applying = false;
    RECT wr, cr;
    GetWindowRect(hwnd, &wr);
    o_GetClientRect(hwnd, &cr);
    if (cr.right > 0) g_scale = (double)cr.right / renderW;  // use what Windows actually gave us
    Log("window shrunk to client %ldx%ld at %ld,%ld (render stays %ux%u, scale %.3f)", cr.right, cr.bottom,
        wr.left, wr.top, renderW, renderH, g_scale);
}

void VirtualSize(HWND hwnd, UINT& w, UINT& h)
{
    if (Active(hwnd)) {
        w = g_vw;
        h = g_vh;
    } else {
        w = h = 0;
    }
}

bool CursorInBackbuffer(UINT bbW, UINT bbH, float& x, float& y)
{
    if (!g_gameHwnd) return false;
    CURSORINFO ci{sizeof(ci)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return false;
    POINT p = ci.ptScreenPos;
    if (o_ScreenToClient)
        o_ScreenToClient(g_gameHwnd, &p);  // real client pixels
    else
        ScreenToClient(g_gameHwnd, &p);
    RECT cr;
    if (o_GetClientRect)
        o_GetClientRect(g_gameHwnd, &cr);
    else
        GetClientRect(g_gameHwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return false;
    if (p.x < 0 || p.y < 0 || p.x >= cr.right || p.y >= cr.bottom) return false;
    x = (float)p.x * bbW / cr.right;
    y = (float)p.y * bbH / cr.bottom;
    return true;
}

void UpdateMouseClip()
{
    if (!g_config.clipMouse || !g_gameHwnd) return;
    bool focused = GetForegroundWindow() == g_gameHwnd && !IsIconic(g_gameHwnd);
    if (!focused) {
        if (g_weClipped) ClipCursor(nullptr);
        g_weClipped = false;
        return;
    }
    if (g_gameClip) {
        g_weClipped = false;  // the game's own clip (mouse look) wins
        return;
    }
    // keep the cursor inside the real client area of the game window
    RECT cr;
    if (o_GetClientRect)
        o_GetClientRect(g_gameHwnd, &cr);
    else
        GetClientRect(g_gameHwnd, &cr);
    POINT a = {0, 0}, b = {cr.right, cr.bottom};
    ::ClientToScreen(g_gameHwnd, &a);
    ::ClientToScreen(g_gameHwnd, &b);
    RECT want = {a.x, a.y, b.x, b.y};
    RECT cur;
    GetClipCursor(&cur);
    if (!g_weClipped || memcmp(&cur, &want, sizeof(RECT)) != 0) {
        ClipCursor(&want);
        g_weClipped = true;
    }
}
