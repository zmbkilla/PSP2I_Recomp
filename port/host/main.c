/* psp2i — host for the recompiled Phantasy Star Portable 2 Infinity
 * (NPJH50332) module `psu`.
 *
 * What a PSP does between "load the executable" and "the game's code runs",
 * done here explicitly:
 *
 *   1. load the ELF's PT_LOAD segment at its link address;
 *   2. hand the rest of user memory (0x08800000-0x0A000000) above the module
 *      to the allocator -- the module's .bss runs to 0x08FA6744, so the
 *      allocator's built-in default would hand out the game's own globals;
 *   3. register every recompiled function for indirect calls;
 *   4. start module_start as the first thread, with argv[0] the boot path in
 *      guest memory, its $gp from the module info;
 *   5. run the scheduler, presenting the framebuffer every vblank.
 *
 * usage: psp2i [--root DIR] [--eboot FILE] [--headless] [--seconds N]
 *              [--capture-every N] [--capture-dir DIR]
 *
 *   --root      directory holding disc/ (the UMD contents) and ms/ (memory
 *               stick); default: GameData next to the executable, else ./GameData
 *   --eboot     the decrypted EBOOT.BIN; default: EBOOT.BIN next to the
 *               executable, else <root>/disc/PSP_GAME/SYSDIR/EBOOT.BIN
 *   --headless  no window
 *   --seconds   stop after N seconds of game time and print the report
 *   --capture-every / --capture-dir  dump the framebuffer as PPM every N vblanks
 *   --fps 30|60  initial frame rate through the game's own frame-rate API
 *               (framerate.c); changeable at run time in the settings menu
 *   --menu-key F1..F9|F11  the keyboard key that opens/closes the settings
 *               menu (default F1; the controller's Guide or R3 also do)
 *   --show-fps / --hide-fps  the measured-FPS counter at start (default shown)
 *   --rs-speed 1.0..2.0  right-stick camera sensitivity at start (default
 *               1.0; the settings menu changes it in 0.25 steps)
 *   --rstick X,Y@VBLANK[+N]  scripted right stick (PSP-style bytes), for tests
 *   --overlay-shot VBLANK  (repeatable) save the presented image, overlay
 *               included, as <capture-dir>/overlay_<vblank>.ppm
 *   --audio / --no-audio  sound through SDL3.dll (audio_sdl.c): on by default
 *               with a window, off headless; PSP2I_AUDIO_DUMP=file.wav records
 *   --sdl / --no-sdl  controllers through SDL3.dll (input_sdl.c): on by default
 *               with a window, off headless unless --sdl
 *   --resolution 960x544|1280x720|1920x1080|2560x1440|3840x2160|fullscreen
 *               window size for this run (default: psp2i_display.ini next to
 *               the exe, else 1920x1080); the settings menu changes and saves
 *               it, Alt+Enter toggles fullscreen. PSP2I_PRESENT=gdi skips the
 *               GPU presenter (present.c) and scales with GDI.
 *   --oracle    (trace builds) diff the function at ADDR against an interpreter
 *               the first time it runs; see host/oracle.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/vfpu.h>
#include <time.h>
#include <psprecomp/render.h>

#include "recomp_funcs.h"
#include "input_sdl.h"
#include "audio_sdl.h"
#include "atrac_ffmpeg.h"
#include "framerate.h"
#include "menu.h"
#include "camera.h"
#include "login.h"
#include "online.h"
#include "savecrypt.h"
#include "gamelog.h"
#include "prod.h"
#include "textedit.h"
#include "present.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  include <dbghelp.h>
#  include <mmsystem.h>
#  define host_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define host_mkdir(p) mkdir(p, 0777)
#endif

#define USER_PARTITION_TOP 0x0A000000u
#define SCREEN_W 480
#define SCREEN_H 272

/* ---- ELF loading ---------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf32_Ehdr;
typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} Elf32_Phdr;
typedef struct {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
} Elf32_Shdr;
#pragma pack(pop)

typedef struct {
    uint32_t entry;
    uint32_t gp;
    uint32_t load_lo, load_hi;
    uint32_t stub_lo, stub_hi;   /* .sceStub.text: the import thunks */
    char     name[29];
} module_info;

void oracle_arm(uint32_t addr, uint32_t stub_lo, uint32_t stub_hi);
void oracle_arm_sweep(uint32_t stub_lo, uint32_t stub_hi);
void oracle_report(FILE *out);
void dump_arm(uint32_t addr, const char *path);
void args_arm(uint32_t addr, uint64_t flip);
void dump_on_bad_access(const char *path);
void ring_arm(void);
#ifdef _WIN32
int  d3d11_init(void);
void d3d11_report(FILE *out);
int  d3d11_present(uint32_t addr, uint32_t stride, int fmt, uint32_t *out, int w, int h);
#endif

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

static int load_elf(const char *path, module_info *mi) {
    size_t len = 0;
    uint8_t *b = read_file(path, &len);
    if (!b) { fprintf(stderr, "psp2i: cannot read %s\n", path); return -1; }
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)b;
    if (len < sizeof *eh || memcmp(eh->ident, "\x7F" "ELF", 4) != 0 || eh->machine != 8) {
        fprintf(stderr, "psp2i: %s is not a decrypted MIPS ELF (encrypted EBOOTs must be decrypted first)\n", path);
        free(b);
        return -1;
    }
    memset(mi, 0, sizeof *mi);
    mi->entry = eh->entry;
    mi->load_lo = 0xFFFFFFFFu;

    for (unsigned i = 0; i < eh->phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(b + eh->phoff + i * eh->phentsize);
        if (ph->type != 1) continue;
        if ((size_t)ph->offset + ph->filesz > len ||
            psp_mem_write_block(ph->vaddr, b + ph->offset, ph->filesz) != 0) {
            fprintf(stderr, "psp2i: cannot place segment at 0x%08X\n", ph->vaddr);
            free(b);
            return -1;
        }
        /* .bss: the memory map starts zeroed, but be explicit. */
        for (uint32_t a = ph->vaddr + ph->filesz; a < ph->vaddr + ph->memsz; a++) psp_write8(a, 0);
        if (ph->vaddr < mi->load_lo) mi->load_lo = ph->vaddr;
        if (ph->vaddr + ph->memsz > mi->load_hi) mi->load_hi = ph->vaddr + ph->memsz;
        printf("PT_LOAD 0x%08X..0x%08X (file %u bytes)\n", ph->vaddr, ph->vaddr + ph->memsz, ph->filesz);
    }

    /* $gp and the module name come from .rodata.sceModuleInfo:
     * u16 attribute, u8 version[2], char name[28], u32 gp, ... */
    if (eh->shoff && eh->shstrndx < eh->shnum) {
        const Elf32_Shdr *sh = (const Elf32_Shdr *)(b + eh->shoff);
        const char *strtab = (const char *)(b + sh[eh->shstrndx].offset);
        for (unsigned i = 0; i < eh->shnum; i++) {
            if (!strcmp(strtab + sh[i].name, ".sceStub.text")) {
                mi->stub_lo = sh[i].addr;
                mi->stub_hi = sh[i].addr + sh[i].size;
            }
            if (strcmp(strtab + sh[i].name, ".rodata.sceModuleInfo") != 0) continue;
            const uint8_t *m = b + sh[i].offset;
            memcpy(mi->name, m + 4, 28);
            memcpy(&mi->gp, m + 32, 4);
        }
    }
    free(b);
    if (!mi->gp) { fprintf(stderr, "psp2i: no module info, cannot find $gp\n"); return -1; }
    return 0;
}

/* ---- hooks the generated code calls ------------------------------------- */

void psp_syscall(uint32_t id) {
    /* Firmware calls go through the import thunks, never a raw syscall, so
     * reaching one means control is in data being executed as code. */
    fprintf(stderr, "psp2i: raw syscall 0x%05X reached\n", id);
    psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED);
}

/* An instruction the recompiler could not translate. Reported per address
 * with a count -- once a site is known, repeating it hides new ones. */
#define UNIMPL_SITES 256
static struct { uint32_t addr; uint64_t hits; const char *what; } g_unimpl[UNIMPL_SITES];
static int g_unimpl_n;

void psp_unimplemented(uint32_t addr, const char *what) {
    for (int i = 0; i < g_unimpl_n; i++)
        if (g_unimpl[i].addr == addr) { g_unimpl[i].hits++; return; }
    if (g_unimpl_n < UNIMPL_SITES) {
        g_unimpl[g_unimpl_n].addr = addr;
        g_unimpl[g_unimpl_n].what = what;
        g_unimpl[g_unimpl_n].hits = 1;
        g_unimpl_n++;
    }
    fprintf(stderr, "psp2i: untranslated instruction at 0x%08X: %s\n", addr, what ? what : "?");
}

/* ---- presentation --------------------------------------------------------- */

static uint32_t g_rgba[SCREEN_W * SCREEN_H];   /* 0xAARRGGBB for GDI: the frame plus the overlay */
static uint32_t g_frame[SCREEN_W * SCREEN_H];  /* the game's frame alone (grab_frame, d3d11_present) */
static int      g_have_frame;

/* Convert the current framebuffer. Returns 0 if the game has not set one. */
static int grab_frame(void) {
    uint32_t addr, stride, fmt;
    psp_display_get(&addr, &stride, &fmt);
    if (!addr) return 0;
    const int bpp = fmt == 3 ? 4 : 2;
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t at = addr + (uint32_t)(y * (int)stride + x) * (uint32_t)bpp;
            uint32_t p = bpp == 4 ? psp_read32(at) : psp_read16(at);
            uint32_t r, g, b;
            switch (fmt) {
            case 0:  r = (p & 0x1F) * 255 / 31; g = ((p >> 5) & 0x3F) * 255 / 63; b = ((p >> 11) & 0x1F) * 255 / 31; break;
            case 1:  r = (p & 0x1F) * 255 / 31; g = ((p >> 5) & 0x1F) * 255 / 31; b = ((p >> 10) & 0x1F) * 255 / 31; break;
            case 2:  r = (p & 0xF) * 17; g = ((p >> 4) & 0xF) * 17; b = ((p >> 8) & 0xF) * 17; break;
            default: r = p & 0xFF; g = (p >> 8) & 0xFF; b = (p >> 16) & 0xFF; break;
            }
            g_frame[y * SCREEN_W + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return 1;
}

static void ctrl_commit(void);
static int g_dump_key;                     /* F12 pressed: capture on the next vblank */

/* The settings menu and the FPS counter (menu.c), drawn over the presented
 * image -- the same for either renderer, and never in the game's VRAM, so
 * F12 dumps and --capture-every frames stay the game's own. */
static menu_state g_menu;
static fps_meter  g_fpsm;
static uint32_t   g_host_held, g_host_latched;  /* MI_* from keys the game never sees */
static uint32_t   g_keys_latched;          /* PSP bits of keys pressed since the last vblank */
static char       g_menu_key_name[8] = "F1";

/* The PSN sign-in screen (login.c), opened when the game asks to sign in;
 * while open it takes all input. */
static login_state g_login;
static uint32_t    g_login_keys;          /* MI_* from navigation keys while a text screen is open */
/* The settings menu's SEGA SERVER editor (textedit.c). */
static textedit    g_sega_edit;
static int         g_edit_enter;          /* Enter pressed in the editor: save */

static int text_ui_open(void) { return g_login.state != LOGIN_CLOSED || g_sega_edit.open; }

static void draw_overlay(void) {
    menu_image img = { g_rgba, SCREEN_W, SCREEN_H };
    if (g_menu.show_fps) fps_draw(&g_fpsm, &img);
    const char *st = online_status_line();
    if (st) {
        char line[96];
        snprintf(line, sizeof line, "%.78s", st);
        const int w = menu_text_width(line);
        for (int y = 2; y < 13; y++) for (int x = SCREEN_W - w - 7; x < SCREEN_W - 2; x++)
            if (x >= 0) g_rgba[y * SCREEN_W + x] = 0xFF000000u | ((g_rgba[y * SCREEN_W + x] >> 2) & 0x003F3F3Fu);
        menu_text(&img, SCREEN_W - w - 4, 4, line, 0xFFFFFF);
    }
    login_draw(&g_login, &img);
    menu_draw(&g_menu, &img, framerate_game_fps(), g_menu_key_name);
    textedit_draw(&g_sega_edit, &img);
}

/* What the window shows: the latest game frame with the overlay on top. */
static void compose(void) {
    memcpy(g_rgba, g_frame, sizeof g_rgba);
    draw_overlay();
}

#ifdef _WIN32
static HWND g_wnd;
static int g_alt_enter;                     /* Alt+Enter pressed; handled with the menu */
static uint32_t g_keys;
static WPARAM g_menu_vk = VK_F1;

/* Keys for the menu only: the hotkey, and Escape/Backspace as Back. */
static uint32_t host_key(WPARAM vk) {
    if (vk == g_menu_vk) return MI_TOGGLE;
    if (vk == VK_ESCAPE || vk == VK_BACK) return MI_BACK;
    return 0;
}

static uint32_t key_bit(WPARAM vk) {
    switch (vk) {
    case VK_UP:     return 0x0010;
    case VK_RIGHT:  return 0x0020;
    case VK_DOWN:   return 0x0040;
    case VK_LEFT:   return 0x0080;
    case 'Q':       return 0x0100;   /* L */
    case 'W':       return 0x0200;   /* R */
    case 'S':       return 0x1000;   /* triangle */
    case 'X':       return 0x2000;   /* circle */
    case 'Z':       return 0x4000;   /* cross */
    case 'A':       return 0x8000;   /* square */
    case VK_RETURN: return 0x0008;   /* start */
    case VK_SPACE:  return 0x0001;   /* select */
    default:        return 0;
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CHAR:
        /* Text for the sign-in screen (letters do not act as PSP buttons then). */
        if (g_sega_edit.open) { if (wp > 32 && wp < 127) textedit_type(&g_sega_edit, (char)wp); }
        else if (g_login.state != LOGIN_CLOSED && wp >= 32 && wp < 127) login_type(&g_login, (char)wp);
        return 0;
    case WM_KEYDOWN: {
        const int first = !(lp & (1 << 30));                     /* not an auto-repeat */
        if (wp == VK_F12 && first) g_dump_key = 1;
        if (text_ui_open() && wp != g_menu_vk) {
            uint32_t k = 0;
            switch (wp) {
            case VK_UP: k = MI_UP; break;
            case VK_DOWN: case VK_TAB: k = MI_DOWN; break;
            case VK_LEFT: k = MI_LEFT; break;
            case VK_RIGHT: k = MI_RIGHT; break;
            case VK_RETURN: if (g_sega_edit.open) g_edit_enter = 1; else k = MI_CONFIRM; break;
            case VK_ESCAPE: k = MI_BACK; break;
            case VK_BACK: if (g_sega_edit.open) textedit_backspace(&g_sega_edit); else login_backspace(&g_login); break;
            default: break;
            }
            g_login_keys |= k;
            return 0;
        }
        const uint32_t hk = host_key(wp);
        if (hk) { g_host_held |= hk; if (first) g_host_latched |= hk; return 0; }
        if (first) g_keys_latched |= key_bit(wp);
        g_keys |= key_bit(wp);  ctrl_commit(); return 0;
    }
    case WM_KEYUP:
        switch (wp) {
        case VK_UP: g_login_keys &= ~MI_UP; break;
        case VK_DOWN: case VK_TAB: g_login_keys &= ~MI_DOWN; break;
        case VK_LEFT: g_login_keys &= ~MI_LEFT; break;
        case VK_RIGHT: g_login_keys &= ~MI_RIGHT; break;
        case VK_RETURN: g_login_keys &= ~MI_CONFIRM; break;
        case VK_ESCAPE: g_login_keys &= ~MI_BACK; break;
        default: break;
        }
        g_host_held &= ~host_key(wp); g_keys &= ~key_bit(wp); ctrl_commit(); return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {               /* Alt+Enter */
            if (!(lp & (1 << 30))) g_alt_enter = 1;
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == '\r') return 0;                                 /* no beep for Alt+Enter */
        break;
    case WM_SIZE:    present_resized(); InvalidateRect(h, NULL, FALSE); return 0;
    case WM_ERASEBKGND: return 1;                                 /* WM_PAINT covers it all */
    case WM_CLOSE:   psp_request_exit(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (present_active()) { EndPaint(h, &ps); return 0; }     /* the swap chain shows it */
        RECT rc;
        GetClientRect(h, &rc);
        /* Aspect kept, black bars (GDI fallback; present.c does this on the GPU). */
        int x, y, w, hh;
        present_fit(rc.right, rc.bottom, SCREEN_W, SCREEN_H, &x, &y, &w, &hh);
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT bar;
        if (x > 0) {
            SetRect(&bar, 0, 0, x, rc.bottom); FillRect(dc, &bar, black);
            SetRect(&bar, x + w, 0, rc.right, rc.bottom); FillRect(dc, &bar, black);
        }
        if (y > 0) {
            SetRect(&bar, 0, 0, rc.right, y); FillRect(dc, &bar, black);
            SetRect(&bar, 0, y + hh, rc.right, rc.bottom); FillRect(dc, &bar, black);
        }
        SetStretchBltMode(dc, COLORONCOLOR);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = SCREEN_W;
        bi.bmiHeader.biHeight = -SCREEN_H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, x, y, w, hh, 0, 0, SCREEN_W, SCREEN_H,
                      g_rgba, &bi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* Size the window for a RES_ mode (menu.h): a framed window with that client
 * size, shrunk (aspect kept) to fit the monitor's work area, or borderless
 * covering the whole monitor for RES_FULLSCREEN. */
static void window_apply_res(int mode) {
    if (!g_wnd) return;
    if (IsZoomed(g_wnd) || IsIconic(g_wnd)) ShowWindow(g_wnd, SW_RESTORE);
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi);
    int w, h;
    if (!menu_res_size(mode, &w, &h)) {
        const RECT m = mi.rcMonitor;
        SetWindowLongPtrA(g_wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_wnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    } else {
        const RECT wa = mi.rcWork;
        RECT r = { 0, 0, w, h };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        const int fw = (r.right - r.left) - w, fh = (r.bottom - r.top) - h;   /* frame */
        const int aw = (wa.right - wa.left) - fw, ah = (wa.bottom - wa.top) - fh;
        if (w > aw || h > ah) {
            int x, y;
            present_fit(aw, ah, w, h, &x, &y, &w, &h);
            fprintf(stderr, "window: %s does not fit this monitor; using %dx%d\n", menu_res_name(mode), w, h);
        }
        SetWindowLongPtrA(g_wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_wnd, HWND_NOTOPMOST,
                     wa.left + ((wa.right - wa.left) - (w + fw)) / 2, wa.top + ((wa.bottom - wa.top) - (h + fh)) / 2,
                     w + fw, h + fh, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    present_resized();
}

static void window_open(int res) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "psp2i";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    g_wnd = CreateWindowA("psp2i", "PSP2i (recompiled)", WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, SCREEN_W * 2, SCREEN_H * 2,
                          NULL, NULL, wc.hInstance, NULL);
    window_apply_res(res);
    const char *e = getenv("PSP2I_PRESENT");
    if (e && !strcmp(e, "gdi")) fprintf(stderr, "present: PSP2I_PRESENT=gdi, scaling with GDI\n");
    else present_init(g_wnd);
}

static void window_pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
}
#endif

/* ---- the per-vblank hook --------------------------------------------------- */

static int      g_headless;
static double   g_seconds;
static uint32_t g_capture_every;
static char     g_capture_dir[512] = "captures";
#define MAX_OVERLAY_SHOT 16
static uint64_t g_overlay_shot[MAX_OVERLAY_SHOT];   /* --overlay-shot */
static int      g_noverlay_shot;

/* Scripted input for headless runs: --press BUTTON@VBLANK[+FRAMES] holds a
 * button from that vblank for FRAMES vblanks (default 6). Repeatable. */
#define MAX_PRESS 128
static struct { uint32_t bits; uint64_t at, len; } g_press[MAX_PRESS];
static int g_npress;
static uint32_t g_script_bits;
#define PRESS_MENU 0x80000000u             /* --press menu@...: the settings-menu hotkey, never the game's */

static int add_press(const char *spec) {
    static const struct { const char *name; uint32_t bit; } B[] = {
        { "select", 0x0001 }, { "start", 0x0008 }, { "up", 0x0010 }, { "right", 0x0020 },
        { "down", 0x0040 }, { "left", 0x0080 }, { "l", 0x0100 }, { "r", 0x0200 },
        { "triangle", 0x1000 }, { "circle", 0x2000 }, { "cross", 0x4000 }, { "square", 0x8000 },
        { "menu", PRESS_MENU },
    };
    char name[32];
    unsigned long long at = 0, len = 6;
    if (g_npress >= MAX_PRESS || sscanf(spec, "%31[a-z]@%llu+%llu", name, &at, &len) < 2) return -1;
    for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) {
        if (strcmp(name, B[i].name)) continue;
        g_press[g_npress].bits = B[i].bit;
        g_press[g_npress].at = at;
        g_press[g_npress].len = len;
        g_npress++;
        return 0;
    }
    return -1;
}

/* --stick X,Y@VBLANK[+FRAMES]: hold the analog stick at PSP values X,Y
 * (0..255, 128 = centre) -- scripted analog input for headless tests. */
#define MAX_STICK 32
static struct { uint8_t x, y; uint64_t at, len; } g_stick[MAX_STICK];
static int g_nstick;
static int g_script_stick = -1;            /* index held now, -1 none */
static struct { uint8_t x, y; uint64_t at, len; } g_rstick[MAX_STICK];   /* --rstick */
static int g_nrstick;

static int add_stick(const char *spec) {
    unsigned x, y;
    unsigned long long at = 0, len = 6;
    if (g_nstick >= MAX_STICK || sscanf(spec, "%u,%u@%llu+%llu", &x, &y, &at, &len) < 3 || x > 255 || y > 255)
        return -1;
    g_stick[g_nstick].x = (uint8_t)x; g_stick[g_nstick].y = (uint8_t)y;
    g_stick[g_nstick].at = at; g_stick[g_nstick].len = len;
    g_nstick++;
    return 0;
}

/* --dump-ram-at VBLANK PATH (repeatable): guest RAM as a raw file. */
#define MAX_RAMDUMP 8
static struct { uint64_t at; char path[512]; } g_ramdump[MAX_RAMDUMP];
static int g_nramdump;

static void dump_ram_now(uint64_t vb) {
    for (int i = 0; i < g_nramdump; i++) {
        if (g_ramdump[i].at != vb) continue;
        FILE *f = fopen(g_ramdump[i].path, "wb");
        if (f) { fwrite(psp_mem.ram, 1, PSP_RAM_SIZE, f); fclose(f); }
        fprintf(stderr, "ram: dumped at vblank %llu -> %s\n", (unsigned long long)vb, g_ramdump[i].path);
    }
}

/* --log-floats ADDR COUNT EVERY: print COUNT floats at guest ADDR every
 * EVERY vblanks (e.g. a player position while testing movement). */
static uint32_t g_logf_addr, g_logf_count, g_logf_every;

static void apply_script(uint64_t vb) {
    if (g_nramdump) dump_ram_now(vb);
    if (g_logf_every && vb % g_logf_every == 0) {
        fprintf(stderr, "floats: vblank %llu flip %llu", (unsigned long long)vb, (unsigned long long)psp_display_flips());
        for (uint32_t i = 0; i < g_logf_count; i++) {
            uint32_t w = psp_read32(g_logf_addr + i * 4);
            float v;
            memcpy(&v, &w, 4);
            fprintf(stderr, " %.3f", v);
        }
        fprintf(stderr, "\n");
    }
    if (g_nstick) {
        int k = -1;
        for (int i = 0; i < g_nstick; i++)
            if (vb >= g_stick[i].at && vb < g_stick[i].at + g_stick[i].len) k = i;
        if (k != g_script_stick) {
            g_script_stick = k;
            if (k >= 0) fprintf(stderr, "input: stick %u,%u at vblank %llu\n", g_stick[k].x, g_stick[k].y, (unsigned long long)vb);
            else fprintf(stderr, "input: stick centred at vblank %llu\n", (unsigned long long)vb);
            ctrl_commit();
        }
    }
    if (!g_npress) return;
    uint32_t bits = 0;
    for (int i = 0; i < g_npress; i++)
        if (vb >= g_press[i].at && vb < g_press[i].at + g_press[i].len) bits |= g_press[i].bits;
    if (bits != g_script_bits) {
        fprintf(stderr, "input: buttons 0x%04X at vblank %llu\n", bits, (unsigned long long)vb);
        g_script_bits = bits;
        ctrl_commit();
    }
}

/* What the game reads: keyboard, SDL controllers and the --press script
 * combined. Buttons are OR-ed; the analog stick comes from the controllers
 * (centred when none is deflected). */
static int      g_sdl_on;
static uint32_t g_pad_bits;
static uint8_t  g_pad_ax = 128, g_pad_ay = 128;

static void ctrl_commit(void) {
    uint32_t bits = g_script_bits | g_pad_bits;
#ifdef _WIN32
    bits |= g_keys;
#endif
    uint8_t ax = g_pad_ax, ay = g_pad_ay;
    if (g_script_stick >= 0) { ax = g_stick[g_script_stick].x; ay = g_stick[g_script_stick].y; }
    /* The settings menu takes all input while open (menu_filter_game). */
    if (g_login.state != LOGIN_CLOSED) {
        /* The sign-in screen has the input; buttons still held when it
         * closes stay hidden from the game until released. */
        g_menu.game_mask |= bits & ~PRESS_MENU;
        bits = 0;
        ax = ay = 128;
    }
    bits = menu_filter_game(&g_menu, bits & ~PRESS_MENU);
    if (g_menu.open) ax = ay = 128;
    psp_ctrl_set(bits, ax, ay);
}

/* PSP buttons as menu inputs (menu_inputs_from_psp: Circle accepts, Cross
 * goes back, as in the game), plus the scripted hotkey. */
static uint32_t menu_inputs(uint32_t psp, uint8_t ax, uint8_t ay) {
    return menu_inputs_from_psp(psp, ax, ay) | ((psp & PRESS_MENU) ? MI_TOGGLE : 0);
}

/* The window size (menu RESOLUTION), kept in psp2i_display.ini next to the exe:
 *   resolution=1920x1080      (or 960x544 ... 3840x2160, fullscreen) */
static char g_display_ini[600];
static int  g_last_windowed = RES_DEFAULT;  /* where Alt+Enter returns from fullscreen */

static int display_load(void) {
    FILE *f = g_display_ini[0] ? fopen(g_display_ini, "r") : NULL;
    if (!f) return -1;
    char line[128];
    int res = -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "resolution=", 11)) res = menu_res_parse(line + 11);
    fclose(f);
    return res;
}

static void display_save(int res) {
    FILE *f = g_display_ini[0] ? fopen(g_display_ini, "w") : NULL;
    if (!f) { fprintf(stderr, "display: cannot write %s\n", g_display_ini); return; }
    fprintf(f, "; psp2i display settings (the settings menu changes this)\n");
    fprintf(f, "resolution=%s\n", menu_res_name(res));
    fclose(f);
}

static void display_set(int res) {
    if (res != RES_FULLSCREEN) g_last_windowed = res;
    if (g_headless) { fprintf(stderr, "menu: resolution %s (headless: not saved)\n", menu_res_name(res)); return; }
#ifdef _WIN32
    window_apply_res(res);
#endif
    display_save(res);
    fprintf(stderr, "menu: resolution %s (saved)\n", menu_res_name(res));
}

/* Once per vblank: the menu reads the raw input (the game's is filtered). */
static void menu_step(void) {
    uint32_t psp = g_script_bits | g_pad_bits, latched = g_host_latched;
#ifdef _WIN32
    psp |= g_keys;
    latched |= menu_inputs(g_keys_latched, 128, 128);
#endif
    g_keys_latched = 0;
    g_host_latched = 0;
#ifdef _WIN32
    if (g_alt_enter) {
        g_alt_enter = 0;
        g_menu.res = g_menu.res == RES_FULLSCREEN ? g_last_windowed : RES_FULLSCREEN;
        display_set(g_menu.res);
    }
#endif
    uint8_t ax = g_pad_ax, ay = g_pad_ay;
    if (g_script_stick >= 0) { ax = g_stick[g_script_stick].x; ay = g_stick[g_script_stick].y; }
    uint32_t held = menu_inputs(psp, ax, ay) | g_host_held;
    if (input_sdl_host_buttons() & (INPUT_HOST_GUIDE | INPUT_HOST_RSTICK)) held |= MI_TOGGLE;

    snprintf(g_menu.sega, sizeof g_menu.sega, "%s", online_sega_redirect());
    if (g_sega_edit.open) {
        /* The SEGA SERVER editor has the input (pad buttons and navigation
         * keys; typed characters arrive through WM_CHAR). */
        const uint32_t pad = g_script_bits | g_pad_bits;
        int r = textedit_update(&g_sega_edit, menu_inputs_from_psp(pad & 0xFFFF, ax, ay) | g_login_keys, pad);
        if (g_edit_enter) { g_edit_enter = 0; g_sega_edit.open = 0; r = TE_SAVE; }
        if (r == TE_SAVE) {
            online_set_sega_redirect(g_sega_edit.text);
            fprintf(stderr, "menu: SEGA server redirect %s\n", online_sega_redirect()[0] ? online_sega_redirect() : "off");
        }
        menu_update(&g_menu, 0, 0);       /* keep its press detection in step */
        g_menu.prev = held;               /* no stray press when the editor closes */
        return;
    }

    const int fx = menu_update(&g_menu, held, latched);
    if (fx & MFX_EDIT_SEGA) {
        char hint[96];
        snprintf(hint, sizeof hint, "%.22s -> HOST[:PORT], EMPTY = OFF", online_sega_host());
        textedit_open(&g_sega_edit, "SEGA SERVER REDIRECT", hint, online_sega_redirect());
    }
    if (fx & MFX_FPS_CHANGED) {
        framerate_set(g_menu.fps);
        fprintf(stderr, "menu: frame rate %d fps\n", g_menu.fps);
    }
    if (fx & MFX_RES_CHANGED) display_set(g_menu.res);
    if (fx & MFX_SHOW_FPS) fprintf(stderr, "menu: FPS counter %s\n", g_menu.show_fps ? "on" : "off");
    if (fx & MFX_RS_SPEED) {
        camera_set_speed(menu_rs_speed(&g_menu));
        fprintf(stderr, "menu: right-stick sensitivity %.2fx\n", (double)menu_rs_speed(&g_menu));
    }
    if (fx & (MFX_OPENED | MFX_CLOSED)) {
        fprintf(stderr, "menu: %s at vblank %llu\n", (fx & MFX_OPENED) ? "opened" : "closed",
                (unsigned long long)psp_sched_vblank_count());
        ctrl_commit();                     /* take or give back the game's input now */
    }
}

/* The right stick for the camera (camera.c): SDL, or --rstick in scripts;
 * centred while the settings menu is open. */
static void update_camera_stick(uint64_t vb) {
    uint8_t rx = 128, ry = 128;
    if (g_sdl_on) input_sdl_right_stick(&rx, &ry);
    for (int i = 0; i < g_nrstick; i++)
        if (vb >= g_rstick[i].at && vb < g_rstick[i].at + g_rstick[i].len) { rx = g_rstick[i].x; ry = g_rstick[i].y; }
    if (g_menu.open) rx = ry = 128;
    camera_set_stick(rx, ry);
}

static void poll_controllers(void) {
    if (!g_sdl_on) return;
    uint32_t b;
    uint8_t ax, ay;
    input_sdl_poll(&b, &ax, &ay);
    if (b != g_pad_bits || ax != g_pad_ax || ay != g_pad_ay) {
        g_pad_bits = b; g_pad_ax = ax; g_pad_ay = ay;
        ctrl_commit();
    }
}

/* Frame-rate measurement: the game's frames are its framebuffer flips, sampled
 * over windows of 60 vblanks (about one second of host time). */
#define FPS_MAX_WIN 4096
static double   g_fps_win[FPS_MAX_WIN];
static int      g_fps_n;
static uint64_t g_fps_last_flips, g_fps_last_us;
static int      g_fps_log = -1;

static void sample_fps(uint64_t vb) {
    if (vb % 60) return;
    uint64_t now = psp_sched_now_us(), flips = psp_display_flips();
    if (g_fps_last_us && g_fps_n < FPS_MAX_WIN) {
        double fps = (double)(flips - g_fps_last_flips) * 1e6 / (double)(now - g_fps_last_us);
        g_fps_win[g_fps_n++] = fps;
        if (g_fps_log < 0) g_fps_log = getenv("PSP2I_FPS_LOG") != NULL;
        if (g_fps_log) fprintf(stderr, "fps: %5.1f at vblank %llu (counter shows %.1f)\n", fps,
                               (unsigned long long)vb, fps_meter_value(&g_fpsm));
    }
    g_fps_last_flips = flips;
    g_fps_last_us = now;
}

static void report_fps(void) {
    /* Skip the first 5 windows (boot, logos) when summarising steady state. */
    int from = g_fps_n > 10 ? 5 : 0, n = 0, below = 0;
    double sum = 0, mn = 1e9;
    for (int i = from; i < g_fps_n; i++) {
        sum += g_fps_win[i]; n++;
        if (g_fps_win[i] < mn) mn = g_fps_win[i];
        if (g_fps_win[i] < 29.0) below++;
    }
    if (!n) return;
    double run = psp_sched_now_us() / 1e6;
    fprintf(stderr, "  frame rate          avg %.1f fps, min %.1f fps over %d 1-s windows (%d below 29)\n",
            sum / n, mn, n, below);
    {
        uint64_t sp[4];
        psp_display_flip_spacing(sp);
        fprintf(stderr, "  flip spacing        1 vblank: %llu, 2: %llu, 3: %llu, 4+: %llu\n",
                (unsigned long long)sp[0], (unsigned long long)sp[1], (unsigned long long)sp[2], (unsigned long long)sp[3]);
        uint64_t fb[6];
        psp_display_frame_busy(fb);
        fprintf(stderr, "  frame busy time     <8ms %llu, <16.7 %llu, <25 %llu, <33.3 %llu, <50 %llu, 50+ %llu\n",
                (unsigned long long)fb[0], (unsigned long long)fb[1], (unsigned long long)fb[2],
                (unsigned long long)fb[3], (unsigned long long)fb[4], (unsigned long long)fb[5]);
    }
    /* For PSP2I_GE_DUMP_FLIP / --watch-from-flip, which count flips. */
    fprintf(stderr, "  display flips       %llu at vblank %llu\n",
            (unsigned long long)psp_display_flips(), (unsigned long long)psp_sched_vblank_count());
    fprintf(stderr, "  host time          GE lists %.1f%%, of which rasterizing %.1f%%; idle %.1f%%\n",
            100.0 * psp_ge_host_us() / 1e6 / run, 100.0 * psp_gpu_host_us() / 1e6 / run,
            100.0 * psp_sched_idle_us() / 1e6 / run);
}

static uint32_t g_watch_late_addr;
static uint64_t g_watch_late_flip;
/* --find-word VALUE VBLANK: list every RAM word equal to VALUE at that vblank
 * (to find where the game keeps a pointer before watching it). */
static uint32_t g_find_value;
static uint64_t g_find_vblank;
static void find_word(void) {
    int n = 0;
    for (uint32_t off = 0; off + 4 <= PSP_RAM_SIZE && n < 64; off += 4) {
        uint32_t w;
        memcpy(&w, psp_mem.ram + off, 4);
        if (w == g_find_value) {
            fprintf(stderr, "find-word: 0x%08X at 0x%08X\n", w, PSP_RAM_BASE + off);
            n++;
        }
    }
    fprintf(stderr, "find-word: %d match(es) for 0x%08X at vblank %llu\n", n, g_find_value,
            (unsigned long long)psp_sched_vblank_count());
}

static void exe_dir(char *out, size_t cap);

/* F12: the displayed frame as dumps/frame_<vblank>.ppm and every draw of the
 * next frame as dumps/draws_<vblank>.txt, next to the exe. */
static void take_dump(uint64_t vb) {
    char dir[600], path[700];
    exe_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/dumps", dir);
    host_mkdir(path);
    snprintf(path, sizeof path, "%s/dumps/frame_%06llu.ppm", dir, (unsigned long long)vb);
    psp_display_capture(path);
    fprintf(stderr, "dump: F12 at vblank %llu -> %s\n", (unsigned long long)vb, path);
    snprintf(path, sizeof path, "%s/dumps/draws_%06llu.txt", dir, (unsigned long long)vb);
    psp_gpu_dump_next_frame(path);
}

/* --profile-from / --profile-to VBLANK: sample only inside that window.
 * Never before the first vblank: the call-stack unwinder takes the loader's
 * function-table lock, and suspending the game thread while it loads a DLL
 * (D3D, SDL, FFmpeg all load before the scheduler starts) deadlocks. */
static volatile LONG g_prof_on = 0;
static uint64_t g_prof_from, g_prof_to;

static void on_vblank(void) {
    uint64_t vb = psp_sched_vblank_count();
    g_prof_on = vb >= g_prof_from && (!g_prof_to || vb < g_prof_to);
    if (g_dump_key) { g_dump_key = 0; take_dump(vb); }
    if (g_find_vblank && vb == g_find_vblank) find_word();
    if (g_watch_late_addr && psp_display_flips() >= g_watch_late_flip) {
        /* --watch-from-flip: arm the write watch only once the game is there. */
        psp_mem_watch_write(g_watch_late_addr);
        g_watch_late_addr = 0;
    }
    apply_script(vb);
    poll_controllers();
    menu_step();
    update_camera_stick(vb);
    online_poll();
    if (g_login.state != LOGIN_CLOSED) {
        static int was_open;
        if (!was_open) {
            /* Headless tests only: pre-fill the fields (never logged). */
            const char *u = getenv("PSP2I_TEST_LOGIN_USER"), *pw = getenv("PSP2I_TEST_LOGIN_PASS");
            if (u) snprintf(g_login.user, sizeof g_login.user, "%s", u);
            if (pw) snprintf(g_login.pass, sizeof g_login.pass, "%s", pw);
            if (u && pw) g_login.sel = LOGIN_BTN_SIGNIN;
        }
        was_open = 1;
        uint32_t psp = g_script_bits | g_pad_bits;
        uint8_t ax = g_pad_ax, ay = g_pad_ay;
        if (g_script_stick >= 0) { ax = g_stick[g_script_stick].x; ay = g_stick[g_script_stick].y; }
        const uint32_t held = menu_inputs_from_psp(psp & 0xFFFF, ax, ay) | g_login_keys;
        switch (login_update(&g_login, held, psp)) {
        case LOGIN_ACT_SUBMIT:   online_login_submit(); break;
        case LOGIN_ACT_CANCEL:   online_login_cancel(); break;
        case LOGIN_ACT_CLOSE_OK: online_login_closed_ok(); break;
        default: break;
        }
        if (g_login.state == LOGIN_CLOSED) { was_open = 0; ctrl_commit(); }
    }
    fps_meter_sample(&g_fpsm, psp_sched_now_us(), psp_display_flips());
    sample_fps(vb);
    /* The picture for the window. With D3D11 and the display buffer ahead of
     * VRAM it comes from an asynchronous copy (d3d11_present: no stall, at
     * most a vblank or two late); otherwise from VRAM. Done headless too, so
     * benchmarks pay what a windowed run pays. Captures and F12 read VRAM,
     * which psp_mem_ptr's access hook keeps exact. */
    int have_frame = 0;
    {
        uint32_t addr, stride, fmt;
        psp_display_get(&addr, &stride, &fmt);
#ifdef _WIN32
        const int p = addr ? d3d11_present(addr, stride, (int)fmt, g_frame, SCREEN_W, SCREEN_H) : 0;
        if (p == 1) have_frame = 1;
        if (p == 1 && getenv("PSP2I_PRESENT_VERIFY")) {
            /* The asynchronous picture against VRAM made current (the access
             * hook reads the same target back): they must match exactly. */
            static uint32_t keep[SCREEN_W * SCREEN_H];
            static uint64_t frames, bad_frames, bad_px;
            memcpy(keep, g_frame, sizeof keep);
            grab_frame();
            uint64_t bad = 0;
            for (int i = 0; i < SCREEN_W * SCREEN_H; i++) bad += keep[i] != g_frame[i];
            frames++; if (bad) { bad_frames++; bad_px += bad; }
            if (frames % 600 == 0 || (bad && bad_frames <= 5))
                fprintf(stderr, "present-verify: %llu frames, %llu differ (%llu pixels)\n",
                        (unsigned long long)frames, (unsigned long long)bad_frames, (unsigned long long)bad_px);
        }
        else if (p == 0 && !g_headless) have_frame = grab_frame();
#endif
    }
#ifdef _WIN32
    if (!g_headless) {
        if (have_frame) g_have_frame = 1;
        if (g_have_frame) {
            compose();
            if (!present_active() || present_frame(g_rgba, SCREEN_W, SCREEN_H) != 0) InvalidateRect(g_wnd, NULL, FALSE);
        }
        if ((vb % 30) == 0) {
            char title[160];
            snprintf(title, sizeof title, "PSP2i (recompiled) - vblank %llu, GE cmds %llu",
                     (unsigned long long)vb, (unsigned long long)psp_ge_command_count());
            SetWindowTextA(g_wnd, title);
        }
        window_pump();
    }
#endif
    for (int i = 0; i < g_noverlay_shot; i++) {
        if (g_overlay_shot[i] != vb || !grab_frame()) continue;
        compose();
        char path[700];
        snprintf(path, sizeof path, "%s/overlay_%06llu.ppm", g_capture_dir, (unsigned long long)vb);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
            for (int k = 0; k < SCREEN_W * SCREEN_H; k++) {
                const uint8_t rgb[3] = { (uint8_t)(g_rgba[k] >> 16), (uint8_t)(g_rgba[k] >> 8), (uint8_t)g_rgba[k] };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
            fprintf(stderr, "overlay-shot: vblank %llu -> %s\n", (unsigned long long)vb, path);
        }
    }
    if (g_capture_every && vb % g_capture_every == 0) {
        char path[700];
        snprintf(path, sizeof path, "%s/frame_%06llu.ppm", g_capture_dir, (unsigned long long)vb);
        psp_display_capture(path);
        /* PSP2I_CAPTURE_FB=ADDR: also capture a 512-wide 8888 buffer at ADDR
         * (e.g. a game's off-screen scene target) as fb_<vblank>.ppm. */
        static const char *fbenv = (const char *)1;
        if (fbenv == (const char *)1) fbenv = getenv("PSP2I_CAPTURE_FB");
        if (fbenv) {
            const uint32_t a = (uint32_t)strtoul(fbenv, NULL, 0);
            const psp_gpu_backend *be = psp_gpu_get_backend();
            if (be) be->sync_vram(a, 272u * 512u * 4u);
            snprintf(path, sizeof path, "%s/fb_%06llu.ppm", g_capture_dir, (unsigned long long)vb);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n480 272\n255\n");
                for (uint32_t y = 0; y < 272; y++)
                    for (uint32_t x = 0; x < 480; x++) {
                        uint32_t p = psp_read32(a + (y * 512u + x) * 4u);
                        uint8_t rgb[3] = { (uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16) };
                        fwrite(rgb, 1, 3, f);
                    }
                fclose(f);
            }
        }
    }
    if (g_seconds > 0 && psp_sched_now_us() >= (uint64_t)(g_seconds * 1e6)) psp_request_exit();
}

/* ---- report ---------------------------------------------------------------- */

#ifdef _WIN32
static void prof_report(void);
#endif
static void report(void) {
    fprintf(stderr, "\n==== psp2i report ====\n");
    fprintf(stderr, "  run time            %.2f s\n", psp_sched_now_us() / 1e6);
    report_fps();
    camera_report(stderr);
#ifdef _WIN32
    d3d11_report(stderr);
#endif
#ifdef _WIN32
    prof_report();
#endif
    fprintf(stderr, "  vblanks             %llu\n", (unsigned long long)psp_sched_vblank_count());
    fprintf(stderr, "  framebuffer         0x%08X\n", psp_display_framebuffer());
    fprintf(stderr, "  GE commands         %llu\n", (unsigned long long)psp_ge_command_count());
    fprintf(stderr, "  GE vertices         %llu\n", (unsigned long long)psp_ge_vertex_count());
    fprintf(stderr, "  GE pixels written   %llu\n", (unsigned long long)psp_ge_pixels());
    fprintf(stderr, "  bytes read (sceIo)  %llu\n", (unsigned long long)psp_io_bytes_read());
    fprintf(stderr, "  audio buffers       %llu\n", (unsigned long long)psp_audio_blocks());
    fprintf(stderr, "  bad memory accesses %llu\n", (unsigned long long)psp_mem_bad_access);
    fprintf(stderr, "  VFPU traps          %llu\n", (unsigned long long)psp_vfpu_trap_count());
    fprintf(stderr, "  untranslated sites  %d\n", g_unimpl_n);
    for (int i = 0; i < g_unimpl_n && i < 40; i++)
        fprintf(stderr, "    0x%08X %-10s x%llu\n", g_unimpl[i].addr, g_unimpl[i].what,
                (unsigned long long)g_unimpl[i].hits);
    oracle_report(stderr);
    psp_hle_dump_calls(stderr, 30);
    psp_ge_dump_stats(stderr);
    psp_hle_dump_unimplemented(stderr);
    psp_sched_dump(stderr);
}

/* ---- sampling profiler (--profile) -------------------------------------------------
 *
 * A background thread suspends the game thread about every millisecond and
 * records its instruction pointer; at exit the samples are resolved to
 * function and source line through the PDB. Coarse, but it measures where the
 * time actually goes without instrumenting anything. */
#ifdef _WIN32
#define PROF_SLOTS (1 << 16)
static struct { DWORD64 ip; uint32_t n; } g_prof[PROF_SLOTS];
static volatile LONG g_prof_run;
static uint64_t g_prof_total;
static HANDLE g_prof_target;

/* Call stacks (x64 unwind data; the thread is suspended). Recompiled code
 * calls recompiled code directly, so the host stack is the game's call stack
 * too: "inclusive" counts every function on the stack once per sample (a
 * game function's whole subtree), and "outside the exe" attributes samples
 * that land in kernel waits, the D3D driver or the CRT to the nearest caller
 * in the exe -- which is what turns "ZwWaitForSingleObject 18%" into a
 * function of ours. */
#define PROF_DEPTH 96
typedef struct { DWORD64 ip; uint32_t n; } prof_slot;
static prof_slot g_prof_incl[PROF_SLOTS], g_prof_wait[PROF_SLOTS];
/* Leaf function and its caller, as (leaf ip, caller ip) pairs: who calls the
 * hot leaves (a CRT math routine says little without its caller). */
static struct { DWORD64 leaf, caller, caller2; uint32_t n; } g_prof_pair[PROF_SLOTS];

static void prof_pair(DWORD64 leaf, DWORD64 caller, DWORD64 caller2) {
    uint32_t h = (uint32_t)(((leaf >> 2) ^ (caller * 31) ^ (caller2 * 131)) * 2654435761u) & (PROF_SLOTS - 1);
    for (int k = 0; k < 64; k++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (g_prof_pair[h].leaf == leaf && g_prof_pair[h].caller == caller && g_prof_pair[h].caller2 == caller2) { g_prof_pair[h].n++; return; }
        if (!g_prof_pair[h].leaf) {
            g_prof_pair[h].leaf = leaf; g_prof_pair[h].caller = caller; g_prof_pair[h].caller2 = caller2;
            g_prof_pair[h].n = 1; return;
        }
    }
}
static DWORD64 g_exe_lo, g_exe_hi;

static void prof_count(DWORD64 ip, prof_slot *t) {
    uint32_t h = (uint32_t)((ip >> 2) * 2654435761u) & (PROF_SLOTS - 1);
    for (int k = 0; k < 64; k++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (t[h].ip == ip) { t[h].n++; return; }
        if (!t[h].ip) { t[h].ip = ip; t[h].n = 1; return; }
    }
}

static void prof_stack(CONTEXT *c) {
    DWORD64 seen[PROF_DEPTH];
    int nseen = 0, outside = c->Rip < g_exe_lo || c->Rip >= g_exe_hi;
    const DWORD64 leaf = c->Rip;
    DWORD64 caller1 = 0;
    for (int d = 0; d < PROF_DEPTH && c->Rip; d++) {
        const DWORD64 ip = c->Rip;
        if (ip >= g_exe_lo && ip < g_exe_hi) {
            if (outside) { prof_count(ip, g_prof_wait); outside = 0; }
            int dup = 0;
            for (int k = 0; k < nseen && !dup; k++) dup = seen[k] == ip;
            if (!dup) { seen[nseen++] = ip; prof_count(ip, g_prof_incl); }
        }
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ip, &base, NULL);
        if (!fe) {                                 /* leaf: return address on top */
            c->Rip = *(DWORD64 *)c->Rsp;
            c->Rsp += 8;
        } else {
            void *hd = NULL;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ip, fe, c, &hd, &est, NULL);
        }
        if (d == 0 && c->Rip != ip) caller1 = c->Rip;
        if (d == 1 && caller1) prof_pair(leaf, caller1, c->Rip != ip ? c->Rip : 0);
        if (c->Rip == ip) break;
    }
}

static DWORD WINAPI prof_thread(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    while (g_prof_run) {
        Sleep(1);
        if (!g_prof_on) continue;
        if (SuspendThread(g_prof_target) == (DWORD)-1) continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(g_prof_target, &ctx)) {
            DWORD64 ip = ctx.Rip;
            uint32_t h = (uint32_t)((ip >> 2) * 2654435761u) & (PROF_SLOTS - 1);
            for (int k = 0; k < 64; k++, h = (h + 1) & (PROF_SLOTS - 1)) {
                if (g_prof[h].ip == ip) { g_prof[h].n++; break; }
                if (!g_prof[h].ip) { g_prof[h].ip = ip; g_prof[h].n = 1; break; }
            }
            g_prof_total++;
            prof_stack(&ctx);
        }
        ResumeThread(g_prof_target);
    }
    timeEndPeriod(1);
    return 0;
}

static void prof_start(void) {
    {
        const uint8_t *b = (const uint8_t *)GetModuleHandleA(NULL);
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
        g_exe_lo = (DWORD64)(uintptr_t)b;
        g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_prof_target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_prof_run = 1;
    CreateThread(NULL, 0, prof_thread, NULL, 0, NULL);
}

typedef struct { char name[160]; uint32_t n; } prof_row;
static int prof_cmp(const void *a, const void *b) {
    return (int)((const prof_row *)b)->n - (int)((const prof_row *)a)->n;
}

static void prof_report(void) {
    if (!g_prof_run) return;
    g_prof_run = 0;
    Sleep(5);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, NULL, TRUE);
    static prof_row fn[8192], ln[8192];
    int nfn = 0, nln = 0;
    for (int i = 0; i < PROF_SLOTS; i++) {
        if (!g_prof[i].ip) continue;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 d = 0;
        char name[160] = "?", line[160] = "?";
        if (SymFromAddr(proc, g_prof[i].ip, &d, si)) snprintf(name, sizeof name, "%s", si->Name);
        IMAGEHLP_LINE64 L;
        DWORD dl = 0;
        memset(&L, 0, sizeof L);
        L.SizeOfStruct = sizeof L;
        if (SymGetLineFromAddr64(proc, g_prof[i].ip, &dl, &L)) {
            const char *f = strrchr(L.FileName, '\\');
            snprintf(line, sizeof line, "%s:%lu (%s)", f ? f + 1 : L.FileName, (unsigned long)L.LineNumber, name);
        } else snprintf(line, sizeof line, "%s", name);
        int k;
        for (k = 0; k < nfn && strcmp(fn[k].name, name); k++) {}
        if (k == nfn && nfn < 8192) { snprintf(fn[nfn].name, sizeof fn[nfn].name, "%s", name); fn[nfn++].n = 0; }
        if (k < 8192) fn[k].n += g_prof[i].n;
        for (k = 0; k < nln && strcmp(ln[k].name, line); k++) {}
        if (k == nln && nln < 8192) { snprintf(ln[nln].name, sizeof ln[nln].name, "%s", line); ln[nln++].n = 0; }
        if (k < 8192) ln[k].n += g_prof[i].n;
    }
    qsort(fn, (size_t)nfn, sizeof fn[0], prof_cmp);
    qsort(ln, (size_t)nln, sizeof ln[0], prof_cmp);
    fprintf(stderr, "  profile: %llu samples\n  top functions:\n", (unsigned long long)g_prof_total);
    const char *topenv = getenv("PSP2I_PROF_TOP");              /* list length, default 25 */
    const int top = topenv && atoi(topenv) > 0 ? atoi(topenv) : 25;
    for (int i = 0; i < nfn && i < top; i++)
        fprintf(stderr, "    %5.1f%%  %s\n", 100.0 * fn[i].n / (double)g_prof_total, fn[i].name);
    fprintf(stderr, "  top lines:\n");
    for (int i = 0; i < nln && i < 40; i++)
        fprintf(stderr, "    %5.1f%%  %s\n", 100.0 * ln[i].n / (double)g_prof_total, ln[i].name);

    /* The stack tables, by function. */
    for (int pass = 0; pass < 2; pass++) {
        const prof_slot *t = pass ? g_prof_wait : g_prof_incl;
        nfn = 0;
        for (int i = 0; i < PROF_SLOTS; i++) {
            if (!t[i].ip) continue;
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 d = 0;
            char name[160] = "?";
            if (SymFromAddr(proc, t[i].ip, &d, si)) snprintf(name, sizeof name, "%s", si->Name);
            int k;
            for (k = 0; k < nfn && strcmp(fn[k].name, name); k++) {}
            if (k == nfn && nfn < 8192) { snprintf(fn[nfn].name, sizeof fn[nfn].name, "%s", name); fn[nfn++].n = 0; }
            if (k < 8192) fn[k].n += t[i].n;
        }
        qsort(fn, (size_t)nfn, sizeof fn[0], prof_cmp);
        fprintf(stderr, pass ? "  outside the exe (waits, driver, CRT), by nearest caller in the exe:\n"
                             : "  inclusive (function and everything it calls):\n");
        for (int i = 0; i < nfn && i < (pass ? 25 : 3 * top); i++)
            fprintf(stderr, "    %5.1f%%  %s\n", 100.0 * fn[i].n / (double)g_prof_total, fn[i].name);
    }

    /* Leaf <- caller, by function names. */
    nfn = 0;
    for (int i = 0; i < PROF_SLOTS; i++) {
        if (!g_prof_pair[i].leaf) continue;
        char names[3][160] = { "?", "?", "?" };
        for (int w = 0; w < 3; w++) {
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 d = 0;
            const DWORD64 a = w == 0 ? g_prof_pair[i].leaf : w == 1 ? g_prof_pair[i].caller : g_prof_pair[i].caller2;
            if (a && SymFromAddr(proc, a, &d, si)) snprintf(names[w], 160, "%s", si->Name);
        }
        char name[160];
        snprintf(name, sizeof name, "%.48s <- %.48s <- %.48s", names[0], names[1], names[2]);
        int k;
        for (k = 0; k < nfn && strcmp(fn[k].name, name); k++) {}
        if (k == nfn && nfn < 8192) { snprintf(fn[nfn].name, sizeof fn[nfn].name, "%s", name); fn[nfn++].n = 0; }
        if (k < 8192) fn[k].n += g_prof_pair[i].n;
    }
    qsort(fn, (size_t)nfn, sizeof fn[0], prof_cmp);
    fprintf(stderr, "  leaf <- caller:\n");
    for (int i = 0; i < nfn && i < 40; i++)
        fprintf(stderr, "    %5.1f%%  %s\n", 100.0 * fn[i].n / (double)g_prof_total, fn[i].name);
}
#endif

/* The game functions on the host stack, innermost first, as PSP addresses:
 * recompiled functions call each other as C functions, so the host stack is
 * the game's call stack. "08B35014 < 08A88584 < ...". For logs that say
 * where in the game something happens (online.c's HTTP log). */
#ifdef _WIN32
int game_stack(char *out, size_t cap, int max_frames) {
    static int sym_ready;
    HANDLE proc = GetCurrentProcess();
    if (!sym_ready) { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(proc, NULL, TRUE); sym_ready = 1; }
    void *frames[48];
    const USHORT n = CaptureStackBackTrace(1, 48, frames, NULL);
    size_t used = 0;
    int k = 0;
    uint32_t last = 0, seen[48];
    out[0] = '\0';
    for (USHORT i = 0; i < n && k < max_frames; i++) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 d = 0;
        if (!SymFromAddr(proc, (DWORD64)(uintptr_t)frames[i], &d, si)) continue;
        if (!strcmp(si->Name, "thread_body")) break;
        unsigned a;
        if ((sscanf(si->Name, "psp_body_%8X", &a) == 1 || sscanf(si->Name, "psp_func_%8X", &a) == 1) && a != last) {
            /* each function once: a state machine re-entering itself would
             * otherwise fill the chain and hide its callers */
            int dup = 0;
            for (int j = 0; j < k && !dup; j++) dup = seen[j] == a;
            last = a;
            if (dup) continue;
            if (k < 48) seen[k] = a;
            used += (size_t)snprintf(out + used, used < cap ? cap - used : 0, "%s%08X", k ? " < " : "", a);
            k++;
        }
    }
    return k;
}

/* --watch hits in a release build: name the recompiled functions on the host
 * stack (the host stack is the game's call stack), innermost first. */
static void watch_backtrace(uint32_t addr, uint32_t value) {
    (void)addr; (void)value;
    static int sym_ready;
    HANDLE proc = GetCurrentProcess();
    if (!sym_ready) { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(proc, NULL, TRUE); sym_ready = 1; }
    void *frames[24];
    const USHORT n = CaptureStackBackTrace(1, 24, frames, NULL);
    fprintf(stderr, "    stack:");
    for (USHORT i = 0; i < n; i++) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 d = 0;
        if (SymFromAddr(proc, (DWORD64)(uintptr_t)frames[i], &d, si)) {
            if (!strncmp(si->Name, "psp_", 4) && strcmp(si->Name, "psp_write_f32") && strcmp(si->Name, "psp_write32"))
                fprintf(stderr, " %s", si->Name);
            if (!strcmp(si->Name, "thread_body")) break;
        }
    }
    fprintf(stderr, "\n");
}
#endif

/* ---- crash reporting ---------------------------------------------------------- */

#ifdef _WIN32
#include <dbghelp.h>

/* A host-side fault (access violation, stack overflow) is a bug in the
 * runtime or the generated code. Report where -- host function, PSP state,
 * thread table -- before the process dies, so it can be found. */
static char g_crash_path[700];          /* crash_log.txt next to the exe */
static DWORD g_crash_tid;               /* the faulting thread */

/* The faulting thread's own call stack, innermost first: for a stack overflow
 * (endless recursion) the first few dozen frames show the cycle. */
static void crash_walk_stack(HANDLE proc, CONTEXT ctx) {
    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, g_crash_tid);
    STACKFRAME64 sf;
    memset(&sf, 0, sizeof sf);
    sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rsp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    fprintf(stderr, "  call stack (innermost first):\n");
    for (int i = 0; i < 60; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th ? th : GetCurrentThread(), &sf, &ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL) || !sf.AddrPC.Offset)
            break;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        const unsigned long long rva = sf.AddrPC.Offset - (DWORD64)(uintptr_t)GetModuleHandleA(NULL);
        if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, si))
            fprintf(stderr, "    #%-2d %s+0x%llx  (exe+0x%llx)\n", i, si->Name, (unsigned long long)disp, rva);
        else
            fprintf(stderr, "    #%-2d exe+0x%llx\n", i, rva);
    }
    if (th) CloseHandle(th);
}

static void crash_report(EXCEPTION_POINTERS *ep) {
    /* The report also goes to crash_log.txt: run.bat shows stderr only in the
     * console, which closes with the game. */
    if (g_crash_path[0]) {
        fprintf(stderr, "\n==== host exception: report written to %s ====\n", g_crash_path);
        if (freopen(g_crash_path, "a", stderr)) {
            time_t now = time(NULL);
            fprintf(stderr, "\n==== crash %s", ctime(&now));
        }
    }
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    fprintf(stderr, "\n==== host exception 0x%08lX at %p ====\n",
            (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, "  %s of %p\n", er->ExceptionInformation[0] ? "write" : "read",
                (void *)er->ExceptionInformation[1]);
    HANDLE proc = GetCurrentProcess();
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    fprintf(stderr, "  exe base %p, fault at exe+0x%llx\n", (void *)base,
            (unsigned long long)((uintptr_t)er->ExceptionAddress - base));
    /* The game-stack logger may already have initialised the symbol handler
     * (a second SymInitialize fails): use it either way. */
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, NULL, TRUE);
    {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, (DWORD64)(uintptr_t)er->ExceptionAddress, &disp, si))
            fprintf(stderr, "  in %s+0x%llx\n", si->Name, (unsigned long long)disp);
        crash_walk_stack(proc, *ep->ContextRecord);
    }
    fprintf(stderr, "  last recompiled function entered: 0x%08X\n", psp_trace_last());
    for (int i = 0; i < 32; i += 4)
        fprintf(stderr, "  %-4s 0x%08X  %-4s 0x%08X  %-4s 0x%08X  %-4s 0x%08X\n",
                psp_reg_names[i], psp_cpu.r[i], psp_reg_names[i + 1], psp_cpu.r[i + 1],
                psp_reg_names[i + 2], psp_cpu.r[i + 2], psp_reg_names[i + 3], psp_cpu.r[i + 3]);
    psp_sched_dump(stderr);
    fflush(stderr);
}

static DWORD WINAPI crash_report_thread(LPVOID p) { crash_report((EXCEPTION_POINTERS *)p); return 0; }

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    static int once;
    if (once++) return EXCEPTION_CONTINUE_SEARCH;
    g_crash_tid = GetCurrentThreadId();
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        /* No stack is left to report on (the handler itself used to die silently):
         * do it on a fresh thread while this one waits. */
        HANDLE t = CreateThread(NULL, 1u << 20, crash_report_thread, ep, 0, NULL);
        if (t) { WaitForSingleObject(t, 60000); CloseHandle(t); }
    } else {
        crash_report(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* ---- main ------------------------------------------------------------------ */

static void exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    GetModuleFileNameA(NULL, out, (DWORD)cap);
    char *s = strrchr(out, '\\');
    if (s) *s = '\0';
#else
    snprintf(out, cap, ".");
#endif
}

static int exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

int main(int argc, char **argv) {
    char dir[512], root[600] = "", eboot[700] = "", ms_dir[600] = "";   /* --ms: memory stick elsewhere (tests) */
    uint32_t oracle_addr = 0;
    const char *renderer = "d3d11";
    int res_arg = -1;                       /* --resolution; else psp2i_display.ini */
    int oracle_all = 0;
    int want_sdl = -1;                     /* -1: default (with a window) */
    int want_audio = -1;
    int fps = 30;
    int show_fps = 1;                      /* the FPS counter: shown unless --hide-fps */
    float rs_speed = 1.0f;                 /* right-stick camera sensitivity */
    uint32_t dump_addr = 0, args_addr = 0;
    uint64_t args_flip = 0;
    const char *dump_path = NULL;
    exe_dir(dir, sizeof dir);
#ifdef _WIN32
    snprintf(g_crash_path, sizeof g_crash_path, "%s\\crash_log.txt", dir);
#endif
#ifdef PSP2I_PROD
    argc = 1;                              /* community build: no command line, see prod.c */
#endif

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--root") && i + 1 < argc)          snprintf(root, sizeof root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--eboot") && i + 1 < argc)         snprintf(eboot, sizeof eboot, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--ms") && i + 1 < argc)            snprintf(ms_dir, sizeof ms_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--headless"))                      g_headless = 1;
        else if (!strcmp(argv[i], "--sdl"))                           want_sdl = 1;
        else if (!strcmp(argv[i], "--no-sdl"))                        want_sdl = 0;
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)           fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--show-fps"))                      show_fps = 1;
        else if (!strcmp(argv[i], "--rs-speed") && i + 1 < argc)      rs_speed = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--rstick") && i + 1 < argc && g_nrstick < MAX_STICK) {
            unsigned x, y; unsigned long long at = 0, len = 6;
            if (sscanf(argv[++i], "%u,%u@%llu+%llu", &x, &y, &at, &len) < 3 || x > 255 || y > 255) {
                fprintf(stderr, "bad --rstick %s (want e.g. 255,128@4000+60)\n", argv[i]); return 1;
            }
            g_rstick[g_nrstick].x = (uint8_t)x; g_rstick[g_nrstick].y = (uint8_t)y;
            g_rstick[g_nrstick].at = at; g_rstick[g_nrstick].len = len;
            g_nrstick++;
        }
        else if (!strcmp(argv[i], "--hide-fps"))                      show_fps = 0;
        else if (!strcmp(argv[i], "--overlay-shot") && i + 1 < argc && g_noverlay_shot < MAX_OVERLAY_SHOT)
            g_overlay_shot[g_noverlay_shot++] = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--menu-key") && i + 1 < argc) {
            const char *k = argv[++i];
            const int n = (k[0] == 'F' || k[0] == 'f') ? atoi(k + 1) : 0;
            /* F10 is a system key (menu bar) and F12 takes dumps. */
            if (n < 1 || n > 11 || n == 10) { fprintf(stderr, "bad --menu-key %s (want F1..F9 or F11)\n", k); return 1; }
            snprintf(g_menu_key_name, sizeof g_menu_key_name, "F%d", n);
#ifdef _WIN32
            g_menu_vk = (WPARAM)(VK_F1 + n - 1);
#endif
        }
        else if (!strcmp(argv[i], "--audio"))                         want_audio = 1;
        else if (!strcmp(argv[i], "--no-audio"))                      want_audio = 0;
        else if (!strcmp(argv[i], "--log-floats") && i + 3 < argc) {
            g_logf_addr = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            g_logf_count = (uint32_t)strtoul(argv[i + 2], NULL, 0);
            g_logf_every = (uint32_t)strtoul(argv[i + 3], NULL, 0);
            if (g_logf_count > 16) g_logf_count = 16;
            i += 3;
        }
        else if (!strcmp(argv[i], "--stick") && i + 1 < argc) {
            if (add_stick(argv[++i]) != 0) { fprintf(stderr, "bad --stick %s (want e.g. 255,128@4000+60)\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--dump-ram-at") && i + 2 < argc && g_nramdump < MAX_RAMDUMP) {
            g_ramdump[g_nramdump].at = strtoull(argv[i + 1], NULL, 0);
            snprintf(g_ramdump[g_nramdump].path, sizeof g_ramdump[g_nramdump].path, "%s", argv[i + 2]);
            g_nramdump++;
            i += 2;
        }
        else if (!strcmp(argv[i], "--find-word") && i + 2 < argc) {
            g_find_value = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            g_find_vblank = strtoull(argv[i + 2], NULL, 0);
            i += 2;
        }
        else if (!strcmp(argv[i], "--watch-from-flip") && i + 2 < argc) {
            g_watch_late_addr = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            g_watch_late_flip = strtoull(argv[i + 2], NULL, 0);
            i += 2;
        }
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc)      renderer = argv[++i];
        else if (!strcmp(argv[i], "--resolution") && i + 1 < argc) {
            res_arg = menu_res_parse(argv[++i]);
            if (res_arg < 0) { fprintf(stderr, "bad --resolution %s (want e.g. 1920x1080 or fullscreen)\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) {
            if (add_press(argv[++i]) != 0) { fprintf(stderr, "bad --press %s (want e.g. cross@600+6)\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--oracle-all"))                    oracle_all = 1;
#ifdef _WIN32
        else if (!strcmp(argv[i], "--profile"))                       prof_start();
        else if (!strcmp(argv[i], "--profile-from") && i + 1 < argc)  g_prof_from = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--profile-to") && i + 1 < argc)    g_prof_to = strtoull(argv[++i], NULL, 0);
#endif
        else if (!strcmp(argv[i], "--label-ring"))                    ring_arm();
        else if (!strcmp(argv[i], "--dump-on-bad") && i + 1 < argc)   dump_on_bad_access(argv[++i]);
        else if (!strcmp(argv[i], "--dump-at") && i + 2 < argc)       { dump_addr = (uint32_t)strtoul(argv[i + 1], NULL, 0); dump_path = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--args-from-flip") && i + 2 < argc) {
            args_addr = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            args_flip = strtoull(argv[i + 2], NULL, 0);
            i += 2;
        }
        else if (!strcmp(argv[i], "--oracle") && i + 1 < argc)        oracle_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc)         psp_mem_watch_write((uint32_t)strtoul(argv[++i], NULL, 0));
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)       g_seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--capture-every") && i + 1 < argc) g_capture_every = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capture-dir") && i + 1 < argc)   snprintf(g_capture_dir, sizeof g_capture_dir, "%s", argv[++i]);
        else { fprintf(stderr, "usage: %s [--root DIR] [--eboot FILE] [--headless] [--seconds N] "
                               "[--capture-every N] [--capture-dir DIR]\n", argv[0]); return 2; }
    }
#ifdef PSP2I_PROD
    if (prod_check(dir, root, sizeof root, eboot, sizeof eboot) != 0) return 1;
#endif
    if (!root[0]) {
        snprintf(root, sizeof root, "%s/GameData", dir);
        char probe[700];
        snprintf(probe, sizeof probe, "%s/disc/PSP_GAME/PARAM.SFO", root);
        if (!exists(probe)) snprintf(root, sizeof root, "GameData");
    }
    if (!eboot[0]) {
        snprintf(eboot, sizeof eboot, "%s/EBOOT.BIN", dir);
        if (!exists(eboot)) snprintf(eboot, sizeof eboot, "%s/disc/PSP_GAME/SYSDIR/EBOOT.BIN", root);
    }
    if (g_capture_every || g_noverlay_shot) host_mkdir(g_capture_dir);
    {
        char ms[700];
        if (ms_dir[0]) {
            host_mkdir(ms_dir);
            snprintf(ms, sizeof ms, "%s/PSP", ms_dir);          host_mkdir(ms);
            snprintf(ms, sizeof ms, "%s/PSP/SAVEDATA", ms_dir); host_mkdir(ms);
        } else {
            snprintf(ms, sizeof ms, "%s/ms", root);          host_mkdir(ms);
            snprintf(ms, sizeof ms, "%s/ms/PSP", root);      host_mkdir(ms);
            snprintf(ms, sizeof ms, "%s/ms/PSP/SAVEDATA", root); host_mkdir(ms);
        }
    }

#ifdef _WIN32
    SetUnhandledExceptionFilter(on_crash);
    /* The scheduler sleeps until the next vblank or timeout with Sleep(ms - 1)
     * and spins the rest (threadman.c host_sleep_us), which assumes the 1 ms
     * timer resolution. At Windows' default (15.6 ms) one Sleep(1) can take a
     * whole vblank: frames that finished in time were shown a vblank late,
     * costing about 20 fps in the lobby at 60 fps. Only --profile set this
     * before, which is why profiled runs looked faster. */
    timeBeginPeriod(1);
    psp_mem_set_watch_hook(watch_backtrace);
#endif
    printf("root:  %s\neboot: %s\n", root, eboot);

    if (psp_mem_init() != 0) { fprintf(stderr, "psp2i: psp_mem_init failed\n"); return 1; }
    psp_cpu_reset();

    module_info mi;
    if (load_elf(eboot, &mi) != 0) return 1;
    printf("module '%s': entry 0x%08X, gp 0x%08X, image 0x%08X..0x%08X\n",
           mi.name, mi.entry, mi.gp, mi.load_lo, mi.load_hi);

    psp_hle_init();
#ifdef _WIN32
    /* GPU rendering by default; the software rasterizer is the fallback
     * and the reference (--renderer software). */
    if (strcmp(renderer, "software") != 0 && d3d11_init() != 0)
        fprintf(stderr, "psp2i: falling back to the software renderer\n");
#endif
    psp_io_set_root(root);
    {
        const char *e = getenv("PSP2I_VFPU_NOINLINE");     /* A/B: runtime VFPU calls only */
        if (e && *e == '1') { psp_vfpu_force_runtime(1); printf("vfpu: inline operations disabled (PSP2I_VFPU_NOINLINE=1)\n"); }
    }
    if (ms_dir[0]) { psp_io_set_ms_root(ms_dir); printf("ms:    %s\n", ms_dir); }
    psp_sysmem_set_heap(mi.load_hi, USER_PARTITION_TOP);
    psp_recomp_register();
    if (dump_addr) dump_arm(dump_addr, dump_path);
    if (args_addr) args_arm(args_addr, args_flip);   /* entry hooks fire in trace builds only */
    if (oracle_addr || oracle_all) {
#ifndef PSPRECOMP_TRACE
        fprintf(stderr, "psp2i: --oracle needs a trace build (-DPSP2I_TRACE=ON); ignored\n");
#else
        if (oracle_addr) oracle_arm(oracle_addr, mi.stub_lo, mi.stub_hi);
        if (oracle_all)  oracle_arm_sweep(mi.stub_lo, mi.stub_hi);
#endif
    }

    /* argv[0]: the path the kernel booted the module from, copied into guest
     * memory; module_start receives (length including NUL, pointer). */
    static const char boot_path[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    uint32_t argp = psp_sysmem_alloc(0x100, 1);
    psp_mem_write_block(argp, boot_path, (uint32_t)sizeof boot_path);

    snprintf(g_display_ini, sizeof g_display_ini, "%s/psp2i_display.ini", dir);
    int res = res_arg >= 0 ? res_arg : display_load();
    if (res < 0) res = RES_DEFAULT;
    if (res != RES_FULLSCREEN) g_last_windowed = res;
#ifdef _WIN32
    if (!g_headless) window_open(res);
#endif
    if (want_sdl == 1 || (want_sdl < 0 && !g_headless)) g_sdl_on = input_sdl_init() == 0;
    audio_init(want_audio == 1 || (want_audio < 0 && !g_headless));
    atrac_ffmpeg_init(dir);        /* ATRAC music; silent if FFmpeg is absent */
    menu_init(&g_menu, fps, show_fps);
    g_menu.res = res;
    fps_meter_init(&g_fpsm);
    framerate_init(fps);
    menu_set_rs_speed(&g_menu, rs_speed);
    camera_init(menu_rs_speed(&g_menu));
    /* PSP savedata encryption: saves interchangeable with a PSP / PPSSPP */
    static const psp_savedata_crypto SAVE_CRYPTO = { savecrypt_decrypt, savecrypt_encrypt, savecrypt_sfo_hash };
    psp_savedata_set_crypto(&SAVE_CRYPTO);
    online_init(dir, &g_login);
    gamelog_init(dir);
    psp_sched_set_vblank_hook(on_vblank);

    printf("starting module_start at 0x%08X\n", mi.entry);
    fflush(stdout);
    int rc = psp_sched_run(mi.entry, (uint32_t)sizeof boot_path, argp, 0x20, 0x40000, mi.gp);

    report();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    if (g_sdl_on) input_sdl_shutdown();
    audio_shutdown();
    psp_mem_free();
    return rc;
}
