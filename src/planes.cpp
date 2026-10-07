#include "planes.h"
#include "game.h"
#include "hooks.h"
#include "log.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace planes {
namespace {

// Display plane provider vtable slots (byte offsets / 8), from the engine's single-plane
// providers (game window, display texture):
enum Slot {
    kDtor = 0,
    kCount = 7,         // 0x38 () -> number of planes
    kEnabled = 8,       // 0x40 (i) -> bool
    kNeedsPresent = 9,  // 0x48 (i)
    kOutputSlot = 10,   // 0x50 (i) -> int
    kFrustum = 11,      // 0x58 (i, bool* ortho, float* l, float* r, float* b, float* t), in/out tangents
    kViewport = 12,     // 0x60 (i, int* x, int* y)
    kAspect = 13,       // 0x68 (i, flag) -> float
    kOffset = 16,       // 0x80 (i, float out[3]) camera-space offset of the plane's view
    kWidth = 18,        // 0x90 (i)
    kHeight = 19,       // 0x98 (i)
    kBind = 21,         // 0xa8 (renderer, i, 0) binds the plane's output
    kPresent = 22,      // 0xb0 (renderer, i)
    kSlots = 25,
};

using PFN_Alloc = void* (*)(size_t);
using PFN_PlaneCtor = void* (*)(void* mem, void* window, float aspect);
using PFN_Engine = char* (*)();

PFN_Alloc     g_alloc = nullptr;
PFN_PlaneCtor g_ctor = nullptr;
PFN_Engine    g_engine = nullptr;  // engine singleton; +0x110 = renderer (its +0x500 = queued overlays)
bool          g_installed = false;

struct Composite {
    void** vtbl;
    void*  inner;  // the window plane (plane 0)
    int    eye;    // -1: both planes; 0/1: only that plane (as plane 0)
};
void*     g_vtbl[kSlots] = {};
Composite g_composite{nullptr, nullptr, -1};
Composite g_eyeOnly[2] = {{nullptr, nullptr, 0}, {nullptr, nullptr, 1}};  // per-eye scene views

std::mutex g_mtx;
std::atomic<bool> g_on{false};
void*  g_texPlane = nullptr;   // the render overlay's DisplayTexture plane (plane 1)
float  g_offset[3] = {};
float  g_tans[4] = {};
bool   g_haveEye = false;
bool   g_twoThisFrame = false;  // decided once per frame, when the engine fetches the provider

// shared with the sun shadow thunks (see InstallSunShadows)
struct SunShadowData {
    void*   eye[2];  // +0x00 the eye light views' contexts (this frame)
    uint8_t on;      // +0x10 two planes this frame
    uint8_t pad[7];
    void*   scene;   // +0x18 the scene view's light context (this frame)
};
SunShadowData* g_sun = nullptr;


template <typename T> T Method(void* obj, int slot) { return (T)(*(void***)obj)[slot]; }

// plane 1 is the right eye (the overlay's texture), plane 0 the window (left eye)
uint32_t Global(Composite* c, uint32_t i) { return c->eye >= 0 ? (uint32_t)c->eye : i; }
void* PlaneFor(Composite* c, uint32_t i)
{
    if (!g_twoThisFrame) return c->inner;
    return Global(c, i) == 1 ? g_texPlane : c->inner;
}
uint32_t LocalIndex(Composite* c, uint32_t i) { return g_twoThisFrame ? 0 : i; }
bool IsRight(Composite* c, uint32_t i) { return g_twoThisFrame && Global(c, i) == 1; }

// --- routed methods --------------------------------------------------------------------------

uint32_t Count(Composite* c)
{
    if (g_twoThisFrame) return c->eye >= 0 ? 1 : 2;
    return Method<uint32_t (*)(void*)>(c->inner, kCount)(c->inner);
}

uint64_t Enabled(Composite* c, uint32_t i)
{
    void* p = PlaneFor(c, i);
    return Method<uint64_t (*)(void*, uint32_t)>(p, kEnabled)(p, LocalIndex(c, i));
}

uint64_t NeedsPresent(Composite* c, uint32_t i)
{
    void* p = PlaneFor(c, i);
    return Method<uint64_t (*)(void*, uint32_t)>(p, kNeedsPresent)(p, LocalIndex(c, i));
}

void Frustum(Composite* c, uint32_t i, bool* ortho, float* l, float* r, float* b, float* t)
{
    void* p = PlaneFor(c, i);
    Method<void (*)(void*, uint32_t, bool*, float*, float*, float*, float*)>(p, kFrustum)(p, LocalIndex(c, i), ortho, l,
                                                                                          r, b, t);
    if (IsRight(c, i)) {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (g_haveEye) {
            *l = g_tans[0];
            *r = g_tans[1];
            *b = g_tans[2];
            *t = g_tans[3];
        }
    }
}

void Viewport(Composite* c, uint32_t i, int* x, int* y)
{
    void* p = PlaneFor(c, i);
    Method<void (*)(void*, uint32_t, int*, int*)>(p, kViewport)(p, LocalIndex(c, i), x, y);
}

float Aspect(Composite* c, uint32_t i, uint8_t flag)
{
    void* p = PlaneFor(c, i);
    return Method<float (*)(void*, uint32_t, uint8_t)>(p, kAspect)(p, LocalIndex(c, i), flag);
}

void Offset(Composite* c, uint32_t i, float* out)
{
    void* p = PlaneFor(c, i);
    Method<void (*)(void*, uint32_t, float*)>(p, kOffset)(p, LocalIndex(c, i), out);
    if (IsRight(c, i)) {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (g_haveEye) {
            out[0] += g_offset[0];
            out[1] += g_offset[1];
            out[2] += g_offset[2];
        }
    }
}

uint32_t Width(Composite* c, uint32_t i)
{
    void* p = PlaneFor(c, i);
    return Method<uint32_t (*)(void*, uint32_t)>(p, kWidth)(p, LocalIndex(c, i));
}

uint32_t Height(Composite* c, uint32_t i)
{
    void* p = PlaneFor(c, i);
    return Method<uint32_t (*)(void*, uint32_t)>(p, kHeight)(p, LocalIndex(c, i));
}

void Bind(Composite* c, void* renderer, uint32_t i, uint64_t arg)
{
    void* p = PlaneFor(c, i);
    Method<void (*)(void*, void*, uint32_t, uint64_t)>(p, kBind)(p, renderer, LocalIndex(c, i), arg);
}

uint64_t OutputSlot(Composite* c, uint32_t i)
{
    void* p = PlaneFor(c, i);
    return Method<uint64_t (*)(void*, uint32_t)>(p, kOutputSlot)(p, LocalIndex(c, i));
}

void Present(Composite* c, void* renderer, uint32_t i)
{
    void* p = PlaneFor(c, i);
    Method<void (*)(void*, void*, uint32_t)>(p, kPresent)(p, renderer, LocalIndex(c, i));
}

// Every other slot goes to the window plane: mov rcx,[rcx+8]; mov rax,[rcx]; jmp [rax+slot*8]
bool BuildVtable()
{
    BYTE* code = (BYTE*)VirtualAlloc(nullptr, kSlots * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return false;
    for (int s = 0; s < kSlots; s++) {
        BYTE* p = code + s * 16;
        const BYTE stub[] = {0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0xA0};
        memcpy(p, stub, sizeof(stub));
        int32_t disp = s * 8;
        memcpy(p + sizeof(stub), &disp, 4);
        g_vtbl[s] = p;
    }
    g_vtbl[kCount] = (void*)Count;
    g_vtbl[kEnabled] = (void*)Enabled;
    g_vtbl[kNeedsPresent] = (void*)NeedsPresent;
    g_vtbl[kFrustum] = (void*)Frustum;
    g_vtbl[kViewport] = (void*)Viewport;
    g_vtbl[kAspect] = (void*)Aspect;
    g_vtbl[kOffset] = (void*)Offset;
    g_vtbl[kWidth] = (void*)Width;
    g_vtbl[kHeight] = (void*)Height;
    g_vtbl[kBind] = (void*)Bind;
    g_vtbl[kOutputSlot] = (void*)OutputSlot;
    g_vtbl[kPresent] = (void*)Present;
    g_composite.vtbl = g_vtbl;
    g_eyeOnly[0].vtbl = g_eyeOnly[1].vtbl = g_vtbl;
    return true;
}

// --- the engine's provider lookup, rebuilt ---------------------------------------------------
// Original: mgr+0x20 = optional provider factory; otherwise mgr+0x28 caches a window plane for
// (window->0x68(), aspect), recreated when either changes.
void* Hook_GetPlanes(char* mgr, void* window, float aspect)
{
    void*& cached = *(void**)(mgr + 0x28);
    void* factory = *(void**)(mgr + 0x20);
    if (factory) {
        if (cached) Method<void (*)(void*, int)>(cached, kDtor)(cached, 1);
        cached = nullptr;
        return Method<void* (*)(void*, void*, float)>(factory, 5)(factory, window, aspect);
    }
    void* key = Method<void* (*)(void*)>(window, 13)(window);
    if (!(cached && *(void**)((char*)cached + 0x38) == key &&
          Method<float (*)(void*, uint32_t, uint8_t)>(cached, kAspect)(cached, 0, 0) == aspect)) {
        if (cached) Method<void (*)(void*, int)>(cached, kDtor)(cached, 1);
        cached = g_ctor(g_alloc(0x40), key, aspect);
    }
    // Render overlays queued this frame (shop previews, ...) render their view with the same job
    // identifier as a second plane (plane index << 24): one plane in such frames.
    bool queued = false;
    if (g_on) {
        char* renderer = *(char**)(g_engine() + 0x110);
        queued = renderer && *(void***)(renderer + 0x508) != *(void***)(renderer + 0x500);
    }
    bool two = g_on && g_texPlane && !queued;
    g_twoThisFrame = two;
    if (g_sun) g_sun->on = two;
    if (!g_on || !g_texPlane) return cached;
    g_composite.inner = g_eyeOnly[0].inner = g_eyeOnly[1].inner = cached;
    return &g_composite;
}

// --- sun shadows for the eye light views -----------------------------------------------------
// The sun's shadow cascades are one set for the scene, fitted by the cascade generator to the frustum
// of each view that has flag 0x10; a second such view refits the same cascades to its own frustum
// (the other eye then misses shadows at its outer edge). So the eye light views (see
// InstallLightViews) come without that flag and get no cascades of their own; the scene view (both
// eyes' frustum) keeps fitting them. Without the flag their light contexts would be set up without
// the sun's shadows, so while plane stereo runs, three call sites are redirected to small thunks
// (rel32 calls, so the thunks live near the exe):
//   light pass setup:  call with rcx = light object ([rcx] = context)
//                      -> an eye light view's context takes over the scene context's first light record
//   context setup:     call (rcx = context, dl = view flags bit 0x10)
//                      -> for an eye light view's context: dl = 1, [context+0x30] = 1
//   cascade creation:  2 calls inside the setup (rdi = context) -> skipped for it
BYTE* AllocNear(BYTE* site, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t lo = (uintptr_t)site > 0x70000000 ? (uintptr_t)site - 0x70000000 : gran;
    for (uintptr_t a = ((uintptr_t)site & ~(gran - 1)) - gran; a > lo; a -= gran) {
        void* p = VirtualAlloc((void*)a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) return (BYTE*)p;
    }
    return nullptr;
}

struct Emit {
    BYTE* p;
    void b(std::initializer_list<int> v) { for (int x : v) *p++ = (BYTE)x; }
    void q(uint64_t v) { memcpy(p, &v, 8); p += 8; }
    void jmpAbs(void* t) { b({0x48, 0xB8}); q((uint64_t)t); b({0xFF, 0xE0}); }  // mov rax, t ; jmp rax
    // near jump (jmp: EB, jcc: its short opcode 7x), target set later by land()
    BYTE* jcc(int op)
    {
        if (op == 0xEB) *p++ = 0xE9;
        else { *p++ = 0x0F; *p++ = (BYTE)(op + 0x10); }
        BYTE* d = p;
        p += 4;
        return d;
    }
    void land(BYTE* d) { *(int32_t*)d = (int32_t)(p - (d + 4)); }
};

// call rel32 at `call` -> `thunk`; returns the original target
BYTE* RedirectCall(BYTE* call, BYTE* thunk)
{
    BYTE* target = call + 5 + *(int32_t*)(call + 1);
    int64_t rel = (int64_t)(thunk - (call + 5));
    if (rel != (int32_t)rel) return nullptr;
    DWORD old;
    if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    *(int32_t*)(call + 1) = (int32_t)rel;
    VirtualProtect(call + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return target;
}

// The sun data block of a view without shadow cascades (planar reflections) keeps the light's shadow
// distance and cascade count 0; the shader then takes the cascade-0 matrix from rows that were never
// written for it. With plane stereo the reflections' slots held other views' blocks before, so the
// mirrors of the left eye got a veil that moved with the head. Starting the distance at 0 skips sun
// shadows there; views with cascades overwrite it with their last split right after.
void InstallNoCascadeDistance()
{
    struct Site { const char* pattern; int offset; };
    const Site sites[] = {
        {"F3 44 0F 11 6B 08 8B 87 F0 01 00 00 89 02 4A 8B 44 36 08", 6},  // mov eax, [rdi+1F0h]
        {"8B 81 F0 01 00 00 41 89 07 F3 42 0F 10 74 36 28", 0},          // mov eax, [rcx+1F0h]
    };
    static const BYTE zero[6] = {0x31, 0xC0, 0x0F, 0x1F, 0x40, 0x00};  // xor eax, eax ; nop
    for (const Site& s : sites) {
        BYTE* p = GameFindPattern(s.pattern);
        DWORD old;
        if (!p || !VirtualProtect(p + s.offset, sizeof(zero), PAGE_EXECUTE_READWRITE, &old)) {
            Log("planes: sun distance site not found: mirrors may show a shadow veil in plane stereo");
            continue;
        }
        memcpy(p + s.offset, zero, sizeof(zero));
        VirtualProtect(p + s.offset, sizeof(zero), old, &old);
        FlushInstructionCache(GetCurrentProcess(), p + s.offset, sizeof(zero));
    }
}

bool InstallSunShadows()
{
    BYTE* pass = GameFindPattern("F3 44 0F 11 44 24 30 48 8B D7 F3 0F 11 7C 24 28 48 8B CE F3 0F 11 74 24 20 "
                                 "E8 ?? ?? ?? ?? 48 8B 03");
    BYTE* setup = GameFindPattern(
        "8B 90 F8 03 00 00 4C 8D 80 10 01 00 00 8B 87 94 03 00 00 C1 EA 04 80 E2 01 89 44 24 20 E8");
    BYTE* create1 = GameFindPattern("45 8B CF 4D 8B 46 F8 41 8B 16 48 8B 49 58 E8");
    BYTE* create2 = GameFindPattern("C7 44 24 20 FF FF FF FF 45 8B CD 4D 8B 46 F8 48 8B 49 58 E8");
    if (!pass || !setup || !create1 || !create2) {
        Log("planes: sun shadow sites not found (%p %p %p %p): right eye without sun shadows", pass, setup,
            create1, create2);
        return false;
    }
    pass += 25;
    setup += 29;
    create1 += 14;
    create2 += 19;
    if (*create1 != 0xE8 || *create2 != 0xE8 ||
        create1 + 5 + *(int32_t*)(create1 + 1) != create2 + 5 + *(int32_t*)(create2 + 1)) {
        Log("planes: sun shadow cascade calls differ: right eye without sun shadows");
        return false;
    }
    BYTE* mem = AllocNear(pass, 4096);
    if (!mem) {
        Log("planes: no memory near the game code: right eye without sun shadows");
        return false;
    }
    g_sun = (SunShadowData*)mem;
    BYTE* passThunk = mem + 0x40;
    BYTE* setupThunk = mem + 0x180;
    BYTE* createThunk = mem + 0x1C0;
    BYTE* passTarget = pass + 5 + *(int32_t*)(pass + 1);
    BYTE* setupTarget = setup + 5 + *(int32_t*)(setup + 1);
    BYTE* createTarget = create1 + 5 + *(int32_t*)(create1 + 1);

    // An eye light view's light pass takes over the first light record (cascade count, focus cascade flag,
    // cascade pointers, split distances) of the scene view's context: the cascade generator only
    // completes the scene's, so the eye's splits would stay uninitialized and hard shadows would pick
    // wrong cascades (only near, drifting, or none at all).
    Emit e{passThunk};
    e.b({0x49, 0xBA}); e.q((uint64_t)g_sun);          // mov r10, g_sun
    e.b({0x41, 0x80, 0x7A, 0x10, 0x00});              // cmp byte [r10+10h], 0 (other modes' contexts
    BYTE* done1 = e.jcc(0x74);                        // je done                own their cascades)
    e.b({0x4C, 0x8B, 0x19});                          // mov r11, [rcx] (the context)
    e.b({0x4D, 0x3B, 0x1A});                          // cmp r11, [r10]
    BYTE* eye = e.jcc(0x74);                          // je eye
    e.b({0x4D, 0x3B, 0x5A, 0x08});                    // cmp r11, [r10+8]
    BYTE* done2 = e.jcc(0x75);                        // jne done
    e.land(eye);
    e.b({0x49, 0x8B, 0x42, 0x18});                    // mov rax, [r10+18h] (the scene's context)
    e.b({0x48, 0x85, 0xC0});                          // test rax, rax
    BYTE* done3 = e.jcc(0x74);                        // je done
    e.b({0x48, 0x8B, 0x40, 0x10});                    // mov rax, [rax+10h] (its first record)
    e.b({0x4D, 0x8B, 0x5B, 0x10});                    // mov r11, [r11+10h]
    e.b({0x48, 0x85, 0xC0});                          // test rax, rax
    BYTE* done4 = e.jcc(0x74);                        // je done
    e.b({0x4D, 0x85, 0xDB});                          // test r11, r11
    BYTE* done5 = e.jcc(0x74);                        // je done
    e.b({0x80, 0x38, 0x00});                          // cmp byte [rax], 0 (no cascades)
    BYTE* done6 = e.jcc(0x74);                        // je done
    for (int off = 0; off < 0x80; off += 8) {
        e.b({0x4C, 0x8B, 0x50, off});                 // mov r10, [rax+off]
        e.b({0x4D, 0x89, 0x53, off});                 // mov [r11+off], r10
    }
    for (BYTE* d : {done1, done2, done3, done4, done5, done6}) e.land(d);
    e.jmpAbs(passTarget);

    // setup and cascade creation: is the context (rcx / rdi) an eye light view's?
    auto isEye = [&](int cmpLo, int cmpHi) {
        e.b({0x49, 0xBA}); e.q((uint64_t)g_sun);      // mov r10, g_sun
        e.b({0x41, 0x80, 0x7A, 0x10, 0x00});          // cmp byte [r10+10h], 0
        BYTE* no1 = e.jcc(0x74);                      // je no
        e.b({0x49, 0x3B, cmpLo});                     // cmp reg, [r10]
        BYTE* yes = e.jcc(0x74);                      // je yes
        e.b({0x49, 0x3B, cmpHi, 0x08});               // cmp reg, [r10+8]
        BYTE* no2 = e.jcc(0x75);                      // jne no
        e.land(yes);
        return std::make_pair(no1, no2);
    };
    e.p = setupThunk;
    auto [s1, s2] = isEye(0x0A, 0x4A);                // rcx
    e.b({0xB2, 0x01});                                // mov dl, 1
    e.b({0xC6, 0x41, 0x30, 0x01});                    // mov byte [rcx+30h], 1
    e.land(s1);
    e.land(s2);
    e.jmpAbs(setupTarget);

    e.p = createThunk;
    auto [c1, c2] = isEye(0x3A, 0x7A);                // rdi
    e.b({0xC3});                                      // ret (skipped; the result is not used)
    e.land(c1);
    e.land(c2);
    e.jmpAbs(createTarget);
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);

    if (!RedirectCall(pass, passThunk) || !RedirectCall(setup, setupThunk) ||
        !RedirectCall(create1, createThunk) || !RedirectCall(create2, createThunk)) {
        Log("planes: sun shadow call sites could not be redirected");
        return false;
    }
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("planes: eye light view sun shadows: light pass +%llx, context setup +%llx, cascades +%llx/+%llx",
        (unsigned long long)(pass - base), (unsigned long long)(setup - base), (unsigned long long)(create1 - base),
        (unsigned long long)(create2 - base));
    return true;
}

// --- a light view per eye --------------------------------------------------------------------
// The engine takes one scene view per frame from its view pool for all planes; its frustum is the
// union of the planes' frusta. Per view the frame scheduler prepares a light object (light context,
// local lights binned into screen tiles), and a plane's subview keeps the light object of the view
// it was created with. So the right eye got the light object of some other view (no local lights)
// or, sharing the scene view's, tiles binned for the union frustum (lights cut off in steps).
// While two planes render, two light views are taken from the pool (the pool creates their light
// objects with them), each set up for one eye, and each plane's subview takes its eye's light
// object. Both planes still draw the scene view as the engine sets it up: it fits the shadow
// cascades to both eyes, and only it has the per-view buffers the full draw needs (a second view
// with them races in the engine's job code: crashes; without them some draws find no buffer).
// Subviews come from a pool too and keep the light object they were created with, while the view
// they are handed changes from frame to frame: with the extra views, two subviews could work on one
// light object in a frame (its lists are rebuilt while the other reads them: crashes). So every
// subview takes the light object of the view it is handed, whenever it is handed out.
using PFN_AllocView = char* (*)(void* pool, uint64_t a2, uint32_t flags, uint32_t a4, uint64_t a5, uint64_t a6,
                                uint64_t a7);
using PFN_SetupView = void (*)(char* view, void* camera, void* provider, float a4);
using PFN_SubView = char* (*)(void* pool, void* renderer, uint32_t job, uint32_t a4, uint64_t a5, uint64_t a6,
                              uint64_t a7, char* view);

// The light views' flags: lit only, like a mirror's view (no own sun cascades, see InstallSunShadows).
constexpr uint32_t kLightViewFlags = 0x1;

PFN_AllocView g_allocView = nullptr;
PFN_SetupView g_setupView = nullptr;
PFN_SubView   g_subView = nullptr;
char*         g_eyeView[2] = {};  // the light views of the frame being set up

char* Hook_AllocView(void* pool, uint64_t a2, uint32_t flags, uint32_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    char* view = g_allocView(pool, a2, flags, a4, a5, a6, a7);
    for (char*& eye : g_eyeView)
        eye = g_twoThisFrame ? g_allocView(pool, a2, kLightViewFlags, a4, a5, a6, a7) : nullptr;
    return view;
}

void Hook_SetupView(char* view, void* camera, void* provider, float a4)
{
    g_setupView(view, camera, provider, a4);
    if (!g_eyeView[0] || !g_eyeView[1] || provider != &g_composite) return;
    for (int i = 0; i < 2; i++) {
        char* eye = g_eyeView[i];
        g_setupView(eye, camera, &g_eyeOnly[i], a4);
        *(char**)(eye + 0x440) = eye + 0x220;  // as the engine does for the scene view
        if (g_sun) g_sun->eye[i] = *(void**)(eye + 0x470);
    }
    if (g_sun) g_sun->scene = *(void**)(view + 0x470);
}

char* Hook_SubView(void* pool, void* renderer, uint32_t job, uint32_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                   char* view)
{
    char* sub = g_subView(pool, renderer, job, a4, a5, a6, a7, view);
    uint32_t plane = job >> 24;  // the main render numbers its planes: job id = plane index << 24
    if (!sub || !g_eyeView[0] || !g_eyeView[1] || plane > 1 || (job & 0xFFFFFF)) return sub;
    char* eye = g_eyeView[plane];
    memcpy(eye + 0x408, view + 0x408, 8);  // output size, set per plane on the scene view
    *(void**)(sub + 0x630) = *(void**)(eye + 0x968);  // the subview's light object
    return sub;
}

bool InstallLightViews()
{
    BYTE* main = GameFindPattern("41 B8 B7 01 00 00 44 89 74 24 28 89 44 24 20 E8 ?? ?? ?? ?? 48 8B 8E ?? ?? ?? ?? "
                                 "4D 8B C7 48 8B 96 ?? ?? ?? ?? 48 8B D8 48 89 44 24 70 F3 0F 10 99 ?? ?? ?? ?? "
                                 "48 8B C8 E8");
    BYTE* sub = GameFindPattern("48 8D 4C 24 48 48 89 4C 24 20 48 8D 8E ?? ?? ?? ?? 48 89 45 80 E8 ?? ?? ?? ?? "
                                "4C 8B E8 48 89 98");
    // inside the subview allocator: re-initialises the handed-out subview (rbx), the view in r15
    BYTE* reinit = GameFindPattern("41 8B D5 48 8D 4B 20 E8 ?? ?? ?? ?? B9 10 00 00 00");
    if (!main || !sub || !reinit) {
        Log("planes: scene view sites not found (%p %p %p): right eye without local lights", main, sub, reinit);
        return false;
    }
    BYTE* reinitCall = reinit + 7;
    BYTE* allocCall = main + 15;
    BYTE* setupCall = main + 56;
    BYTE* subCall = sub + 21;
    BYTE* mem = AllocNear(allocCall, 4096);
    if (!mem) {
        Log("planes: no memory near the game code: right eye without local lights");
        return false;
    }
    Emit e{mem};
    e.jmpAbs((void*)Hook_AllocView);
    e.p = mem + 0x20;
    e.jmpAbs((void*)Hook_SetupView);
    e.p = mem + 0x40;
    e.jmpAbs((void*)Hook_SubView);
    e.p = mem + 0x60;
    e.b({0x4D, 0x85, 0xFF});                          // test r15, r15
    BYTE* noView = e.jcc(0x74);                       // je skip
    e.b({0x49, 0x8B, 0x87, 0x68, 0x09, 0x00, 0x00});  // mov rax, [r15+968h] (the view's light object)
    e.b({0x48, 0x85, 0xC0});                          // test rax, rax
    BYTE* noLights = e.jcc(0x74);                     // je skip
    e.b({0x48, 0x89, 0x83, 0x50, 0x06, 0x00, 0x00});  // mov [rbx+650h], rax (the subview's)
    e.land(noView);
    e.land(noLights);
    e.jmpAbs(reinitCall + 5 + *(int32_t*)(reinitCall + 1));
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    g_allocView = (PFN_AllocView)(allocCall + 5 + *(int32_t*)(allocCall + 1));
    g_setupView = (PFN_SetupView)(setupCall + 5 + *(int32_t*)(setupCall + 1));
    g_subView = (PFN_SubView)(subCall + 5 + *(int32_t*)(subCall + 1));
    if (!RedirectCall(reinitCall, mem + 0x60) || !RedirectCall(allocCall, mem) ||
        !RedirectCall(setupCall, mem + 0x20) || !RedirectCall(subCall, mem + 0x40)) {
        Log("planes: scene view call sites could not be redirected");
        return false;
    }
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("planes: light view per eye: view +%llx, setup +%llx, subview +%llx", (unsigned long long)(allocCall - base),
        (unsigned long long)(setupCall - base), (unsigned long long)(subCall - base));
    return true;
}

} // namespace

bool Install()
{
    if (g_installed) return true;
    // the provider lookup: cmp [rcx+20h], 0 at its start; allocation and constructor calls inside
    BYTE* f = GameFindPattern(
        "48 89 5C 24 10 57 48 83 EC 30 0F 29 74 24 20 0F 28 F2 48 8B FA 48 8B D9 48 83 79 20 00 74 38");
    // updateRenderOverlay: calls the engine singleton getter, then queues at renderer+0x500
    BYTE* u = GameFindPattern("40 53 55 48 83 EC 28 48 8B E9 E8 ?? ?? ?? ?? 80 BD 3C 01 00 00 00 48 8B 98 10 01 00 00");
    if (!f || !u || f[0x99] != 0xB9 || f[0x9E] != 0xE8 || f[0xB1] != 0xE8 || u[10] != 0xE8) {
        Log("planes: engine functions not found (lookup %p, overlay %p): plane stereo unavailable", f, u);
        return false;
    }
    g_alloc = (PFN_Alloc)(f + 0x9E + 5 + *(int32_t*)(f + 0x9F));
    g_ctor = (PFN_PlaneCtor)(f + 0xB1 + 5 + *(int32_t*)(f + 0xB2));
    g_engine = (PFN_Engine)(u + 10 + 5 + *(int32_t*)(u + 11));
    if (!BuildVtable()) return false;
    InstallSunShadows();
    InstallNoCascadeDistance();
    InstallLightViews();
    if (!WriteJump(f, (void*)Hook_GetPlanes)) {
        Log("planes: could not hook the provider lookup");
        return false;
    }
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log("planes: provider lookup +%llx hooked (alloc +%llx, plane ctor +%llx, engine +%llx)",
        (unsigned long long)(f - base), (unsigned long long)((BYTE*)g_alloc - base),
        (unsigned long long)((BYTE*)g_ctor - base), (unsigned long long)((BYTE*)g_engine - base));
    g_installed = true;
    return true;
}

bool SetStereo(bool on)
{
    if (!g_installed) return false;
    if (!on) {
        g_on = false;
        Log("planes: stereo off");
        return true;
    }
    // the overlay the Lua mod just queued: last entry of the renderer's overlay queue
    char* renderer = *(char**)(g_engine() + 0x110);
    void** begin = renderer ? *(void***)(renderer + 0x500) : nullptr;
    void** end = renderer ? *(void***)(renderer + 0x508) : nullptr;
    if (!renderer || end <= begin) {
        Log("planes: no queued render overlay");
        return false;
    }
    char* overlay = (char*)end[-1];
    void* tex = overlay + 0x88;  // the overlay's DisplayTexture plane
    // take it out of the queue again: rendered by itself it would use the second plane's job id
    *(void***)(renderer + 0x508) = end - 1;
    overlay[0x13C] = 0;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_texPlane = tex;
    }
    g_on = true;
    Log("planes: stereo on, overlay %p, texture plane %p (%ux%u)", overlay, tex, *(uint32_t*)((char*)tex + 8),
        *(uint32_t*)((char*)tex + 0xC));
    return true;
}

bool Active() { return g_on && g_texPlane; }

void SetOtherEye(const float offset[3], const float tans[4])
{
    std::lock_guard<std::mutex> lock(g_mtx);
    memcpy(g_offset, offset, sizeof(g_offset));
    memcpy(g_tans, tans, sizeof(g_tans));
    g_haveEye = true;
}

} // namespace planes
