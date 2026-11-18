/* gl_gpu.c -- GPU_PROFILE: the kind of GPU the game draws on, for the settings left on "auto"

   Why: a GPU built into the processor shares the CPU's memory and has a fraction of a graphics card's memory
   bandwidth; the same settings that a graphics card takes in its stride cost it the most per frame (the shadow
   map's lookups, many-sample anti-aliasing), and one AMD built-in GPU's driver here stopped responding with 8x
   anti-aliasing and every effect on.  A port addition: the Mac game ran on one kind of machine.

   The kind: the DXGI adapter that OpenGL draws on (matched by vendor, then by name) is asked by Direct3D 11 whether
   its memory is the CPU's (D3D11_FEATURE_D3D11_OPTIONS2 UnifiedMemoryArchitecture): true on Intel and AMD built-in
   GPUs, false on graphics cards.  Creating that Direct3D device takes ~100-200 ms, so it is done on a thread of
   its own started once GL's names are known (gmv_gpu_profile_start, from gl_context.c); the settings ask
   (gmv_gpu_integrated) when they are first needed, long after the answer is in.  NVIDIA makes no built-in PC GPUs:
   not asked.  Without an answer (no Direct3D 11.3, no matching adapter, Linux) the renderer's name decides.

   GPU_PROFILE: auto (default) asks; igpu / full take that kind without asking (a test, or a wrong guess); 0 = every
   setting on auto takes the graphics card's value, as before the profile.  What "auto" means for each setting is
   the caller's (MSAA in gl_msaa.c, the game's own). */
#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "log.h"
#include "shims.h"
#include "glshim.h"

static volatile LONG g_integrated = -1;        /* 1 built-in, 0 graphics card, -1 not known yet */
static char g_vendor[64], g_renderer[128];

int gmv_setting_is_auto(const char *v)
{
    return !v || !*v || !_stricmp(v, "auto");
}

static unsigned vendor_id(const char *v)
{
    if (strstr(v, "NVIDIA") || strstr(v, "nouveau"))
        return 0x10de;
    if (strstr(v, "ATI") || strstr(v, "AMD"))
        return 0x1002;
    if (strstr(v, "Intel"))
        return 0x8086;
    return 0;
}

/* By the name alone. */
static int by_name(unsigned vid, const char *r)
{
    if (strstr(r, "llvmpipe") || strstr(r, "softpipe") || strstr(r, "Basic Render") || strstr(r, "SVGA3D"))
        return 1;                               /* drawn by the CPU or a virtual machine: the lighter profile */
    if (vid == 0x8086)                          /* Intel: built in, but the Arc A / B cards */
        return !(strstr(r, "Arc(TM) A") || strstr(r, "Arc(TM) B") || strstr(r, "Arc A") || strstr(r, "Arc B") ||
                 strstr(r, "Iris(R) Xe MAX"));
    if (vid == 0x1002) {                        /* AMD: the processors' GPUs by their names */
        if (strstr(r, " RX ") || strstr(r, "Pro") || strstr(r, "Series") || strstr(r, "FirePro"))
            return 0;
        if (strstr(r, "Radeon(TM) Graphics") || strstr(r, "Radeon Graphics") || strstr(r, "Vega"))
            return 1;
        for (const char *p = r; (p = strstr(p, "Radeon")) != NULL; p++) {     /* 610M .. 890M */
            const char *d = strchr(p, ' ');
            if (d && d[1] >= '6' && d[1] <= '8' && d[2] >= '0' && d[2] <= '9' && d[3] >= '0' && d[3] <= '9' &&
                d[4] == 'M')
                return 1;
        }
    }
    return 0;
}

#ifdef _WIN32
static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

/* 1 / 0: the adapter's memory is / is not the CPU's; -1 no answer.  name: the adapter's. */
static int by_d3d(unsigned vid, const char *renderer, char *name, size_t nn)
{
    typedef HRESULT (WINAPI *factory_fn)(REFIID, void **);
    typedef HRESULT (WINAPI *device_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
                                        UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    HMODULE dx = LoadLibraryA("dxgi.dll"), d3 = LoadLibraryA("d3d11.dll");
    factory_fn mk = dx ? (factory_fn)(void *)GetProcAddress(dx, "CreateDXGIFactory1") : NULL;
    device_fn dev_mk = d3 ? (device_fn)(void *)GetProcAddress(d3, "D3D11CreateDevice") : NULL;
    IDXGIFactory1 *f = NULL;
    if (!mk || !dev_mk || FAILED(mk(&iid_factory1, (void **)&f)) || !f)
        return -1;
    IDXGIAdapter1 *best = NULL, *a;
    int best_score = -1;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        int score = -1;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (!vid || d.VendorId == vid)) {
            char n[128];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, n, sizeof n, NULL, NULL);
            score = 1 + (*n && !strncmp(renderer, n, strlen(n)));   /* GL's name starts with the adapter's */
            if (score > best_score)
                snprintf(name, nn, "%s", n);
        }
        if (score > best_score) {
            if (best)
                IDXGIAdapter1_Release(best);
            best = a, best_score = score;
        } else
            IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    if (!best)
        return -1;
    ID3D11Device *dev = NULL;
    HRESULT hr = dev_mk((IDXGIAdapter *)best, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev,
                        NULL, NULL);
    IDXGIAdapter1_Release(best);
    if (FAILED(hr) || !dev)
        return -1;
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2;
    memset(&o2, 0, sizeof o2);
    hr = ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2);
    ID3D11Device_Release(dev);
    return FAILED(hr) ? -1 : o2.UnifiedMemoryArchitecture ? 1 : 0;
}
#endif

static void detect(void)
{
    unsigned vid = vendor_id(g_vendor);
    const char *r = *g_renderer ? g_renderer : "(unknown GPU)";
    if (vid == 0x10de) {
        g_integrated = 0;
        gmv_log("[gpu] %s: a graphics card (NVIDIA); the settings on auto take its profile\n", r);
        return;
    }
    int k = -1;
    char name[128] = "";
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
#ifdef _WIN32
    k = by_d3d(vid, g_renderer, name, sizeof name);
#endif
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    g_integrated = k >= 0 ? k : by_name(vid, g_renderer);
    gmv_log("[gpu] %s: %s (%s, %.0f ms); the settings on auto take its profile\n", r,
            g_integrated ? "a GPU built into the processor" : "a graphics card",
            k > 0 ? "its memory is the CPU's, asked of Direct3D 11" : k == 0 ? "memory of its own, asked of Direct3D 11"
                  : "by its name",
            (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
    if (*name && strncmp(g_renderer, name, strlen(name)))
        gmv_log("[gpu] (Direct3D's adapter: %s)\n", name);
}

#ifdef _WIN32
static HANDLE g_worker;

static DWORD WINAPI worker_main(LPVOID p)
{
    (void)p;
    detect();
    return 0;
}
#endif

/* gl_context.c, once the context is current: GL's names are copied for the worker, which has no context. */
void gmv_gpu_profile_start(void)
{
    const char *e = gmv_getenv("GMV_GPU_PROFILE");
    const char *v = (const char *)glGetString(GL_VENDOR), *r = (const char *)glGetString(GL_RENDERER);
    snprintf(g_vendor, sizeof g_vendor, "%s", v ? v : "");
    snprintf(g_renderer, sizeof g_renderer, "%s", r ? r : "");
    if (e && *e == '0' && !e[1]) {
        g_integrated = 0;
        gmv_log("[gpu] GPU_PROFILE=0: the settings on auto take the graphics card's values on any GPU\n");
    } else if (e && !_stricmp(e, "igpu")) {
        g_integrated = 1;
        gmv_log("[gpu] %s: the built-in GPU's profile (GPU_PROFILE=igpu)\n", g_renderer);
    } else if (e && !_stricmp(e, "full")) {
        g_integrated = 0;
        gmv_log("[gpu] %s: the graphics card's profile (GPU_PROFILE=full)\n", g_renderer);
    } else {
#ifdef _WIN32
        g_worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
        if (!g_worker)
#endif
            detect();
    }
}

/* 1 = a GPU built into the processor.  Waits for the worker if it is still at it (it is not, by the first frame
   drawn in the city). */
int gmv_gpu_integrated(void)
{
#ifdef _WIN32
    if (g_integrated < 0 && g_worker && WaitForSingleObject(g_worker, 5000) != WAIT_OBJECT_0)
        gmv_log("[gpu] the GPU's kind took over 5 s to ask; the graphics card's profile meanwhile\n");
#endif
    return g_integrated > 0;
}
