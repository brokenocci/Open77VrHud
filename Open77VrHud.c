/*
 * Open77 VR HUD — LibOVR bridge for R.E.A.L. VR and Virtual Desktop.
 *
 * Measured on Cyberpunk 2077 2.31, R.E.A.L. VR 26.3, Quest 3:
 *  - R.E.A.L. submits through LibOVR. Each ovr_EndFrame carries two layers:
 *    type 1 (eyes) and type 3 (the HUD board).
 *  - A current LibOVR layer header is Type + Flags + Reserved[128] = 136 bytes.
 *    The quad texture pointer is at +136. Offset +8 is the legacy layout and is
 *    empty on this runtime.
 *  - Open77 imports each WebUI page as three shared D3D12 textures. Those
 *    handles are reopened on the D3D11 device R.E.A.L. uses for LibOVR.
 *  - The pages are composited into our own LibOVR swapchain and submitted as
 *    a third quad layer that copies the board pose.
 *
 * Hooks:
 *  - ID3D12Device vtable slot 32 (OpenSharedHandle)
 *  - ovr_CreateTextureSwapChainDX prologue (session and D3D11 device)
 *  - ovr_EndFrame: tail jump on the Virtual Desktop build, prologue on the
 *    Meta Horizon build
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <psapi.h>

typedef struct { uint8_t major, pad; uint16_t minor; uint32_t patch, pre_type, pre_num; } SemVer;
typedef struct { SemVer sdk; const wchar_t* name; const wchar_t* author; SemVer version; int64_t runtime; } PluginInfo;

static const GUID kRes12   = {0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};
static const GUID kFence12 = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
static const GUID kDev12   = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
static const GUID kTex11   = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
static const GUID kDev11   = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};
static const GUID kDev11_1 = {0xa04bfb29, 0x08ef, 0x43d6, {0xa4, 0x9c, 0xa9, 0xbd, 0xbd, 0xcb, 0xe6, 0x86}};
static const GUID kMulti11 = {0x9b7e4e00, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}};

#define RING_MAX 48
#define RING_SLOTS 3
#define JOB_MAX 24
#define CHAIN_MAX 8
#define LAYER_MAX 16
#define VP_MAX 16

/* Open77 imports each WebUI surface as a ring of 3 shared textures
   ("ring revision 1 imported: 3 slots"), all from the same thread within a
   few milliseconds. CEF rotates frames across the slots, so older slots keep
   stale frames. Only the most recently written slot is drawn. */
typedef struct {
    ID3D12Resource* res;              /* our ref: detect when Open77 releases it */
    HANDLE dup;
    ID3D11Texture2D* tex11;
    ID3D11ShaderResourceView* srv;
    int open_fail;
    uint64_t sig;
    int sig_valid;
    unsigned alpha;
} Slot;

typedef struct {
    int used, dead;
    unsigned id;
    DWORD tid;
    DWORD t_wall;                     /* ms since midnight, matched against the Open77 log */
    char name[40];                    /* Open77 surface name, from the import log line */
    double t_last;
    UINT w, h;
    DXGI_FORMAT fmt;
    int n;
    Slot s[RING_SLOTS];
    int newest;
    unsigned passes;
    unsigned pend_id;
    int pend_want, pend_mask;
    uint64_t pend_sig[RING_SLOTS];
    unsigned pend_alpha[RING_SLOTS];
    DWORD last_change;
} Ring;

typedef struct {
    int busy, ring, slot;
    unsigned ring_id, pass_id, issued;
    ID3D11Texture2D* stage;
    UINT sw, sh;
    DXGI_FORMAT sfmt;
} Job;

typedef struct {
    ID3D11RenderTargetView* rtv;
    ID3D11DepthStencilView* dsv;
    D3D11_VIEWPORT vp[VP_MAX];
    UINT nvp;
    ID3D11BlendState* bs;
    FLOAT bf[4];
    UINT mask;
    ID3D11RasterizerState* rs;
    ID3D11InputLayout* il;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11ShaderResourceView* srv;
    ID3D11SamplerState* ss;
    ID3D11DepthStencilState* dss;
    UINT sref;
} Saved;

static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_stop;
static int g_enabled = 1, g_tint = -1, g_dump = 2;
static int g_select_newest = 1, g_only_bgra = 1, g_sample_rings = 2, g_static_hide_ms = 0;
static wchar_t g_dir[MAX_PATH], g_log_path[MAX_PATH], g_ini_path[MAX_PATH];

static Ring g_rings[RING_MAX];
static unsigned g_ring_seq, g_pass_seq;
static int g_ring_rr;
static Job g_jobs[JOB_MAX];
static ID3D11Texture2D* g_scratch;
static ID3D11ShaderResourceView* g_scratch_srv;
static UINT g_scratch_w, g_scratch_h, g_scratch_mip, g_scratch_mips;
static DXGI_FORMAT g_scratch_fmt;
static int g_sample_ok = 1, g_sample_errors;
static int g_fence_logs;
static UINT g_act_w, g_act_h;

/* Native surface visibility, read from the Open77 log. The connection screen
   (open77_shell) is hidden with "WebUI hide applied" and is not repainted, so
   its last frame would otherwise stay in the headset. */
#define VIS_MAX 16
static struct { char name[40]; int hidden; } g_vis[VIS_MAX];
static int g_vis_n;
static int g_follow_log = 1;
static unsigned g_menu_ring;
static int g_menu_hidden;
static char g_hide_list[256];
static wchar_t g_logs_dir[MAX_PATH], g_tail_path[MAX_PATH];
static LONGLONG g_tail_off;
static DWORD g_tail_scan;

static void release_sampling(void);

static void** g_dev12_vt;
static void* g_raw_open;
static int g_d3d12_hooked;

static HMODULE g_lib;
static unsigned char* g_end_jmp;
static int32_t g_end_rel;
static void* g_end_tail;
static void* g_end_stub;
static void* g_end_meta_tramp;
static int g_end_mode;            /* 0 = none, 1 = VD tail jump, 2 = Meta prologue */
static void* g_create_tramp;

/* MSVC prologue shared by ovr_EndFrame and ovr_CreateTextureSwapChainDX in the
   native Meta build (LibOVRRTImpl64_1.dll): three mov [rsp+x], reg instructions,
   15 bytes, and the 15th byte is an instruction boundary. The bytes are
   position-independent, so they can be relocated into a trampoline without a
   disassembler. */
static const unsigned char kMetaProlog[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18
};
static void* g_get_buf;
static void* g_get_index;
static void* g_get_len;
static void* g_commit;
static void* g_create;
static void* g_ovr_session;
static IUnknown* g_ovr_dev;
static int g_create_logs;
static volatile LONG g_reset_pending;
static void* g_sess_arg;

static void* g_sess;
static int g_hdr;
static unsigned char g_board[320];
static int g_board_ok;
static ID3D11Texture2D* g_board_tex;
static void* g_chain;
static int g_chain_len;
static UINT g_cw, g_ch;
static ID3D11Texture2D* g_ctex[CHAIN_MAX];
static ID3D11RenderTargetView* g_crtv[CHAIN_MAX];
static int g_decode;
static ID3D11Device* g_dev;
static ID3D11Device1* g_dev1;
static ID3D11DeviceContext* g_ctx;
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;
static ID3D11PixelShader* g_ps_lin;
static ID3D11PixelShader* g_ps_cursor;
static ID3D11BlendState* g_bs;
static ID3D11SamplerState* g_ss;
static ID3D11RasterizerState* g_rs;
static unsigned char g_quad_out[320];
static unsigned g_frames;
static int g_fail_n, g_bridge_off, g_ok_logged, g_tint_logged, g_sess_fail_logged, g_open_logs, g_layout_logged;
static DWORD g_last_create_try;

/* ------------------------------------------------------------------ utilities */

static void hud_log(const char* fmt, ...) {
    FILE* f = _wfopen(g_log_path, L"a");
    SYSTEMTIME st;
    va_list ap;
    if (!f) return;
    GetLocalTime(&st);
    fprintf(f, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (const unsigned char*)p + n <= (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
}

static int com_vtable(void* obj) {
    MEMORY_BASIC_INFORMATION mbi;
    void** vt;
    if (!obj || !readable(obj, sizeof(void*))) return 0;
    vt = *(void***)obj;
    if (!vt || !VirtualQuery(vt, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE) return 0;
    return 1;
}

static void hook_slot(void** vt, int idx, void* hook, void** saved) {
    DWORD old = 0;
    if (!vt) return;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    if (vt[idx] != hook) {
        if (saved && !*saved) *saved = vt[idx];
        vt[idx] = hook;
    }
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

static int resource_desc(ID3D12Resource* res, D3D12_RESOURCE_DESC* out) {
    typedef D3D12_RESOURCE_DESC*(STDMETHODCALLTYPE* Fn)(ID3D12Resource*, D3D12_RESOURCE_DESC*);
    if (!com_vtable(res)) return 0;
    memset(out, 0, sizeof(*out));
    ((Fn)(*(void***)res)[10])(res, out);
    return out->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && out->Width > 0 && out->Height > 0;
}

static int rgba8(DXGI_FORMAT f) {
    return f == 27 || f == 28 || f == 29 || f == 87 || f == 90 || f == 91;
}

static int read_key(const char* text, const char* key, char* out, int n) {
    size_t klen = strlen(key);
    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == ';' || *p == '#') { while (*p && *p != '\n') p++; continue; }
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            int i = 0;
            p += klen + 1;
            while (*p && *p != '\r' && *p != '\n' && i + 1 < n) out[i++] = *p++;
            out[i] = 0;
            return 1;
        }
        while (*p && *p != '\n') p++;
    }
    return 0;
}

static void load_ini(void) {
    FILE* f = _wfopen(g_ini_path, L"rb");
    char buf[4096], val[32];
    size_t n;
    if (!f) return;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (read_key(buf, "enabled", val, sizeof(val))) g_enabled = atoi(val) != 0;
    if (read_key(buf, "tint", val, sizeof(val))) g_tint = (strcmp(val, "auto") == 0) ? -1 : (atoi(val) != 0);
    if (read_key(buf, "dump", val, sizeof(val))) g_dump = atoi(val);
    if (read_key(buf, "select", val, sizeof(val))) g_select_newest = strcmp(val, "all") != 0;
    if (read_key(buf, "formats", val, sizeof(val))) g_only_bgra = strcmp(val, "all") != 0;
    if (read_key(buf, "sampleRings", val, sizeof(val))) {
        g_sample_rings = atoi(val);
        if (g_sample_rings < 1) g_sample_rings = 1;
        if (g_sample_rings > 8) g_sample_rings = 8;
    }
    if (read_key(buf, "staticHideMs", val, sizeof(val))) g_static_hide_ms = atoi(val);
    if (read_key(buf, "followLog", val, sizeof(val))) g_follow_log = atoi(val) != 0;
    read_key(buf, "hideSurfaces", g_hide_list, sizeof(g_hide_list));
}

/* ------------------------------------------------- Open77 rings (D3D12) */

static double now_ms(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static int bgra(DXGI_FORMAT f) {
    return f == 87 || f == 90 || f == 91;
}

static void release_slot(Slot* s) {
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->tex11) ID3D11Texture2D_Release(s->tex11);
    if (s->dup) CloseHandle(s->dup);
    if (s->res) ID3D12Resource_Release(s->res);
    memset(s, 0, sizeof(*s));
}

static void free_ring_locked(Ring* r) {
    int k;
    if (r->id && r->id == g_menu_ring) g_menu_ring = 0;
    for (k = 0; k < RING_SLOTS; k++) release_slot(&r->s[k]);
    memset(r, 0, sizeof(*r));
}

static Ring* alloc_ring_locked(void) {
    int i, oldest = -1;
    for (i = 0; i < RING_MAX; i++) if (!g_rings[i].used) return &g_rings[i];
    for (i = 0; i < RING_MAX; i++) if (g_rings[i].dead) { free_ring_locked(&g_rings[i]); return &g_rings[i]; }
    for (i = 0; i < RING_MAX; i++) if (oldest < 0 || g_rings[i].id < g_rings[oldest].id) oldest = i;
    free_ring_locked(&g_rings[oldest]);
    return &g_rings[oldest];
}

static void note_shared(void* obj, HANDLE handle) {
    typedef HRESULT(STDMETHODCALLTYPE* QIFn)(void*, const GUID*, void**);
    ID3D12Resource* res = NULL;
    ID3D12Fence* fence = NULL;
    D3D12_RESOURCE_DESC rd;
    HANDLE dup = NULL;
    Ring* r = NULL;
    DWORD tid = GetCurrentThreadId();
    double t = now_ms();
    int i, k, created = 0, complete = 0, changed = 0;
    unsigned rid;
    if (!obj || !com_vtable(obj)) return;
    if (SUCCEEDED(((QIFn)(*(void***)obj)[0])(obj, &kFence12, (void**)&fence)) && fence) {
        ID3D12Fence_Release(fence);
        if (g_fence_logs < 24) {
            g_fence_logs++;
            hud_log("shared fence tid %u", tid);
        }
        return;
    }
    if (FAILED(((QIFn)(*(void***)obj)[0])(obj, &kRes12, (void**)&res)) || !res) return;
    if (!resource_desc(res, &rd) || rd.Width < 256 || rd.Height < 256 || rd.DepthOrArraySize != 1 || !rgba8(rd.Format) || !handle) {
        ID3D12Resource_Release(res);
        return;
    }
    /* Open77 CEF surfaces are single-mip BGRA8. R.E.A.L. eye and board
       swapchains also pass through here, as 3-mip RGBA8 sRGB. */
    if (g_only_bgra && (!bgra(rd.Format) || rd.MipLevels != 1)) {
        static int skipped;
        if (skipped < 8) {
            skipped++;
            hud_log("skip shared %ux%u fmt %u mips %u tid %u (not an Open77 page)", (unsigned)rd.Width, rd.Height, (unsigned)rd.Format, (unsigned)rd.MipLevels, tid);
        }
        ID3D12Resource_Release(res);
        return;
    }
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS) || !dup) {
        ID3D12Resource_Release(res);
        return;
    }
    EnterCriticalSection(&g_lock);
    for (i = 0; i < RING_MAX; i++) {
        Ring* c = &g_rings[i];
        if (!c->used || c->dead || c->tid != tid || c->n >= RING_SLOTS) continue;
        if (c->w != (UINT)rd.Width || c->h != rd.Height || c->fmt != rd.Format) continue;
        if (t - c->t_last > 25.0) continue;
        r = c;
        break;
    }
    if (!r) {
        SYSTEMTIME st;
        r = alloc_ring_locked();
        memset(r, 0, sizeof(*r));
        GetLocalTime(&st);
        r->t_wall = ((st.wHour * 60u + st.wMinute) * 60u + st.wSecond) * 1000u + st.wMilliseconds;
        r->used = 1;
        r->id = ++g_ring_seq;
        r->tid = tid;
        r->w = (UINT)rd.Width;
        r->h = rd.Height;
        r->fmt = rd.Format;
        r->newest = -1;
        r->last_change = GetTickCount();
        created = 1;
    }
    k = r->n++;
    r->s[k].res = res;
    r->s[k].dup = dup;
    r->t_last = t;
    complete = r->n == RING_SLOTS;
    rid = r->id;
    if (g_act_w != r->w || g_act_h != r->h) {
        g_act_w = r->w;
        g_act_h = r->h;
        changed = 1;
    }
    LeaveCriticalSection(&g_lock);
    if (created && rid <= 64)
        hud_log("ring %u new %ux%u fmt %u tid %u%s", rid, (unsigned)rd.Width, rd.Height, (unsigned)rd.Format, tid, changed ? " (active size)" : "");
    if (complete && rid <= 64) hud_log("ring %u complete", rid);
}

static HRESULT STDMETHODCALLTYPE Hook_OpenShared(ID3D12Device* self, HANDLE handle, REFIID iid, void** out) {
    typedef HRESULT(STDMETHODCALLTYPE* Fn)(ID3D12Device*, HANDLE, REFIID, void**);
    HRESULT hr = ((Fn)g_raw_open)(self, handle, iid, out);
    if (SUCCEEDED(hr) && out && *out) note_shared(*out, handle);
    return hr;
}

static void install_d3d12(void) {
    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    typedef HRESULT(WINAPI* CreateFn)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    CreateFn create;
    ID3D12Device* dev = NULL;
    if (!d3d12) return;
    create = (CreateFn)GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!create || FAILED(create(NULL, D3D_FEATURE_LEVEL_11_0, &kDev12, (void**)&dev)) || !dev) {
        hud_log("d3d12 device probe failed");
        g_d3d12_hooked = 1;
        return;
    }
    g_dev12_vt = *(void***)dev;
    hook_slot(g_dev12_vt, 32, (void*)Hook_OpenShared, &g_raw_open);
    ID3D12Device_Release(dev);
    g_d3d12_hooked = 1;
    hud_log("d3d12 OpenSharedHandle observer installed");
}

/* --------------------------------------------------------- hook LibOVR */

static unsigned char* near_stub(unsigned char* site, void* dest) {
    uintptr_t step;
    for (step = 0x10000; step < 0x70000000ull; step += 0x100000) {
        int dir;
        for (dir = 0; dir < 2; dir++) {
            uintptr_t hint = dir ? (uintptr_t)site + step : (uintptr_t)site - step;
            unsigned char* p = (unsigned char*)VirtualAlloc((void*)(hint & ~0xFFFFull), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            intptr_t rel;
            if (!p) continue;
            rel = (intptr_t)p - (intptr_t)site;
            if (rel > 2147483647LL || rel < -2147483647LL) {
                VirtualFree(p, 0, MEM_RELEASE);
                continue;
            }
            p[0] = 0xFF;
            p[1] = 0x25;
            *(uint32_t*)(p + 2) = 0;
            *(void**)(p + 6) = dest;
            return p;
        }
    }
    return NULL;
}

static void* hook_prologue(unsigned char* target, const unsigned char* expect, size_t n, void* hook) {
    unsigned char* tramp;
    DWORD old = 0;
    size_t i;
    if (!target || memcmp(target, expect, n) != 0) return NULL;
    tramp = (unsigned char*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return NULL;
    memcpy(tramp, target, n);
    tramp[n] = 0xFF;
    tramp[n + 1] = 0x25;
    tramp[n + 2] = tramp[n + 3] = tramp[n + 4] = tramp[n + 5] = 0;
    *(void**)(tramp + n + 6) = target + n;
    VirtualProtect(target, n, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xFF;
    target[1] = 0x25;
    target[2] = target[3] = target[4] = target[5] = 0;
    *(void**)(target + 6) = hook;
    for (i = 14; i < n; i++) target[i] = 0x90;
    VirtualProtect(target, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, n);
    return tramp;
}

static int __cdecl Hook_CreateChain(void* session, IUnknown* device, const int* desc, void** out) {
    typedef int(__cdecl* Fn)(void*, IUnknown*, const int*, void**);
    typedef HRESULT(STDMETHODCALLTYPE* QIFn)(void*, const GUID*, void**);
    if (session && g_ovr_session != session) {
        if (g_ovr_session && g_sess) InterlockedExchange(&g_reset_pending, 1);
        g_ovr_session = session;
        hud_log("ovr session %p", session);
    }
    if (device && com_vtable(device) && g_ovr_dev != device) {
        void* d11 = NULL;
        int is11 = SUCCEEDED(((QIFn)(*(void***)device)[0])(device, &kDev11, &d11)) && d11;
        if (d11) ((void(STDMETHODCALLTYPE*)(void*))(*(void***)d11)[2])(d11);
        ((void(STDMETHODCALLTYPE*)(void*))(*(void***)device)[1])(device);
        g_ovr_dev = device;
        hud_log("ovr device %p d3d11 %d", device, is11);
    }
    if (desc && readable(desc, 40) && g_create_logs < 4) {
        g_create_logs++;
        hud_log("realvr chain desc type %d fmt %d array %d %dx%d mips %d samples %d misc %u bind %u",
                desc[0], desc[1], desc[2], desc[3], desc[4], desc[5], desc[6], (unsigned)desc[8], (unsigned)desc[9]);
    }
    return ((Fn)g_create_tramp)(session, device, desc, out);
}

static int __cdecl Hook_EndTail(void* internal, void* session, uint64_t frameIndex, void** layers, unsigned count);
static int __cdecl Hook_EndFrame_Meta(void* session, uint64_t frameIndex, void* vsd, void** layers, unsigned count);

static void reset_bridge(const char* why) {
    int i;
    EnterCriticalSection(&g_lock);
    g_chain = NULL;
    g_sess = NULL;
    g_sess_arg = NULL;
    g_board_ok = 0;
    g_board_tex = NULL;
    g_hdr = 0;
    g_frames = 0;
    g_fail_n = 0;
    g_bridge_off = 0;
    g_ok_logged = 0;
    g_sess_fail_logged = 0;
    for (i = 0; i < CHAIN_MAX; i++) {
        if (g_crtv[i]) ID3D11RenderTargetView_Release(g_crtv[i]);
        g_crtv[i] = NULL;
        g_ctex[i] = NULL;
    }
    g_chain_len = 0;
    LeaveCriticalSection(&g_lock);
    hud_log("bridge reset (%s)", why);
}

static void patch_endframe(HMODULE lib) {
    unsigned char* fn = (unsigned char*)GetProcAddress(lib, "ovr_EndFrame");
    unsigned char* jmp;
    int32_t oldRel;
    intptr_t nr;
    DWORD old = 0;
    void* tail;
    if (!fn) { hud_log("ovr_EndFrame missing"); return; }
    /* Native Meta build: standard MSVC prologue, hooked at the function entry. */
    if (memcmp(fn, kMetaProlog, sizeof(kMetaProlog)) == 0) {
        void* tr;
        if (fn[0] == 0xFF && fn[1] == 0x25) return;   /* already hooked */
        tr = hook_prologue(fn, kMetaProlog, sizeof(kMetaProlog), (void*)Hook_EndFrame_Meta);
        if (tr) {
            g_end_meta_tramp = tr;
            g_end_mode = 2;
            hud_log("endframe prologue hooked (meta build) %p", (void*)fn);
        } else hud_log("endframe meta prologue hook failed");
        return;
    }
    /* Virtual Desktop build: the public EndFrame ends with a jmp (E9) at +38
       into the internal implementation. Redirect that jump. */
    jmp = fn + 38;
    if (jmp[0] != 0xE9) {
        hud_log("endframe shape unexpected %02X %02X %02X ... @38 %02X (bridge off)", fn[0], fn[1], fn[2], jmp[0]);
        return;
    }
    oldRel = *(int32_t*)(jmp + 1);
    tail = jmp + 5 + oldRel;
    if (tail == (void*)Hook_EndTail || (g_end_stub && tail == g_end_stub)) return;
    g_end_tail = tail;
    nr = (intptr_t)Hook_EndTail - (intptr_t)(jmp + 5);
    if (nr > 2147483647LL || nr < -2147483647LL) {
        unsigned char* stub = near_stub(jmp + 5, (void*)Hook_EndTail);
        if (!stub) { hud_log("endframe too far, no stub"); return; }
        g_end_stub = stub;
        nr = (intptr_t)stub - (intptr_t)(jmp + 5);
    }
    VirtualProtect(jmp, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t*)(jmp + 1) = (int32_t)nr;
    VirtualProtect(jmp, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), jmp, 5);
    g_end_jmp = jmp;
    g_end_rel = (int32_t)nr;
    g_end_mode = 1;
    hud_log("endframe tail hooked (%p -> tail %p)", (void*)fn, tail);
}

static void patch_create(HMODULE lib) {
    static const unsigned char vd[] = {0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xF1, 0x49, 0x8B, 0xF8};
    unsigned char* fn = (unsigned char*)GetProcAddress(lib, "ovr_CreateTextureSwapChainDX");
    void* tramp = NULL;
    if (!fn) return;
    if (fn[0] == 0xFF && fn[1] == 0x25) return;
    if (memcmp(fn, vd, sizeof(vd)) == 0)
        tramp = hook_prologue(fn, vd, sizeof(vd), (void*)Hook_CreateChain);
    else if (memcmp(fn, kMetaProlog, sizeof(kMetaProlog)) == 0)
        tramp = hook_prologue(fn, kMetaProlog, sizeof(kMetaProlog), (void*)Hook_CreateChain);
    else {
        hud_log("chain create prologue mismatch %02X %02X %02X %02X (device from board)", fn[0], fn[1], fn[2], fn[3]);
        return;
    }
    if (tramp) {
        g_create_tramp = tramp;
        hud_log("chain create hooked");
    } else hud_log("chain create hook failed");
}

static void check_libovr(void) {
    HMODULE lib = GetModuleHandleA("VirtualDesktop.LibOVRRT64_1.dll");
    if (!lib) lib = GetModuleHandleA("LibOVRRT64_1.dll");
    if (!lib) {
        if (g_lib) {
            g_lib = NULL;
            g_end_jmp = NULL;
            g_ovr_session = NULL;
            g_ovr_dev = NULL;
            reset_bridge("libovr unloaded");
        }
        return;
    }
    if (lib != g_lib) {
        char path[MAX_PATH];
        path[0] = 0;
        GetModuleFileNameA(lib, path, MAX_PATH);
        g_lib = lib;
        g_end_jmp = NULL;
        g_end_mode = 0;
        g_end_meta_tramp = NULL;
        g_ovr_session = NULL;
        g_ovr_dev = NULL;
        g_create_tramp = NULL;
        hud_log("libovr %p %s", (void*)lib, path);
        g_get_buf = (void*)GetProcAddress(lib, "ovr_GetTextureSwapChainBufferDX");
        g_get_index = (void*)GetProcAddress(lib, "ovr_GetTextureSwapChainCurrentIndex");
        g_get_len = (void*)GetProcAddress(lib, "ovr_GetTextureSwapChainLength");
        g_commit = (void*)GetProcAddress(lib, "ovr_CommitTextureSwapChain");
        g_create = (void*)GetProcAddress(lib, "ovr_CreateTextureSwapChainDX");
        hud_log("ovr exports buf %d index %d len %d commit %d create %d", g_get_buf != NULL, g_get_index != NULL, g_get_len != NULL, g_commit != NULL, g_create != NULL);
        reset_bridge("libovr loaded");
        patch_create(lib);
        patch_endframe(lib);
        return;
    }
    if (g_end_mode == 1 && g_end_jmp && readable(g_end_jmp, 5) && *(int32_t*)(g_end_jmp + 1) != g_end_rel) {
        hud_log("endframe patch lost (module reloaded in place), re-hooking");
        g_ovr_session = NULL;
        g_ovr_dev = NULL;
        g_create_tramp = NULL;
        reset_bridge("reload");
        patch_create(lib);
        patch_endframe(lib);
    }
}

/* ------------------------------------------------------------ ovr utils */

static int ovr_buf(void* sess, void* chain, int idx, ID3D11Texture2D** out) {
    typedef int(__cdecl* BufFn)(void*, void*, int, const GUID*, void**);
    void* buf = NULL;
    int rc;
    *out = NULL;
    if (!g_get_buf || !sess || !chain) return -1;
    rc = ((BufFn)g_get_buf)(sess, chain, idx, &kTex11, &buf);
    if (rc < 0) return rc;
    if (!buf || !com_vtable(buf)) return -2;
    *out = (ID3D11Texture2D*)buf;
    return 0;
}

static int ovr_index(void* sess, void* chain, int* idx) {
    typedef int(__cdecl* Fn)(void*, void*, int*);
    *idx = 0;
    if (!g_get_index) return -1;
    return ((Fn)g_get_index)(sess, chain, idx);
}

static int ovr_len(void* sess, void* chain) {
    typedef int(__cdecl* Fn)(void*, void*, int*);
    int n = 0;
    if (!g_get_len || ((Fn)g_get_len)(sess, chain, &n) < 0 || n <= 0) return 3;
    return n;
}

static int ovr_commit(void* sess, void* chain) {
    typedef int(__cdecl* Fn)(void*, void*);
    if (!g_commit) return -1;
    return ((Fn)g_commit)(sess, chain);
}

static int ovr_format(DXGI_FORMAT f) {
    switch (f) {
        case 28: return 4;   /* R8G8B8A8_UNORM */
        case 27: case 29: return 5;   /* R8G8B8A8 typeless / sRGB */
        case 87: return 6;   /* B8G8R8A8_UNORM */
        case 90: case 91: return 7;   /* B8G8R8A8 typeless / sRGB */
        case 10: return 10;  /* R16G16B16A16_FLOAT */
        default: return 5;
    }
}

/* ------------------------------------------------------------ D3D11 */

static const char* kShader =
    "Texture2D t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "  V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.p = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0); o.uv = uv; return o;\n"
    "}\n"
    "float4 ps(V i) : SV_Target { return t.Sample(s, i.uv); }\n"
    "float4 ps_lin(V i) : SV_Target {\n"
    "  float4 c = t.Sample(s, i.uv);\n"
    "  float3 lo = c.rgb / 12.92;\n"
    "  float3 hi = pow((c.rgb + 0.055) / 1.055, 2.4);\n"
    "  c.rgb = (c.rgb <= 0.04045) ? lo : hi;\n"
    "  return c;\n"
    "}\n"
    "float4 ps_cursor(V i) : SV_Target {\n"
    "  float2 p = i.uv;\n"
    "  float inside = (p.x > 0.10 && p.y > 0.10 && p.x + p.y < 0.86) ? 1.0 : 0.0;\n"
    "  float shape = (p.x + p.y < 1.02) ? 1.0 : 0.0;\n"
    "  if (shape < 0.5) discard;\n"
    "  return inside > 0.5 ? float4(1, 1, 1, 1) : float4(0, 0, 0, 1);\n"
    "}\n";

typedef HRESULT(WINAPI* CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3D10Blob**, ID3D10Blob**);

static ID3D10Blob* compile(CompileFn fn, const char* entry, const char* target) {
    ID3D10Blob* code = NULL;
    ID3D10Blob* err = NULL;
    HRESULT hr = fn(kShader, strlen(kShader), "hud", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        hud_log("shader %s failed 0x%08lX %s", entry, (unsigned long)hr, err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "");
        if (code) ID3D10Blob_Release(code);
        code = NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static void release_d3d11(void) {
    if (g_vs) ID3D11VertexShader_Release(g_vs);
    if (g_ps) ID3D11PixelShader_Release(g_ps);
    if (g_ps_lin) ID3D11PixelShader_Release(g_ps_lin);
    if (g_ps_cursor) ID3D11PixelShader_Release(g_ps_cursor);
    if (g_bs) ID3D11BlendState_Release(g_bs);
    if (g_ss) ID3D11SamplerState_Release(g_ss);
    if (g_rs) ID3D11RasterizerState_Release(g_rs);
    if (g_ctx) ID3D11DeviceContext_Release(g_ctx);
    if (g_dev1) ID3D11Device1_Release(g_dev1);
    if (g_dev) ID3D11Device_Release(g_dev);
    g_vs = NULL; g_ps = NULL; g_ps_lin = NULL; g_ps_cursor = NULL; g_bs = NULL; g_ss = NULL; g_rs = NULL; g_ctx = NULL; g_dev1 = NULL; g_dev = NULL;
}

static int init_d3d11(ID3D11Device* dev) {
    HMODULE lib;
    CompileFn fn;
    ID3D10Blob* b;
    D3D11_BLEND_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    void* multi = NULL;
    int i;
    if (g_dev == dev && g_vs && g_ps && g_ps_lin && g_ps_cursor && g_bs) return 1;
    release_d3d11();
    /* Slots already opened, and the sampling resources, belong to another device. */
    EnterCriticalSection(&g_lock);
    release_sampling();
    for (i = 0; i < RING_MAX; i++) {
        int k;
        Ring* r = &g_rings[i];
        if (!r->used) continue;
        for (k = 0; k < r->n; k++) {
            Slot* s = &r->s[k];
            if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
            if (s->tex11) ID3D11Texture2D_Release(s->tex11);
            s->srv = NULL;
            s->tex11 = NULL;
            s->open_fail = 0;
            s->sig_valid = 0;
        }
        r->newest = -1;
        r->passes = 0;
    }
    LeaveCriticalSection(&g_lock);
    g_dev = dev;
    ID3D11Device_AddRef(dev);
    if (FAILED(ID3D11Device_QueryInterface(dev, &kDev11_1, (void**)&g_dev1))) g_dev1 = NULL;
    ID3D11Device_GetImmediateContext(dev, &g_ctx);
    if (g_ctx && SUCCEEDED(ID3D11DeviceContext_QueryInterface(g_ctx, &kMulti11, &multi)) && multi) {
        typedef BOOL(STDMETHODCALLTYPE* SetFn)(void*, BOOL);
        BOOL prev = ((SetFn)(*(void***)multi)[5])(multi, TRUE);
        ((void(STDMETHODCALLTYPE*)(void*))(*(void***)multi)[2])(multi);
        hud_log("d3d11 multithread protection on (was %d)", prev);
    }
    lib = LoadLibraryA("d3dcompiler_47.dll");
    fn = lib ? (CompileFn)GetProcAddress(lib, "D3DCompile") : NULL;
    if (!fn) { hud_log("d3dcompiler_47 missing"); return 0; }
    b = compile(fn, "vs", "vs_5_0");
    if (!b) return 0;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &g_vs);
    ID3D10Blob_Release(b);
    b = compile(fn, "ps", "ps_5_0");
    if (!b) return 0;
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &g_ps);
    ID3D10Blob_Release(b);
    b = compile(fn, "ps_lin", "ps_5_0");
    if (!b) return 0;
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &g_ps_lin);
    ID3D10Blob_Release(b);
    b = compile(fn, "ps_cursor", "ps_5_0");
    if (!b) return 0;
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &g_ps_cursor);
    ID3D10Blob_Release(b);
    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(dev, &bd, &g_bs);
    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &sd, &g_ss);
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &g_rs);
    if (!g_vs || !g_ps || !g_ps_lin || !g_ps_cursor || !g_bs || !g_ss || !g_rs || !g_ctx) {
        hud_log("d3d11 objects failed vs %d ps %d lin %d cursor %d bs %d ss %d rs %d ctx %d dev1 %d", g_vs != NULL, g_ps != NULL, g_ps_lin != NULL, g_ps_cursor != NULL, g_bs != NULL, g_ss != NULL, g_rs != NULL, g_ctx != NULL, g_dev1 != NULL);
        return 0;
    }
    hud_log("d3d11 ready on device %p (dev1 %d)", (void*)dev, g_dev1 != NULL);
    return 1;
}

static DXGI_FORMAT unorm_of(DXGI_FORMAT f) {
    if (f == 27 || f == 29) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (f == 90 || f == 91) return DXGI_FORMAT_B8G8R8A8_UNORM;
    return f;
}

static int ensure_chain(void* session) {
    D3D11_TEXTURE2D_DESC bd;
    IUnknown* dev = g_ovr_dev;
    ID3D11Device* owner = NULL;
    ID3D11Device* got = NULL;
    int desc[10];
    void* chain = NULL;
    int rc, i, len;
    typedef int(__cdecl* CreateFn)(void*, IUnknown*, const void*, void**);
    (void)session;
    if (g_chain) return 1;
    if (!g_board_tex || !g_create || !g_sess) return 0;
    if (GetTickCount() - g_last_create_try < 3000 && g_last_create_try) return 0;
    g_last_create_try = GetTickCount();
    ID3D11Texture2D_GetDesc(g_board_tex, &bd);
    if (!dev) {
        ID3D11Texture2D_GetDevice(g_board_tex, &got);
        dev = (IUnknown*)got;
        hud_log("chain device taken from board texture %p", (void*)got);
    }
    if (!dev) return 0;
    memset(desc, 0, sizeof(desc));
    desc[0] = 0;                       /* ovrTexture_2D */
    desc[1] = ovr_format(bd.Format);
    desc[2] = 1;                       /* ArraySize */
    desc[3] = (int)bd.Width;
    desc[4] = (int)bd.Height;
    desc[5] = 1;                       /* MipLevels */
    desc[6] = 1;                       /* SampleCount */
    desc[7] = 0;                       /* StaticImage + padding */
    desc[8] = 0;                       /* MiscFlags */
    desc[9] = 1;                       /* ovrTextureBind_DX_RenderTarget */
    rc = ((CreateFn)g_create)(g_sess, dev, desc, &chain);
    if (got) ID3D11Device_Release(got);
    if (rc < 0 || !chain) {
        hud_log("hud chain create failed rc %d (%dx%d ovrfmt %d)", rc, desc[3], desc[4], desc[1]);
        return 0;
    }
    len = ovr_len(g_sess, chain);
    if (len > CHAIN_MAX) len = CHAIN_MAX;
    for (i = 0; i < len; i++) {
        rc = ovr_buf(g_sess, chain, i, &g_ctex[i]);
        if (rc || !g_ctex[i]) {
            hud_log("hud chain buffer %d failed rc %d", i, rc);
            return 0;
        }
    }
    ID3D11Texture2D_GetDevice(g_ctex[0], &owner);
    if (!owner || !init_d3d11(owner)) {
        if (owner) ID3D11Device_Release(owner);
        return 0;
    }
    ID3D11Device_Release(owner);
    {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(g_ctex[0], &td);
        for (i = 0; i < len; i++) {
            HRESULT hr;
            D3D11_RENDER_TARGET_VIEW_DESC rv;
            if (td.Format == 27 || td.Format == 90) {
                memset(&rv, 0, sizeof(rv));
                rv.Format = unorm_of(td.Format);
                rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)g_ctex[i], &rv, &g_crtv[i]);
                g_decode = 0;
            } else {
                hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)g_ctex[i], NULL, &g_crtv[i]);
                g_decode = (td.Format == 29 || td.Format == 91);
            }
            if (FAILED(hr) || !g_crtv[i]) {
                hud_log("hud rtv %d failed 0x%08lX fmt %u", i, (unsigned long)hr, (unsigned)td.Format);
                return 0;
            }
        }
        g_cw = td.Width;
        g_ch = td.Height;
        g_chain_len = len;
        g_chain = chain;
        hud_log("hud chain ready %ux%u fmt %u len %d decode %d (board %ux%u fmt %u)", td.Width, td.Height, (unsigned)td.Format, len, g_decode, bd.Width, bd.Height, (unsigned)bd.Format);
    }
    return 1;
}

static void open_rings_locked(void) {
    int i, k;
    if (!g_dev1) return;
    for (i = 0; i < RING_MAX; i++) {
        Ring* r = &g_rings[i];
        if (!r->used || r->dead) continue;
        for (k = 0; k < r->n; k++) {
            Slot* s = &r->s[k];
            HRESULT hr;
            D3D11_TEXTURE2D_DESC td;
            if (s->tex11 || s->open_fail || !s->dup) continue;
            hr = ID3D11Device1_OpenSharedResource1(g_dev1, s->dup, &kTex11, (void**)&s->tex11);
            if (FAILED(hr) || !s->tex11) {
                s->tex11 = NULL;
                s->open_fail = 1;
                if (g_open_logs < 4) { g_open_logs++; hud_log("slot d3d11 open failed 0x%08lX (ring %u)", (unsigned long)hr, r->id); }
                continue;
            }
            ID3D11Texture2D_GetDesc(s->tex11, &td);
            if (td.Format == 27 || td.Format == 90) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sv;
                memset(&sv, 0, sizeof(sv));
                sv.Format = unorm_of(td.Format);
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sv.Texture2D.MipLevels = 1;
                hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)s->tex11, &sv, &s->srv);
            } else hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)s->tex11, NULL, &s->srv);
            if (FAILED(hr) || !s->srv) {
                s->srv = NULL;
                s->open_fail = 1;
                if (g_open_logs < 4) { g_open_logs++; hud_log("slot srv failed 0x%08lX fmt %u", (unsigned long)hr, (unsigned)td.Format); }
                continue;
            }
            if (g_open_logs < 4) { g_open_logs++; hud_log("slot opened in d3d11 %ux%u fmt %u misc 0x%X (ring %u)", td.Width, td.Height, (unsigned)td.Format, (unsigned)td.MiscFlags, r->id); }
        }
    }
}

static int ring_ready(const Ring* r) {
    int k;
    if (!r->used || r->dead || r->n < 1 || r->w != g_act_w || r->h != g_act_h) return 0;
    for (k = 0; k < r->n; k++) if (!r->s[k].tex11 || !r->s[k].srv) return 0;
    return 1;
}

/* ------------------------------------------- current-slot selection */
/* Content signature of each slot: copy into a mip texture, GenerateMips, then
   read a mip of about 40x40 (each texel averages ~30x30 pixels; only a value
   that changes is needed). The read uses DO_NOT_WAIT a few frames later, so
   the GPU is never stalled. */

#ifndef DXGI_ERROR_WAS_STILL_DRAWING
#define DXGI_ERROR_WAS_STILL_DRAWING ((HRESULT)0x887A000AL)
#endif

static void release_sampling(void) {
    int i;
    for (i = 0; i < JOB_MAX; i++) {
        if (g_jobs[i].stage) ID3D11Texture2D_Release(g_jobs[i].stage);
        memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
    }
    for (i = 0; i < RING_MAX; i++) {
        g_rings[i].pend_id = 0;
        g_rings[i].pend_want = g_rings[i].pend_mask = 0;
    }
    if (g_scratch_srv) ID3D11ShaderResourceView_Release(g_scratch_srv);
    if (g_scratch) ID3D11Texture2D_Release(g_scratch);
    g_scratch_srv = NULL;
    g_scratch = NULL;
    g_scratch_w = g_scratch_h = 0;
}

static int ensure_scratch(UINT w, UINT h, DXGI_FORMAT fmt) {
    D3D11_TEXTURE2D_DESC td;
    DXGI_FORMAT sf = bgra(fmt) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT L;
    HRESULT hr;
    if (g_scratch && g_scratch_w == w && g_scratch_h == h && g_scratch_fmt == sf) return 1;
    release_sampling();
    memset(&td, 0, sizeof(td));
    td.Width = w;
    td.Height = h;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = sf;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    hr = ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_scratch);
    if (FAILED(hr) || !g_scratch) {
        g_scratch = NULL;
        hud_log("sampling scratch create failed 0x%08lX (%ux%u fmt %u)", (unsigned long)hr, w, h, (unsigned)sf);
        return 0;
    }
    hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)g_scratch, NULL, &g_scratch_srv);
    if (FAILED(hr) || !g_scratch_srv) {
        g_scratch_srv = NULL;
        ID3D11Texture2D_Release(g_scratch);
        g_scratch = NULL;
        hud_log("sampling scratch srv failed 0x%08lX", (unsigned long)hr);
        return 0;
    }
    ID3D11Texture2D_GetDesc(g_scratch, &td);
    for (L = 0; L + 1 < td.MipLevels && ((w >> L) > 48 || (h >> L) > 48); L++) {}
    g_scratch_w = w;
    g_scratch_h = h;
    g_scratch_fmt = sf;
    g_scratch_mips = td.MipLevels;
    g_scratch_mip = L;
    hud_log("sampling ready %ux%u fmt %u mips %u, signature mip %u (%ux%u)", w, h, (unsigned)sf, td.MipLevels, L,
            (w >> L) ? (w >> L) : 1, (h >> L) ? (h >> L) : 1);
    return 1;
}

static int ensure_stage(Job* j, UINT sw, UINT sh, DXGI_FORMAT f) {
    D3D11_TEXTURE2D_DESC td;
    if (j->stage && j->sw == sw && j->sh == sh && j->sfmt == f) return 1;
    if (j->stage) ID3D11Texture2D_Release(j->stage);
    j->stage = NULL;
    memset(&td, 0, sizeof(td));
    td.Width = sw;
    td.Height = sh;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &j->stage)) || !j->stage) {
        j->stage = NULL;
        return 0;
    }
    j->sw = sw;
    j->sh = sh;
    j->sfmt = f;
    return 1;
}

static void eval_pass_locked(Ring* r) {
    int k, first = 0, changed = 0, prev, best, d;
    for (k = 0; k < r->n; k++) {
        if (!r->s[k].sig_valid) first = 1;
        else if (r->s[k].sig != r->pend_sig[k]) changed |= 1 << k;
    }
    for (k = 0; k < r->n; k++) {
        r->s[k].sig = r->pend_sig[k];
        r->s[k].alpha = r->pend_alpha[k];
        r->s[k].sig_valid = 1;
    }
    r->passes++;
    if (first) {
        /* First sample: no history yet, so take the slot with the most content. */
        best = 0;
        for (k = 1; k < r->n; k++) if (r->s[k].alpha >= r->s[best].alpha) best = k;
        if (r->newest < 0) r->newest = best;
    } else if (changed) {
        /* A changed slot is the one just written. If several changed in the same
           pass, the last one in ring-rotation order wins. */
        r->last_change = GetTickCount();
        prev = r->newest < 0 ? 0 : r->newest;
        best = -1;
        for (d = 1; d <= r->n; d++) {
            k = (prev + d) % r->n;
            if (changed & (1 << k)) best = k;
        }
        if (best >= 0) r->newest = best;
    }
    r->pend_id = 0;
    r->pend_want = r->pend_mask = 0;
}

static void harvest_jobs_locked(void) {
    int i;
    for (i = 0; i < JOB_MAX; i++) {
        Job* j = &g_jobs[i];
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr;
        uint64_t h = 1469598103934665603ull;
        unsigned a = 0, x, y;
        Ring* r;
        if (!j->busy || g_frames - j->issued < 2) continue;
        hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)j->stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING && g_frames - j->issued < 30) continue;
        j->busy = 0;
        r = &g_rings[j->ring];
        if (FAILED(hr)) {
            if (r->used && r->id == j->ring_id && r->pend_id == j->pass_id) {
                r->pend_id = 0;
                r->pend_want = r->pend_mask = 0;
            }
            if (++g_sample_errors > 20 && g_sample_ok) {
                g_sample_ok = 0;
                hud_log("sampling unreliable (last hr 0x%08lX): fallback to drawing every slot", (unsigned long)hr);
            }
            continue;
        }
        for (y = 0; y < j->sh; y++) {
            const unsigned char* row = (const unsigned char*)m.pData + (size_t)y * m.RowPitch;
            for (x = 0; x < j->sw * 4; x++) {
                h ^= row[x];
                h *= 1099511628211ull;
            }
            for (x = 0; x < j->sw; x++) a += row[x * 4 + 3];
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)j->stage, 0);
        if (!r->used || r->id != j->ring_id || r->pend_id != j->pass_id) continue;
        r->pend_sig[j->slot] = h;
        r->pend_alpha[j->slot] = a;
        r->pend_mask |= 1 << j->slot;
        if (r->pend_mask == r->pend_want) eval_pass_locked(r);
    }
}

static void issue_passes_locked(void) {
    int started = 0, tries, i, k, nfree;
    UINT sw, sh;
    for (tries = 0; tries < RING_MAX && started < g_sample_rings; tries++) {
        Ring* r;
        int jobs[RING_SLOTS];
        g_ring_rr = (g_ring_rr + 1) % RING_MAX;
        r = &g_rings[g_ring_rr];
        if (!ring_ready(r) || r->pend_id) continue;
        if (!ensure_scratch(r->w, r->h, r->fmt)) {
            g_sample_ok = 0;
            hud_log("sampling disabled: fallback to drawing every slot");
            return;
        }
        sw = (g_scratch_w >> g_scratch_mip) ? (g_scratch_w >> g_scratch_mip) : 1;
        sh = (g_scratch_h >> g_scratch_mip) ? (g_scratch_h >> g_scratch_mip) : 1;
        nfree = 0;
        for (i = 0; i < JOB_MAX && nfree < r->n; i++) if (!g_jobs[i].busy) jobs[nfree++] = i;
        if (nfree < r->n) return;
        for (k = 0; k < r->n; k++) {
            if (!ensure_stage(&g_jobs[jobs[k]], sw, sh, g_scratch_fmt)) {
                g_sample_ok = 0;
                hud_log("sampling staging failed: fallback to drawing every slot");
                return;
            }
        }
        r->pend_id = ++g_pass_seq;
        r->pend_want = r->pend_mask = 0;
        for (k = 0; k < r->n; k++) {
            Job* j = &g_jobs[jobs[k]];
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource*)g_scratch, 0, 0, 0, 0, (ID3D11Resource*)r->s[k].tex11, 0, NULL);
            ID3D11DeviceContext_GenerateMips(g_ctx, g_scratch_srv);
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource*)j->stage, 0, 0, 0, 0, (ID3D11Resource*)g_scratch, g_scratch_mip, NULL);
            j->busy = 1;
            j->ring = (int)(r - g_rings);
            j->ring_id = r->id;
            j->pass_id = r->pend_id;
            j->slot = k;
            j->issued = g_frames;
            r->pend_want |= 1 << k;
        }
        started++;
    }
}

/* Open77 releases the rings of surfaces it destroys or resizes. When only our
   reference remains, nobody is displaying that ring anymore. */
static void check_released_locked(void) {
    int i, k;
    for (i = 0; i < RING_MAX; i++) {
        Ring* r = &g_rings[i];
        int alive = 0;
        if (!r->used || r->dead || r->n < 1) continue;
        for (k = 0; k < r->n; k++) {
            ULONG c;
            if (!r->s[k].res) continue;
            c = ID3D12Resource_AddRef(r->s[k].res);
            ID3D12Resource_Release(r->s[k].res);
            if (c > 2) alive = 1;
        }
        if (!alive) {
            hud_log("ring %u released by Open77 (%ux%u), freed", r->id, r->w, r->h);
            free_ring_locked(r);
        }
    }
}

static void log_rings_locked(void) {
    int i, n = 0;
    DWORD now = GetTickCount();
    for (i = 0; i < RING_MAX && n < 20; i++) {
        const Ring* r = &g_rings[i];
        if (!r->used || r->dead) continue;
        n++;
        hud_log("  ring %u [%s] %ux%u tid %u slots %d newest %d passes %u static %lus alpha %u/%u/%u%s", r->id, r->name[0] ? r->name : "?",
                r->w, r->h, r->tid, r->n, r->newest, r->passes, (unsigned long)((now - r->last_change) / 1000),
                r->s[0].alpha, r->s[1].alpha, r->s[2].alpha, ring_ready(r) ? "" : " (not ready)");
    }
}

static int name_in_list(const char* list, const char* name) {
    size_t nl = strlen(name);
    const char* p = list;
    while (p && *p) {
        const char* e;
        size_t len;
        while (*p == ' ' || *p == ',') p++;
        e = p;
        while (*e && *e != ',') e++;
        len = (size_t)(e - p);
        while (len && p[len - 1] == ' ') len--;
        if (len == nl && len && strncmp(p, name, len) == 0) return 1;
        p = e;
    }
    return 0;
}

static int ring_hidden_locked(const Ring* r) {
    int i;
    if (g_menu_hidden && r->id == g_menu_ring) return 1;
    if (!r->name[0]) return 0;
    if (g_hide_list[0] && name_in_list(g_hide_list, r->name)) return 1;
    for (i = 0; i < g_vis_n; i++) if (strcmp(g_vis[i].name, r->name) == 0) return g_vis[i].hidden;
    return 0;
}

static void set_vis(const char* name, int hidden) {
    int i, prev = -1;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_vis_n; i++) if (strcmp(g_vis[i].name, name) == 0) break;
    if (i == g_vis_n) {
        if (g_vis_n >= VIS_MAX) { LeaveCriticalSection(&g_lock); return; }
        memset(&g_vis[i], 0, sizeof(g_vis[i]));
        strncpy(g_vis[i].name, name, sizeof(g_vis[i].name) - 1);
        g_vis_n++;
    } else prev = g_vis[i].hidden;
    g_vis[i].hidden = hidden;
    LeaveCriticalSection(&g_lock);
    if (prev != hidden) hud_log("surface [%s] %s by Open77", name, hidden ? "hidden" : "shown");
}

/* The import log line ("[ tid] ... surface N [name] ... ring revision R imported")
   is written by the same thread that opened the handles, after the third slot.
   One thread imports rings one at a time, so the name goes to the oldest ring
   that does not have a name yet. */
static void name_ring(const char* name, DWORD tid, DWORD ms) {
    int i, best = -1;
    DWORD dt = 0;
    unsigned id = 0;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < RING_MAX; i++) {
        Ring* r = &g_rings[i];
        if (!r->used || r->dead || r->tid != tid || r->name[0]) continue;
        if (r->t_wall > ms + 100 || ms > r->t_wall + 3000) continue;
        if (best < 0 || r->id < g_rings[best].id) best = i;
    }
    if (best >= 0) {
        strncpy(g_rings[best].name, name, sizeof(g_rings[best].name) - 1);
        id = g_rings[best].id;
        dt = ms > g_rings[best].t_wall ? ms - g_rings[best].t_wall : 0;
        if (!g_menu_ring && strcmp(name, "freeroam") == 0) g_menu_ring = id;
    }
    LeaveCriticalSection(&g_lock);
    if (best >= 0 && id == g_menu_ring) hud_log("ring %u is the freeroam menu", id);
    if (best >= 0) hud_log("ring %u is [%s] (log +%lu ms)", id, name, (unsigned long)dt);
    else hud_log("import of [%s] tid %lu not matched to a ring", name, (unsigned long)tid);
}

static void save_state(Saved* s) {
    UINT n = 0;
    memset(s, 0, sizeof(*s));
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &s->rtv, &s->dsv);
    s->nvp = VP_MAX;
    ID3D11DeviceContext_RSGetViewports(g_ctx, &s->nvp, s->vp);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &s->bs, s->bf, &s->mask);
    ID3D11DeviceContext_RSGetState(g_ctx, &s->rs);
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &s->il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &s->topo);
    ID3D11DeviceContext_VSGetShader(g_ctx, &s->vs, NULL, &n);
    n = 0;
    ID3D11DeviceContext_PSGetShader(g_ctx, &s->ps, NULL, &n);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 1, &s->srv);
    ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 1, &s->ss);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &s->dss, &s->sref);
}

static void restore_state(Saved* s) {
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &s->rtv, s->dsv);
    ID3D11DeviceContext_RSSetViewports(g_ctx, s->nvp, s->vp);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, s->bs, s->bf, s->mask);
    ID3D11DeviceContext_RSSetState(g_ctx, s->rs);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, s->il);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, s->topo);
    ID3D11DeviceContext_VSSetShader(g_ctx, s->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, s->ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &s->srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &s->ss);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, s->dss, s->sref);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->dsv) ID3D11DepthStencilView_Release(s->dsv);
    if (s->bs) ID3D11BlendState_Release(s->bs);
    if (s->rs) ID3D11RasterizerState_Release(s->rs);
    if (s->il) ID3D11InputLayout_Release(s->il);
    if (s->vs) ID3D11VertexShader_Release(s->vs);
    if (s->ps) ID3D11PixelShader_Release(s->ps);
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->ss) ID3D11SamplerState_Release(s->ss);
    if (s->dss) ID3D11DepthStencilState_Release(s->dss);
}

/* These pages are driven by the Windows cursor, which CEF does not bake into
   the captured frame. The arrow is drawn on top, in the same place. */
static int wants_cursor(const char* name) {
    return strcmp(name, "open77_pause") == 0 || strcmp(name, "open77_admin") == 0
        || strcmp(name, "open77_wardrobe_ui") == 0 || strcmp(name, "open77_contextmenu") == 0;
}

static int cursor_on_page(const int* board, D3D11_VIEWPORT* out) {
    POINT pt;
    RECT rc;
    HWND hwnd;
    float u, v, size;
    if (!GetCursorPos(&pt)) return 0;
    hwnd = GetForegroundWindow();
    if (!hwnd || !ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &rc)) return 0;
    if (rc.right <= 0 || rc.bottom <= 0) return 0;
    if (pt.x < 0 || pt.y < 0 || pt.x >= rc.right || pt.y >= rc.bottom) return 0;
    u = (float)pt.x / (float)rc.right;
    v = (float)pt.y / (float)rc.bottom;
    size = (float)board[3] * 0.045f;
    if (size < 18.f) size = 18.f;
    out->TopLeftX = (FLOAT)board[0] + u * (FLOAT)board[2];
    out->TopLeftY = (FLOAT)board[1] + v * (FLOAT)board[3];
    out->Width = size;
    out->Height = size;
    out->MinDepth = 0.f;
    out->MaxDepth = 1.f;
    return 1;
}

/* Draw the pages into the current swapchain buffer and commit it. */
static int render_hud(const int* board_vp) {
    Saved s;
    D3D11_VIEWPORT vp;
    FLOAT clear[4] = {0, 0, 0, 0};
    int idx = 0, i, o, drawn = 0, avail = 0, rc, tint, sel, cursor = 0;
    int order[RING_MAX];
    ID3D11RenderTargetView* rtv;
    if (ovr_index(g_sess, g_chain, &idx) < 0 || idx < 0 || idx >= g_chain_len) idx = 0;
    rtv = g_crtv[idx];
    if (!rtv) return 0;
    EnterCriticalSection(&g_lock);
    open_rings_locked();
    sel = g_select_newest && g_sample_ok;
    if (sel) harvest_jobs_locked();
    if ((g_frames % 45) == 0) check_released_locked();
    for (i = 0; i < RING_MAX; i++) if (ring_ready(&g_rings[i])) order[avail++] = i;
    for (i = 1; i < avail; i++) {
        int v = order[i], j = i - 1;
        while (j >= 0 && g_rings[order[j]].id > g_rings[v].id) { order[j + 1] = order[j]; j--; }
        order[j + 1] = v;
    }
    tint = (g_tint == 1) || (g_tint == -1 && avail == 0);
    if (tint) { clear[1] = 0.6f * 0.35f; clear[2] = 0.35f; clear[3] = 0.35f; }
    save_state(&s);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, rtv, clear);
    if (avail > 0) {
        vp.TopLeftX = (FLOAT)board_vp[0];
        vp.TopLeftY = (FLOAT)board_vp[1];
        vp.Width = (FLOAT)board_vp[2];
        vp.Height = (FLOAT)board_vp[3];
        vp.MinDepth = 0.f;
        vp.MaxDepth = 1.f;
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(g_ctx, g_rs);
        ID3D11DeviceContext_OMSetBlendState(g_ctx, g_bs, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, NULL, 0);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_decode ? g_ps_lin : g_ps, NULL, 0);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_ss);
        for (o = 0; o < avail; o++) {
            Ring* r = &g_rings[order[o]];
            int k, k0 = 0, k1 = r->n;
            if (ring_hidden_locked(r)) continue;
            if (sel) {
                if (r->newest < 0 || r->newest >= r->n) continue;
                if (g_static_hide_ms > 0 && r->passes >= 2 && GetTickCount() - r->last_change > (DWORD)g_static_hide_ms) continue;
                k0 = r->newest;
                k1 = r->newest + 1;
            }
            for (k = k0; k < k1; k++) {
                ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &r->s[k].srv);
                ID3D11DeviceContext_Draw(g_ctx, 3, 0);
                drawn++;
                if (wants_cursor(r->name) && (!r->s[k].sig_valid || r->s[k].alpha > 64)) cursor = 1;
            }
        }
        if (cursor && g_ps_cursor) {
            D3D11_VIEWPORT cv;
            if (cursor_on_page(board_vp, &cv)) {
                ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &cv);
                ID3D11DeviceContext_PSSetShader(g_ctx, g_ps_cursor, NULL, 0);
                ID3D11DeviceContext_Draw(g_ctx, 3, 0);
            }
        }
        {
            ID3D11ShaderResourceView* none = NULL;
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &none);
        }
    }
    restore_state(&s);
    if (sel) issue_passes_locked();
    if ((g_frames % 1800) == 0 && avail) log_rings_locked();
    LeaveCriticalSection(&g_lock);
    rc = ovr_commit(g_sess, g_chain);
    if (rc < 0) {
        if (g_fail_n < 3) hud_log("hud chain commit failed rc %d", rc);
        return 0;
    }
    if (tint && !g_tint_logged) {
        g_tint_logged = 1;
        hud_log("tint test active (no Open77 pages yet): the headset board should look light blue");
    }
    if ((g_frames % 600) == 0)
        hud_log("hud frame %u: %d rings ready, %d slots drawn (%s), %ux%u", g_frames, avail, drawn,
                sel ? "newest slot" : "all slots", g_act_w, g_act_h);
    if (drawn && !tint && g_tint_logged) g_tint_logged = 0;
    return 1;
}

/* --------------------------------------------------------- layer parsing */

static int plausible_rect(const int* r) {
    return r[0] >= 0 && r[1] >= 0 && r[2] > 0 && r[3] > 0 && r[0] < 16384 && r[1] < 16384 && r[2] <= 16384 && r[3] <= 16384;
}

static int detect_hdr(unsigned char* quad) {
    int cands[2] = {136, 8};
    int i;
    for (i = 0; i < 2; i++) {
        int h = cands[i];
        void* chain;
        if (!readable(quad, h + 60)) continue;
        chain = *(void**)(quad + h);
        if (!chain || !readable(chain, 8)) continue;
        if (!plausible_rect((const int*)(quad + h + 8))) continue;
        return h;
    }
    return 0;
}

static void dump_layer(unsigned i, unsigned char* L) {
    int off;
    for (off = 0; off < 288; off += 8) {
        uint64_t q;
        if (!readable(L + off, 8)) break;
        q = *(uint64_t*)(L + off);
        if (q) hud_log("  layer %u +%d %016llx", i, off, (unsigned long long)q);
    }
}

static int bridge_frame(void* internal, void* session, void** layers, unsigned count) {
    unsigned i;
    unsigned char* board = NULL;
    unsigned char* eye = NULL;
    (void)internal;
    if (InterlockedExchange(&g_reset_pending, 0)) reset_bridge("new ovr session");
    if (g_sess && g_sess_arg && session != g_sess_arg) {
        reset_bridge("endframe session changed");
        return 0;
    }
    for (i = 0; i < count; i++) {
        int ty;
        if (!readable(layers[i], 8)) continue;
        ty = *(int*)layers[i];
        if (ty == 3 && !board) board = (unsigned char*)layers[i];
        else if (ty == 1 && !eye) eye = (unsigned char*)layers[i];
    }
    if (g_frames < (unsigned)g_dump) {
        hud_log("endframe f%u layers %u session %p internal %p ovr_session %p", g_frames, count, session, internal, g_ovr_session);
        for (i = 0; i < count; i++) {
            if (!readable(layers[i], 8)) continue;
            hud_log(" layer %u type %d flags %u", i, *(int*)layers[i], *(unsigned*)((unsigned char*)layers[i] + 4));
            dump_layer(i, (unsigned char*)layers[i]);
        }
    }
    if (board) {
        if (!g_hdr) {
            g_hdr = detect_hdr(board);
            if (g_hdr && !g_layout_logged) {
                const int* r = (const int*)(board + g_hdr + 8);
                const float* q = (const float*)(board + g_hdr + 24);
                const float* pos = (const float*)(board + g_hdr + 40);
                const float* sz = (const float*)(board + g_hdr + 52);
                g_layout_logged = 1;
                hud_log("layout header %d: board chain %p viewport %d,%d %dx%d pose q(%.3f %.3f %.3f %.3f) pos(%.3f %.3f %.3f) size %.3fx%.3f",
                        g_hdr, *(void**)(board + g_hdr), r[0], r[1], r[2], r[3], q[0], q[1], q[2], q[3], pos[0], pos[1], pos[2], sz[0], sz[1]);
                if (eye && readable(eye, g_hdr + 144)) {
                    const float* fov = (const float*)(eye + g_hdr + 48);
                    hud_log("eye layer chains %p %p fov U %.3f D %.3f L %.3f R %.3f (atteso ~1.171/1.171/1.072/1.072)",
                            *(void**)(eye + g_hdr), *(void**)(eye + g_hdr + 8), fov[0], fov[1], fov[2], fov[3]);
                }
            } else if (!g_hdr && g_frames == 5) hud_log("layout not recognized: neither +136 nor +8 hold a swapchain");
        }
        if (g_hdr && readable(board, g_hdr + 60)) {
            memcpy(g_board, board, g_hdr + 60);
            g_board_ok = 1;
        }
    }
    if (!g_board_ok || !g_hdr) return 0;
    if (!g_sess) {
        void* cands[2];
        int nc = 0, c;
        if (g_ovr_session) cands[nc++] = g_ovr_session;
        if (session && session != g_ovr_session) cands[nc++] = session;
        if (!g_ovr_session && g_frames < 3) return 0;
        for (c = 0; c < nc && !g_sess; c++) {
            ID3D11Texture2D* tex = NULL;
            int rc = ovr_buf(cands[c], *(void**)(g_board + g_hdr), 0, &tex);
            if (rc == 0 && tex) {
                D3D11_TEXTURE2D_DESC td;
                ID3D11Texture2D_GetDesc(tex, &td);
                g_sess = cands[c];
                g_sess_arg = session;
                g_board_tex = tex;
                hud_log("board buffer via session %p: %ux%u fmt %u bind 0x%X misc 0x%X", cands[c], td.Width, td.Height, (unsigned)td.Format, (unsigned)td.BindFlags, (unsigned)td.MiscFlags);
            } else if (!g_sess_fail_logged || (g_frames % 300) == 0) {
                g_sess_fail_logged = 1;
                hud_log("board buffer rc %d via session %p", rc, cands[c]);
            }
        }
        if (!g_sess) return 0;
    }
    if (!ensure_chain(session)) return 0;
    if (!render_hud((const int*)(g_board + g_hdr + 8))) return 0;
    memcpy(g_quad_out, g_board, g_hdr + 60);
    *(void**)(g_quad_out + g_hdr) = g_chain;
    return 1;
}

static int __cdecl Hook_EndTail(void* internal, void* session, uint64_t frameIndex, void** layers, unsigned count) {
    typedef int(__cdecl* Fn)(void*, void*, uint64_t, void**, unsigned);
    Fn tail = (Fn)g_end_tail;
    void* mine[LAYER_MAX + 1];
    int rc, added = 0;
    if (!g_bridge_off && count >= 1 && count <= LAYER_MAX && readable(layers, sizeof(void*) * count)) {
        added = bridge_frame(internal, session, layers, count);
        g_frames++;
    }
    if (!added) return tail(internal, session, frameIndex, layers, count);
    memcpy(mine, layers, sizeof(void*) * count);
    mine[count] = g_quad_out;
    rc = tail(internal, session, frameIndex, mine, count + 1);
    if (rc < 0) {
        g_fail_n++;
        hud_log("endframe with hud layer failed rc %d (%d)", rc, g_fail_n);
        if (g_fail_n >= 3) {
            g_bridge_off = 1;
            hud_log("bridge disabled after 3 failures");
        }
    } else if (!g_ok_logged) {
        g_ok_logged = 1;
        hud_log("endframe accepted %u layers rc %d: HUD layer live in headset", count + 1, rc);
    }
    return rc;
}

/* Native Meta build: hook on the prologue of the public ovr_EndFrame.
   Win64 signature: (session, frameIndex, viewScaleDesc, layerPtrList, layerCount). */
static int __cdecl Hook_EndFrame_Meta(void* session, uint64_t frameIndex, void* vsd, void** layers, unsigned count) {
    typedef int(__cdecl* Fn)(void*, uint64_t, void*, void**, unsigned);
    Fn orig = (Fn)g_end_meta_tramp;
    void* mine[LAYER_MAX + 1];
    int rc, added = 0;
    if (!g_bridge_off && layers && count >= 1 && count <= LAYER_MAX && readable(layers, sizeof(void*) * count)) {
        added = bridge_frame(NULL, session, layers, count);
        g_frames++;
    }
    if (!added) return orig(session, frameIndex, vsd, layers, count);
    memcpy(mine, layers, sizeof(void*) * count);
    mine[count] = g_quad_out;
    rc = orig(session, frameIndex, vsd, mine, count + 1);
    if (rc < 0) {
        g_fail_n++;
        hud_log("endframe(meta) with hud layer failed rc %d (%d)", rc, g_fail_n);
        if (g_fail_n >= 3) {
            g_bridge_off = 1;
            hud_log("bridge disabled after 3 failures");
        }
    } else if (!g_ok_logged) {
        g_ok_logged = 1;
        hud_log("endframe(meta) accepted %u layers rc %d: HUD layer live in headset", count + 1, rc);
    }
    return rc;
}

/* ---------------------------------------------------- Open77 log tail */

static void process_log_line(const char* line) {
    const char* p;
    unsigned hh, mm, ss, ff;
    DWORD ms, tid = 0;
    int k;
    if (line[0] != '[') return;
    p = strchr(line, ' ');
    if (!p || sscanf(p + 1, "%2u:%2u:%2u.%3u", &hh, &mm, &ss, &ff) != 4) return;
    ms = ((hh * 60u + mm) * 60u + ss) * 1000u + ff;
    p = line;
    for (k = 0; k < 2 && p; k++) {
        p = strchr(p, ']');
        if (p) p++;
    }
    if (p && (p = strchr(p, '[')) != NULL) tid = (DWORD)strtoul(p + 1, NULL, 10);
    if ((p = strstr(line, "Open77 WebUI surface ")) != NULL) {
        const char* nb = strchr(p, '[');
        const char* ne = nb ? strchr(nb, ']') : NULL;
        if (nb && ne && ne - nb > 1 && ne - nb < 40 && strstr(ne, " ring revision ") && strstr(ne, " imported")) {
            char name[40];
            memcpy(name, nb + 1, (size_t)(ne - nb - 1));
            name[ne - nb - 1] = 0;
            if (tid) name_ring(name, tid, ms);
        }
        return;
    }
    if ((p = strstr(line, "[resource:")) != NULL) {
        const char* ne = strchr(p + 10, ']');
        char name[40];
        size_t len;
        if (!ne) return;
        len = (size_t)(ne - (p + 10));
        if (len == 0 || len >= sizeof(name)) return;
        memcpy(name, p + 10, len);
        name[len] = 0;
        if (strstr(ne, "WebUI hide applied") || strstr(ne, "WebUI hide re-applied")) set_vis(name, 1);
        else if (strstr(ne, "] show requested")) set_vis(name, 0);
        if (strcmp(name, "freeroam") == 0) {
            int hide = -1;
            if (strstr(ne, "menu closed")) hide = 1;
            else if (strstr(ne, "menu open")) hide = 0;
            if (hide >= 0) {
                EnterCriticalSection(&g_lock);
                g_menu_hidden = hide;
                LeaveCriticalSection(&g_lock);
                hud_log("freeroam menu %s (ring %u)", hide ? "hidden" : "shown", g_menu_ring);
            }
        }
    }
}

static int find_newest_log(wchar_t* out) {
    WIN32_FIND_DATAW fd;
    wchar_t pat[MAX_PATH];
    HANDLE h;
    FILETIME best;
    int found = 0;
    best.dwLowDateTime = best.dwHighDateTime = 0;
    _snwprintf(pat, MAX_PATH, L"%s\\open77-2*.log", g_logs_dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
            best = fd.ftLastWriteTime;
            _snwprintf(out, MAX_PATH, L"%s\\%s", g_logs_dir, fd.cFileName);
            found = 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static void tail_step(void) {
    static char buf[65537];
    HANDLE f;
    DWORD got, i, start, last;
    LARGE_INTEGER pos;
    if (!g_follow_log || !g_logs_dir[0]) return;
    if (!g_tail_path[0] || GetTickCount() - g_tail_scan > 10000) {
        wchar_t cand[MAX_PATH];
        g_tail_scan = GetTickCount();
        if (find_newest_log(cand) && wcscmp(cand, g_tail_path) != 0) {
            char narrow[MAX_PATH];
            wcscpy(g_tail_path, cand);
            g_tail_off = 0;
            WideCharToMultiByte(CP_UTF8, 0, wcsrchr(g_tail_path, L'\\') ? wcsrchr(g_tail_path, L'\\') + 1 : g_tail_path, -1, narrow, MAX_PATH, NULL, NULL);
            hud_log("following Open77 log %s", narrow);
        }
    }
    if (!g_tail_path[0]) return;
    f = CreateFileW(g_tail_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    for (;;) {
        pos.QuadPart = g_tail_off;
        if (!SetFilePointerEx(f, pos, NULL, FILE_BEGIN)) break;
        if (!ReadFile(f, buf, 65536, &got, NULL) || got == 0) break;
        start = 0;
        last = 0;
        for (i = 0; i < got; i++) {
            if (buf[i] != '\n') continue;
            buf[i] = 0;
            if (i > start && buf[i - 1] == '\r') buf[i - 1] = 0;
            process_log_line(buf + start);
            start = i + 1;
            last = i + 1;
        }
        if (!last) {
            if (got == 65536) {
                g_tail_off += got;
                continue;
            }
            break;
        }
        g_tail_off += last;
        if (got < 65536) break;
    }
    CloseHandle(f);
}

/* ------------------------------------------------------------- worker */

static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
    hud_log("worker started");
    while (!g_stop) {
        if (!g_d3d12_hooked && GetModuleHandleA("d3d12.dll")) install_d3d12();
        check_libovr();
        tail_step();
        Sleep(500);
    }
    return 0;
}

static BOOL CALLBACK start_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    load_ini();
    {
        wchar_t* cut;
        wcsncpy(g_logs_dir, g_dir, MAX_PATH - 1);
        cut = wcsrchr(g_logs_dir, L'\\');
        if (cut) {
            *cut = 0;
            cut = wcsrchr(g_logs_dir, L'\\');
        }
        if (cut) {
            *cut = 0;
            wcsncat(g_logs_dir, L"\\logs", MAX_PATH - wcslen(g_logs_dir) - 1);
        } else g_logs_dir[0] = 0;
    }
    hud_log("Open77 VR HUD 4.6.0 bridge (tint %d dump %d select %s formats %s sampleRings %d staticHideMs %d followLog %d hide [%s])",
            g_tint, g_dump, g_select_newest ? "newest" : "all", g_only_bgra ? "bgra" : "all", g_sample_rings, g_static_hide_ms,
            g_follow_log, g_hide_list);
    if (g_enabled) CreateThread(NULL, 0, worker, NULL, 0, NULL);
    else hud_log("disabled");
    return TRUE;
}

__declspec(dllexport) unsigned int Supports(void) { return 0; }
__declspec(dllexport) void Query(PluginInfo* info) {
    memset(info, 0, sizeof(*info));
    info->sdk.minor = 5;
    info->sdk.patch = 1;
    info->name = L"Open77 VR HUD";
    info->author = L"local";
    info->version.major = 4;
    info->version.minor = 6;
    info->version.patch = 0;
    info->runtime = -1;
}
__declspec(dllexport) int Main(void* handle, int reason, const void* sdk) {
    (void)handle; (void)sdk;
    if (reason == 0) InitOnceExecuteOnce(&g_once, start_once, NULL, NULL);
    else InterlockedExchange(&g_stop, 1);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t* slash;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameW(inst, g_dir, MAX_PATH);
        slash = wcsrchr(g_dir, L'\\');
        if (slash) *slash = 0;
        _snwprintf(g_log_path, MAX_PATH, L"%s\\Open77VrHud.log", g_dir);
        _snwprintf(g_ini_path, MAX_PATH, L"%s\\Open77VrHud.ini", g_dir);
    }
    return TRUE;
}
