/* OpenGL on Windows: a 3.3 core context through WGL, made current on a hidden
 * window at start (so the GE backend works headless too), then moved to the
 * game window for presenting. The only Windows-specific part of the OpenGL
 * renderer; another platform replaces this file (SDL_GL_CreateContext +
 * SDL_GL_GetProcAddress, EGL, GLX ...). */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "gl_load.h"
#include "gl_ge.h"

#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_FLAGS_ARB            0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);
/* opengl32.dll's own exports, loaded on demand: the exe does not import
 * OpenGL, so the D3D11 and software renderers never load it. */
typedef HGLRC (WINAPI *PFN_wglCreateContext)(HDC);
typedef BOOL  (WINAPI *PFN_wglDeleteContext)(HGLRC);
typedef BOOL  (WINAPI *PFN_wglMakeCurrent)(HDC, HGLRC);
typedef PROC  (WINAPI *PFN_wglGetProcAddress)(LPCSTR);
static PFN_wglCreateContext  p_wglCreateContext;
static PFN_wglDeleteContext  p_wglDeleteContext;
static PFN_wglMakeCurrent    p_wglMakeCurrent;
static PFN_wglGetProcAddress p_wglGetProcAddress;
#define wglCreateContext  p_wglCreateContext
#define wglDeleteContext  p_wglDeleteContext
#define wglMakeCurrent    p_wglMakeCurrent
#define wglGetProcAddress p_wglGetProcAddress

static HMODULE g_gl32;
static HWND    g_hidden, g_wnd;
static HDC     g_dc;
static HGLRC   g_rc;
static int     g_pf;
static PIXELFORMATDESCRIPTOR g_pfd;

static void *get_proc(const char *name) {
    void *p = (void *)wglGetProcAddress(name);
    /* wglGetProcAddress returns small sentinels for unsupported names, and
     * NULL for the GL 1.1 entry points opengl32.dll exports directly. */
    if (p == NULL || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1)
        p = (void *)GetProcAddress(g_gl32, name);
    return p;
}

static int set_format(HDC dc) {
    if (!g_pf) {
        memset(&g_pfd, 0, sizeof g_pfd);
        g_pfd.nSize = sizeof g_pfd;
        g_pfd.nVersion = 1;
        g_pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        g_pfd.iPixelType = PFD_TYPE_RGBA;
        g_pfd.cColorBits = 32;
        g_pfd.iLayerType = PFD_MAIN_PLANE;
        g_pf = ChoosePixelFormat(dc, &g_pfd);
        if (!g_pf) return -1;
    }
    return SetPixelFormat(dc, g_pf, &g_pfd) ? 0 : -1;
}

int gl_platform_init(void) {
    g_gl32 = LoadLibraryA("opengl32.dll");
    if (!g_gl32) { fprintf(stderr, "gl: no opengl32.dll\n"); return -1; }
    p_wglCreateContext  = (PFN_wglCreateContext)(void *)GetProcAddress(g_gl32, "wglCreateContext");
    p_wglDeleteContext  = (PFN_wglDeleteContext)(void *)GetProcAddress(g_gl32, "wglDeleteContext");
    p_wglMakeCurrent    = (PFN_wglMakeCurrent)(void *)GetProcAddress(g_gl32, "wglMakeCurrent");
    p_wglGetProcAddress = (PFN_wglGetProcAddress)(void *)GetProcAddress(g_gl32, "wglGetProcAddress");
    if (!p_wglCreateContext || !p_wglDeleteContext || !p_wglMakeCurrent || !p_wglGetProcAddress) {
        fprintf(stderr, "gl: opengl32.dll lacks the WGL functions\n");
        return -1;
    }
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "psp2i_gl";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    g_hidden = CreateWindowA("psp2i_gl", "", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);
    if (!g_hidden) return -1;
    g_dc = GetDC(g_hidden);
    if (set_format(g_dc) != 0) { fprintf(stderr, "gl: no OpenGL pixel format\n"); return -1; }
    HGLRC legacy = wglCreateContext(g_dc);
    if (!legacy || !wglMakeCurrent(g_dc, legacy)) { fprintf(stderr, "gl: cannot create an OpenGL context\n"); return -1; }
    PFN_wglCreateContextAttribsARB create = (PFN_wglCreateContextAttribsARB)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create) { fprintf(stderr, "gl: no WGL_ARB_create_context (driver too old)\n"); wglDeleteContext(legacy); return -1; }
    const int attrs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                          WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    g_rc = create(g_dc, NULL, attrs);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);
    if (!g_rc || !wglMakeCurrent(g_dc, g_rc)) { fprintf(stderr, "gl: no OpenGL 3.3 core context\n"); return -1; }
    const int missing = gl_load(get_proc);
    if (missing) { fprintf(stderr, "gl: %d entry points missing\n", missing); return -1; }
    PFN_wglSwapIntervalEXT swap = (PFN_wglSwapIntervalEXT)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap) swap(0);                                   /* the scheduler paces frames */
    return 0;
}

int gl_platform_attach(void *window) {
    HWND w = (HWND)window;
    if (!g_rc || !w) return -1;
    HDC dc = GetDC(w);
    if (set_format(dc) != 0 || !wglMakeCurrent(dc, g_rc)) {
        fprintf(stderr, "gl: cannot draw to the window; staying off-screen\n");
        ReleaseDC(w, dc);
        wglMakeCurrent(g_dc, g_rc);
        return -1;
    }
    g_wnd = w;
    g_dc = dc;
    PFN_wglSwapIntervalEXT swap = (PFN_wglSwapIntervalEXT)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap) swap(0);
    return 0;
}

void gl_platform_swap(void) { if (g_wnd) SwapBuffers(g_dc); }

#endif
