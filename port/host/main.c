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
 * usage: psp2i [--root DIR] [--iso FILE | --extracted] [--eboot FILE] [--headless]
 *              [--seconds N] [--capture-every N] [--capture-dir DIR]
 *
 *   --root      directory holding flash/ (the PSP fonts), ms/ (memory stick)
 *               and, for an extracted disc, disc/; default: GameData next to
 *               the executable, else ./GameData
 *   --iso       the game's disc image (.iso), read directly; default: the first
 *               *.iso next to the executable, in the root, or in the root's
 *               parent folder
 *   --extracted use the extracted disc in <root>/disc/ even when an ISO is
 *               found (the default when there is none)
 *   --eboot     the decrypted EBOOT.BIN; default: EBOOT.BIN next to the
 *               executable, else the disc's PSP_GAME/SYSDIR/EBOOT.BIN
 *   --headless  no window
 *   --seconds   stop after N seconds of game time and print the report
 *   --capture-every / --capture-dir  dump the framebuffer as PPM every N vblanks
 *   --fps 30|60|120  initial frame rate through the game's own frame-rate API
 *               (120: the emulated display refreshes at 120 Hz; F7 runs the
 *               frame-rate audit, F6 the press audit, rateaudit.c)
 *               (framerate.c); changeable at run time in the settings menu
 *   --menu-key F1..F9|F11  the keyboard key that opens/closes the settings
 *               menu (default F1; the controller's Guide or R3 also do)
 *   --show-fps / --hide-fps  the measured-FPS counter at start (default shown)
 *   --rs-speed 1.0..2.0  right-stick camera sensitivity at start (default
 *               1.0; the settings menu changes it in 0.25 steps)
 *   --rstick X,Y@VBLANK[+N]  scripted right stick (PSP-style bytes), for tests
 *   --replay-input FILE      play back a PSP2I_INPUT_RECORD=FILE recording (same
 *               input on the same game frame; its fps unless --fps)
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

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/vfpu.h>
#include <signal.h>
#include <time.h>
#include <psprecomp/render.h>
#include <psprecomp/net.h>

#include "recomp_funcs.h"
#include "input_sdl.h"
#include "audio_sdl.h"
#include "atrac_at3.h"
#include "h264_openh264.h"
#include "framerate.h"
#include "menu.h"
#include "font_win.h"
#include "web_overlay.h"
#include "camera.h"
#include "login.h"
#include "online.h"
#include "savecrypt.h"
#include "gamelog.h"
#include "functrace.h"
#include "rateaudit.h"
#include "postfx.h"
#include "prod.h"
#include "textedit.h"
#include "present.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  include <dbghelp.h>
#  include <mmsystem.h>
#  include <psapi.h>
#  define host_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define host_mkdir(p) mkdir(p, 0777)
#endif

#define USER_PARTITION_TOP 0x0A000000u
#define SCREEN_W 480
#define SCREEN_H 272

/* The window title: the game's name in the community build; dev builds add
 * their progress to it (see the vblank handler). */
#ifdef PSP2I_PROD
#define WINDOW_TITLE "Phantasy Star Portable 2 Infinity Recompiled"
#else
#define WINDOW_TITLE "PSP2i (recompiled)"
#endif

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
#include "gl_ge.h"
#include "modelx.h"
static void exe_dir(char *out, size_t cap);

/* The renderer (menu RENDERER, psp2i_display.ini renderer=, --renderer):
 * REN_D3D11 / REN_GL draw the GE on the GPU, REN_SOFTWARE is the reference. */
static int g_renderer = REN_D3D11;        /* what runs now */
static int g_gl_window;                   /* OpenGL presents into the window */
static int g_ge_thread;                   /* OpenGL on the GE thread (ge_thread=on/off, --ge-thread; off by default) */
static int g_ge_thread_arg = -1;
static int g_load_models;                 /* LOAD MODELS (load_models=on/off) */

#ifdef _WIN32
/* With the GE thread (OpenGL), the renderer is on that thread, so presenting
 * is work handed to it: each vblank asks it for the displayed frame and
 * takes the newest one it finished (a vblank later than without the thread),
 * and the composed picture goes to it to be drawn and swapped. The game
 * thread only copies pictures. (Showing the picture through present.c's swap
 * chain instead left OpenGL without a swap to pace it: its driver queue grew
 * to ~900 MB and the picture stalled.) */
static SRWLOCK g_pr_lock = SRWLOCK_INIT;
static struct { uint32_t addr, stride; int fmt, w, h; } g_pr_req;
static volatile int g_pr_queued, g_sh_queued;
static uint32_t *g_pr_buf, *g_pr_pub, *g_sh_buf, *g_sh_work;
static int g_pr_result;                    /* the GE thread's last answer: 0 from VRAM, 1 new, 2 keep */
static uint64_t g_pr_seq, g_pr_taken;
static struct { int w, h, x, y, vw, vh, cw, ch; } g_sh_req;

static void present_job(void *p) {
    (void)p;
    AcquireSRWLockExclusive(&g_pr_lock);
    const uint32_t addr = g_pr_req.addr, stride = g_pr_req.stride;
    const int fmt = g_pr_req.fmt, w = g_pr_req.w, h = g_pr_req.h;
    g_pr_queued = 0;
    ReleaseSRWLockExclusive(&g_pr_lock);
    const int r = gl_ge_present(addr, stride, fmt, g_pr_buf, w, h);
    AcquireSRWLockExclusive(&g_pr_lock);
    if (r == 1) { memcpy(g_pr_pub, g_pr_buf, (size_t)w * (size_t)h * 4); g_pr_seq++; }
    g_pr_result = r;
    ReleaseSRWLockExclusive(&g_pr_lock);
}

static int gl_present_mt(uint32_t addr, uint32_t stride, int fmt, uint32_t *out, int w, int h) {
    if (!g_pr_buf) {
        g_pr_buf = (uint32_t *)calloc((size_t)w * h, 4);
        g_pr_pub = (uint32_t *)calloc((size_t)w * h, 4);
        if (!g_pr_buf || !g_pr_pub) return 0;
    }
    AcquireSRWLockExclusive(&g_pr_lock);
    g_pr_req.addr = addr; g_pr_req.stride = stride; g_pr_req.fmt = fmt; g_pr_req.w = w; g_pr_req.h = h;
    const int queue = !g_pr_queued;
    g_pr_queued = 1;
    int r = g_pr_result == 0 ? 0 : 2;
    if (g_pr_seq != g_pr_taken) { memcpy(out, g_pr_pub, (size_t)w * (size_t)h * 4); g_pr_taken = g_pr_seq; r = 1; }
    ReleaseSRWLockExclusive(&g_pr_lock);
    if (queue) psp_ge_call(present_job, NULL, 0);
    return r;
}

static void show_job(void *p) {
    (void)p;
    AcquireSRWLockExclusive(&g_pr_lock);
    const int w = g_sh_req.w, h = g_sh_req.h;
    memcpy(g_sh_work, g_sh_buf, (size_t)w * (size_t)h * 4);
    const int x = g_sh_req.x, y = g_sh_req.y, vw = g_sh_req.vw, vh = g_sh_req.vh, cw = g_sh_req.cw, ch = g_sh_req.ch;
    g_sh_queued = 0;
    ReleaseSRWLockExclusive(&g_pr_lock);
    if (gl_blit_frame(g_sh_work, w, h, x, y, vw, vh, cw, ch) == 0) gl_platform_swap();
}

static void gl_show_mt(const uint32_t *px, int w, int h, int x, int y, int vw, int vh, int cw, int ch) {
    if (!g_sh_buf) {
        g_sh_buf = (uint32_t *)calloc((size_t)w * h, 4);
        g_sh_work = (uint32_t *)calloc((size_t)w * h, 4);
        if (!g_sh_buf || !g_sh_work) return;
    }
    AcquireSRWLockExclusive(&g_pr_lock);
    memcpy(g_sh_buf, px, (size_t)w * (size_t)h * 4);
    g_sh_req.w = w; g_sh_req.h = h; g_sh_req.x = x; g_sh_req.y = y;
    g_sh_req.vw = vw; g_sh_req.vh = vh; g_sh_req.cw = cw; g_sh_req.ch = ch;
    const int queue = !g_sh_queued;
    g_sh_queued = 1;
    ReleaseSRWLockExclusive(&g_pr_lock);
    if (queue) psp_ge_call(show_job, NULL, 0);
}

static void ge_thread_gl_init(void) {
    if (gl_platform_bind() != 0) fprintf(stderr, "gl: the GE thread could not take the context\n");
}
#endif

/* The displayed frame from the GPU renderer's asynchronous copy (1), keep the
 * last picture (2), or read VRAM (0). */
static int gpu_present(uint32_t addr, uint32_t stride, int fmt, uint32_t *out, int w, int h) {
#ifdef _WIN32
    if (g_renderer == REN_GL && psp_ge_threaded()) return gl_present_mt(addr, stride, fmt, out, w, h);
#endif
    if (g_renderer == REN_GL) return gl_ge_present(addr, stride, fmt, out, w, h);
#ifdef _WIN32
    if (g_renderer == REN_D3D11) return d3d11_present(addr, stride, fmt, out, w, h);
#endif
    return 0;
}

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

static int load_elf_buf(uint8_t *b, size_t len, const char *path, module_info *mi);

static int load_elf(const char *path, module_info *mi) {
    size_t len = 0;
    uint8_t *b = read_file(path, &len);
    if (!b) { fprintf(stderr, "psp2i: cannot read %s\n", path); return -1; }
    return load_elf_buf(b, len, path, mi);
}

/* The ELF in memory (taken and freed); `path` names it in messages. */
static int load_elf_buf(uint8_t *b, size_t len, const char *path, module_info *mi) {
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
static int         g_edit_adhoc;            /* the editor is open for: 0 SEGA SERVER, 1 ADHOC SERVER, 2 MODERN SERVER */
static int         g_edit_enter;          /* Enter pressed in the editor: save */

static int text_ui_open(void) { return g_login.state != LOGIN_CLOSED || g_sega_edit.open; }

static void draw_overlay(void) {
    menu_image img = { g_rgba, SCREEN_W, SCREEN_H };
    if (g_menu.show_fps) fps_draw(&g_fpsm, &img);
#ifndef PSP2I_PROD
    {   /* F7's frame-rate audit, while it runs */
        const char *ra = rateaudit_status();
        if (ra[0]) menu_text(&img, 4, SCREEN_H - 22, ra, 0xFFD040u);
    }
#endif
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

static uint64_t g_quit_at;                 /* QUIT GAME chosen (engine menu): 0 = not */
#ifdef _WIN32
static HWND g_wnd;
static int g_alt_enter;                     /* Alt+Enter pressed; handled with the menu */
static uint32_t g_keys;
static WPARAM g_menu_vk = VK_F1;
static volatile LONG g_prof_keyed, g_prof_keyon;   /* F8 with --profile: sample only between presses */
static volatile ULONGLONG g_close_at;   /* when the window was closed (GetTickCount64); 0 = not closed */

/* Keys for the menu only: the hotkey, and Escape/Backspace as Back. */
static uint32_t host_key(WPARAM vk) {
    if (vk == g_menu_vk) return MI_TOGGLE;
    if (vk == VK_ESCAPE || vk == VK_BACK) return MI_BACK;
    return 0;
}

static int g_swap_oc;                       /* Circle <-> Cross (menu CIRCLE/CROSS, swap_circle_cross=) */
static float g_rs_saved;                    /* menu CONTROLS > SENSITIVITY (rs_speed=; 0: not saved yet) */
static int g_rs_on = 1;                     /* menu CONTROLS > RIGHT STICK (right_stick=on/off) */
static int g_deadzone_pct;                  /* menu CONTROLS > STICK DEADZONE (stick_deadzone=, 0 = the game's own) */
static int g_mouse_cam;                     /* menu CONTROLS > MOUSE CAMERA (mouse_camera=on/off) */
static int g_test_ticks, g_test_logic;      /* menu TESTS (test_tick_pacing= / test_30hz_logic=; framerate.c) */
static int g_draw_dist;                     /* menu DRAW DISTANCE (draw_distance=far/normal; switches.txt 0) */
static int g_vertex_arg = -1;               /* --vertex-work gpu|cpu (tests): over the ini */
static int g_gpu_vertex = 1;                /* menu VERTEX WORK (vertex_work=gpu/cpu; psp_gpu_set_gpu_transform) */
static postfx g_post = { PF_NEAREST, 0, PF_OFF, PF_OFF, 10, 10 };   /* menu GRAPHICS post-processing (postfx.h) */

/* Once per vblank: the DRAW DISTANCE switch (port/switches.txt, switch 0). The
 * PPSSPP cheat it comes from is reported to corrupt the UI while saving, so
 * it stays off while the savedata utility runs and for 3 s after. */
static void draw_dist_apply(void) {
    static uint64_t quiet_until;
    const uint64_t now = psp_sched_now_us();
    if (psp_utility_savedata_busy()) quiet_until = now + 3000000u;
    const uint8_t on = g_draw_dist && now >= quiet_until;
    if (psp_switch[0] != on) {
        psp_switch[0] = on;
        fprintf(stderr, "graphics: draw distance %s%s\n", on ? "far" : "normal",
                g_draw_dist && !on ? " (saving)" : "");
    }
}

/* The keys and mouse buttons bound to the PSP's controls (menu CONTROLS > KEYBOARD/MOUSE, key_*= in
 * psp2i_display.ini), KB_* order; 0 = none. Each key is bound to one control.
 * Defaults: arrows the D-pad, S/X/Z/A Triangle/Circle/Cross/Square, Q/W L/R,
 * Enter Start, Space Select, and the left stick on I/K/J/L (as the SDL host). */
static const WPARAM KEY_DEFAULTS[KB_N] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'S', 'X', 'Z', 'A', 'Q', 'W',
                                           VK_RETURN, VK_SPACE, 'I', 'K', 'J', 'L' };
static const char *const KEY_INI[KB_N] = { "up", "down", "left", "right", "triangle", "circle", "cross", "square",
                                           "l", "r", "start", "select", "stick_up", "stick_down", "stick_left", "stick_right" };
static const uint32_t KEY_PSP[KB_STICK_UP] = { 0x0010, 0x0040, 0x0080, 0x0020, 0x1000, 0x2000, 0x4000, 0x8000,
                                                0x0100, 0x0200, 0x0008, 0x0001 };
static WPARAM g_bind[KB_N] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'S', 'X', 'Z', 'A', 'Q', 'W',
                               VK_RETURN, VK_SPACE, 'I', 'K', 'J', 'L' };

/* A key's name, as the menu and the ini show it (the menu font's characters). */
static const struct { WPARAM vk; const char *name; } KEY_NAMES[] = {
    { VK_UP, "UP" }, { VK_DOWN, "DOWN" }, { VK_LEFT, "LEFT" }, { VK_RIGHT, "RIGHT" },
    { VK_RETURN, "ENTER" }, { VK_SPACE, "SPACE" }, { VK_TAB, "TAB" }, { VK_SHIFT, "SHIFT" },
    { VK_CONTROL, "CTRL" }, { VK_CAPITAL, "CAPS LOCK" }, { VK_INSERT, "INSERT" }, { VK_DELETE, "DELETE" },
    { VK_HOME, "HOME" }, { VK_END, "END" }, { VK_PRIOR, "PAGE UP" }, { VK_NEXT, "PAGE DOWN" },
    { VK_MULTIPLY, "NUM *" }, { VK_ADD, "NUM +" }, { VK_SUBTRACT, "NUM -" }, { VK_DECIMAL, "NUM ." },
    { VK_DIVIDE, "NUM /" }, { VK_OEM_1, "SEMICOLON" }, { VK_OEM_PLUS, "=" }, { VK_OEM_COMMA, "," },
    { VK_OEM_MINUS, "-" }, { VK_OEM_PERIOD, "." }, { VK_OEM_2, "/" }, { VK_OEM_3, "~" }, { VK_OEM_4, "[" },
    { VK_OEM_5, "BACKSLASH" }, { VK_OEM_6, "]" }, { VK_OEM_7, "QUOTE" },
    { VK_LBUTTON, "MOUSE LEFT" }, { VK_RBUTTON, "MOUSE RIGHT" }, { VK_MBUTTON, "MOUSE MIDDLE" },
    { VK_XBUTTON1, "MOUSE 4" }, { VK_XBUTTON2, "MOUSE 5" },
};
static void key_name(WPARAM vk, char *out, size_t cap) {
    if (!vk) { out[0] = '\0'; return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(out, cap, "%c", (char)vk); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(out, cap, "NUM %d", (int)(vk - VK_NUMPAD0)); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(out, cap, "F%d", (int)(vk - VK_F1 + 1)); return; }
    for (size_t i = 0; i < sizeof KEY_NAMES / sizeof KEY_NAMES[0]; i++)
        if (KEY_NAMES[i].vk == vk) { snprintf(out, cap, "%s", KEY_NAMES[i].name); return; }
    snprintf(out, cap, "KEY %02X", (unsigned)vk);
}
/* ... and back (any case); 0 for "none" or a name not known. */
static WPARAM key_parse(const char *s) {
    char want[24];
    snprintf(want, sizeof want, "%s", s);
    want[strcspn(want, "\r\n")] = '\0';
    for (char *p = want; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    for (WPARAM vk = 1; vk < 256; vk++) {
        char n[24];
        key_name(vk, n, sizeof n);
        if (!strcmp(n, want)) return vk;
    }
    return 0;
}
/* The bound keys' names into the menu. */
static void keys_to_menu(void) {
    for (int i = 0; i < KB_N; i++) key_name(g_bind[i], g_menu.key_name[i], sizeof g_menu.key_name[i]);
}

/* The left stick on the keyboard. Bits: 1 up, 2 down, 4 left, 8 right. */
static unsigned g_kstick;
static unsigned key_stick(WPARAM vk) {
    unsigned b = 0;
    for (int i = KB_STICK_UP; i < KB_N; i++) if (g_bind[i] == vk) b |= 1u << (i - KB_STICK_UP);
    return b;
}
/* The stick the keys hold, over the controllers' (centred: 128). */
static void key_stick_apply(uint8_t *ax, uint8_t *ay) {
    if (!g_kstick) return;
    *ax = (uint8_t)(128 + 127 * (((g_kstick & 8) != 0) - ((g_kstick & 4) != 0)));
    *ay = (uint8_t)(128 + 127 * (((g_kstick & 2) != 0) - ((g_kstick & 1) != 0)));
}

/* The PSP buttons a key presses (CIRCLE/CROSS swapped exchanges those two). */
static uint32_t key_bit(WPARAM vk) {
    uint32_t b = 0;
    for (int i = 0; i < KB_STICK_UP; i++) if (g_bind[i] == vk) b |= KEY_PSP[i];
    if (g_swap_oc && (b & 0x6000) && (b & 0x6000) != 0x6000) b ^= 0x6000;
    return b;
}

/* KEYBOARD: a row waits for a key. Escape cancels; the menu's hotkey and
 * Backspace (the menu's Back) cannot be bound. The key leaves any other
 * control it was on, and every held key is let go (the mapping changed). */
static int g_keys_save;                     /* a key was bound: the ini is saved with the menu's changes */
static int key_capture(WPARAM vk, int first) {
    if (!g_menu.open || !g_menu.key_wait) return 0;
    if (!first) return 1;
    if (vk == VK_ESCAPE) { menu_key_done(&g_menu); return 1; }
    if (vk == g_menu_vk || vk == VK_BACK) return 1;
    const int k = g_menu.key_wait - MENU_KEY_FIRST;
    for (int i = 0; i < KB_N; i++) if (g_bind[i] == vk) g_bind[i] = 0;
    g_bind[k] = vk;
    g_keys = 0;
    g_kstick = 0;
    keys_to_menu();
    menu_key_done(&g_menu);
    g_keys_save = 1;                                         /* saved with the menu's changes */
    fprintf(stderr, "menu: key %s -> %s\n", g_menu.key_name[k], KEY_INI[k]);
    return 1;
}

/* The window's messages, on the game thread (window_pump, from the window
 * thread's queue): keys, text, resizes, closing. */
static void game_msg(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)h;
    switch (msg) {
    case WM_CHAR:
        /* Text for the sign-in screen (letters do not act as PSP buttons then). */
        if (g_sega_edit.open) { if (wp > 32 && wp < 127) textedit_type(&g_sega_edit, (char)wp); }
        else if (g_login.state != LOGIN_CLOSED && wp >= 32 && wp < 127) login_type(&g_login, (char)wp);
        return;
    case WM_KEYDOWN: {
        const int first = !(lp & (1 << 30));                     /* not an auto-repeat */
#ifndef PSP2I_PROD                                     /* test keys: not in the community build */
        if (wp == VK_F12 && first) g_dump_key = 1;
        if (wp == VK_F9 && first && g_menu_vk != VK_F9) { functrace_toggle(); return; }   /* function-activity recorder */
        if (wp == VK_F7 && first && g_menu_vk != VK_F7) { rateaudit_start(); return; }    /* frame-rate audit */
        if (wp == VK_F6 && first && g_menu_vk != VK_F6) { rateaudit_start_press(); return; }   /* press audit */
        if (wp == VK_F8 && first && g_menu_vk != VK_F8) {                                      /* profile window */
            g_prof_keyed = 1;
            g_prof_keyon = !g_prof_keyon;
            fprintf(stderr, "profile: F8 -- sampling %s\n", g_prof_keyon ? "on" : "off");
            gamelog_note("[profile] F8: sampling %s", g_prof_keyon ? "on (press F8 again to stop)" : "off");
            return;
        }
#endif
        if (key_capture(wp, first)) return;
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
            return;
        }
        const uint32_t hk = host_key(wp);
        if (hk) { g_host_held |= hk; if (first) g_host_latched |= hk; return; }
        if (key_stick(wp)) { g_kstick |= key_stick(wp); ctrl_commit(); return; }
        if (first) g_keys_latched |= key_bit(wp);
        g_keys |= key_bit(wp);  ctrl_commit(); return;
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
        g_host_held &= ~host_key(wp); g_keys &= ~key_bit(wp); g_kstick &= ~key_stick(wp); ctrl_commit(); return;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {               /* Alt+Enter */
            if (!(lp & (1 << 30))) g_alt_enter = 1;
            return;
        }
        break;
    case WM_SIZE: present_resized(); web_overlay_resize(); return;
    case WM_CLOSE: web_overlay_close(); return;   /* the rest was done on the window thread */
    }
}

/* The composed frame drawn with GDI, scaled with the aspect kept and black
 * bars -- the fallback when the GPU cannot present (present.c does this on
 * the GPU). */
static void gdi_paint(void) {
    HDC dc = GetDC(g_wnd);
    if (!dc) return;
    RECT rc;
    GetClientRect(g_wnd, &rc);
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
    StretchDIBits(dc, x, y, w, hh, 0, 0, SCREEN_W, SCREEN_H, g_rgba, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_wnd, dc);
}

/* ---- the window thread ----------------------------------------------------
 *
 * The window belongs to a thread of its own that does nothing but answer
 * Windows. The game thread can be busy for seconds at a time -- a DNS lookup
 * for one of the game's network requests, the movie decoder's download, a
 * long load -- and a window whose thread reads no messages for 5 s is marked
 * "Not Responding" (its title says so, and it greys out). The window thread
 * queues what the game acts on (keys, text, resizes, closing) and the game
 * thread handles it at each vblank (window_pump), as it did the messages
 * themselves before. Closing hides the window and stops the game at once,
 * and ends the process after 10 s if the game is stuck. */
typedef struct { UINT msg; WPARAM wp; LPARAM lp; } win_msg;
#define WIN_QUEUE 512
static win_msg g_winq[WIN_QUEUE];
static unsigned g_winq_head, g_winq_tail;
static CRITICAL_SECTION g_winq_lock;

static void winq_push(UINT msg, WPARAM wp, LPARAM lp) {
    EnterCriticalSection(&g_winq_lock);
    if (g_winq_tail - g_winq_head < WIN_QUEUE) {
        win_msg *m = &g_winq[g_winq_tail++ % WIN_QUEUE];
        m->msg = msg; m->wp = wp; m->lp = lp;
    }
    LeaveCriticalSection(&g_winq_lock);
}

/* MOUSE CAMERA. The window thread reads the mouse's raw movement (WM_INPUT:
 * counts, not the cursor -- it never stops at the screen's edge) and, while
 * the game thread asks (WM_APP_MOUSECAP: on, the game playing; off, the menu
 * or the sign-in screen open), holds the cursor: hidden, kept in the window.
 * Losing focus lets it go; coming back takes it again. */
#define WM_APP_MOUSECAP (WM_APP + 7)
static volatile LONG g_mouse_dx, g_mouse_dy;   /* movement since the game thread took it */
static volatile LONG g_mouse_held;             /* the cursor is held (the movement counts) */
static int g_mouse_want;                        /* (window thread) the game asks for it */

static void mouse_hold(HWND h, int on) {
    if (on) {
        RECT r;
        GetClientRect(h, &r);
        POINT a = { r.left, r.top }, b = { r.right, r.bottom };
        ClientToScreen(h, &a);
        ClientToScreen(h, &b);
        const RECT s = { a.x, a.y, b.x, b.y };
        ClipCursor(&s);
        if (!g_mouse_held) while (ShowCursor(FALSE) >= 0) {}
    } else {
        ClipCursor(NULL);
        if (g_mouse_held) while (ShowCursor(TRUE) < 0) {}
    }
    InterlockedExchange(&g_mouse_held, on);
}

#define STUCK_TIMER 1
static volatile LONG g_game_stopped;   /* the game loop has returned: the clean-up and report run (any length) */
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INPUT: {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (g_mouse_held && GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1
            && ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            InterlockedExchangeAdd(&g_mouse_dx, ri.data.mouse.lLastX);
            InterlockedExchangeAdd(&g_mouse_dy, ri.data.mouse.lLastY);
        }
        break;                                                    /* DefWindowProc frees it */
    }
    case WM_APP_MOUSECAP:
        g_mouse_want = (int)wp;
        mouse_hold(h, g_mouse_want && GetForegroundWindow() == h);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) { if (g_mouse_held) mouse_hold(h, 0); }
        else if (g_mouse_want) mouse_hold(h, 1);
        break;
    case WM_MOVE:
        if (g_mouse_held) mouse_hold(h, 1);                       /* the window moved: clip to where it is */
        break;
    case WM_SETCURSOR:
        if (g_mouse_held && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_CHAR: case WM_KEYDOWN: case WM_KEYUP:
        winq_push(msg, wp, lp);
        return 0;
    /* Mouse buttons go to the game thread as keys (VK_LBUTTON ..., bindable in
     * CONTROLS > KEYBOARD); the window keeps the mouse while one is down, so
     * a release outside it still arrives. */
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: {
        const int down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
        const WPARAM vk = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ? VK_LBUTTON
                        : msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ? VK_RBUTTON
                        : msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ? VK_MBUTTON
                        : GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
        winq_push(down ? WM_KEYDOWN : WM_KEYUP, vk, 0);
        if (down) SetCapture(h);
        else if (!(GET_KEYSTATE_WPARAM(wp) & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2))) ReleaseCapture();
        return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP ? TRUE : 0;
    }
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { winq_push(msg, wp, lp); return 0; }   /* Alt+Enter */
        break;
    case WM_SYSCHAR:
        if (wp == '\r') return 0;                                 /* no beep for Alt+Enter */
        break;
    case WM_SIZE:
        if (g_mouse_held) mouse_hold(h, 1);
        winq_push(msg, wp, lp);
        web_overlay_post(WEB_POST_RESIZE);     /* the game thread may be in the page's own loop */
        return 0;
    case WM_ERASEBKGND: return 1;                                 /* the game draws it all */
    case WM_PAINT: {                                              /* ... every vblank */
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        /* Gone at once: the game stops at its next check and the clean-up
         * (report, devices, libraries) runs after, out of sight and silent. */
        if (!g_close_at) {
            g_close_at = GetTickCount64();
            ShowWindow(h, SW_HIDE);
            audio_set_master(0.0f);
            psp_request_exit();
            winq_push(msg, wp, lp);
            web_overlay_post(WEB_POST_CLOSE);   /* a web page open over the game (LoadExec, below) */
            SetTimer(h, STUCK_TIMER, 10000, NULL);
        }
        return 0;
    case WM_TIMER:
        if (wp == STUCK_TIMER && !g_game_stopped) TerminateProcess(GetCurrentProcess(), 1);   /* closed, and the game never stopped */
        break;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static HANDLE g_win_ready;

static DWORD WINAPI window_thread(LPVOID arg) {
    WNDCLASSA *wc = (WNDCLASSA *)arg;
    RegisterClassA(wc);
    g_wnd = CreateWindowA("psp2i", WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, SCREEN_W * 2, SCREEN_H * 2,
                          NULL, NULL, wc->hInstance, NULL);
    SetEvent(g_win_ready);
    if (!g_wnd) return 1;
    {   /* the mouse's raw movement, for MOUSE CAMERA */
        RAWINPUTDEVICE rid = { 0x01, 0x02, 0, g_wnd };
        RegisterRawInputDevices(&rid, 1, sizeof rid);
    }
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    return 0;
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
    wc.style = CS_OWNDC;                                     /* OpenGL keeps the window's DC */
    /* The exe's icon (resource 1, asset/icon.ico; compiled into the prod
     * build): large for the taskbar and Alt+Tab, small for the title bar.
     * Without the resource both are NULL and Windows' default stays. */
    wc.hIcon = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    InitializeCriticalSection(&g_winq_lock);
    g_win_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, window_thread, &wc, 0, NULL);
    if (t) { WaitForSingleObject(g_win_ready, INFINITE); CloseHandle(t); }
    {
        HICON sm_icon = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        if (wc.hIcon) SendMessageA(g_wnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
        if (sm_icon) SendMessageA(g_wnd, WM_SETICON, ICON_SMALL, (LPARAM)sm_icon);
    }
    window_apply_res(res);
    const char *e = getenv("PSP2I_PRESENT");
    if (e && !strcmp(e, "gdi")) fprintf(stderr, "present: PSP2I_PRESENT=gdi, scaling with GDI\n");
    else if (g_renderer != REN_GL) present_init(g_wnd);      /* OpenGL presents itself (window_show) */
}

/* The composed frame into the window on the GPU: 0, or nonzero for GDI. */
static int window_show(void) {
    if (g_gl_window) {
        RECT rc;
        GetClientRect(g_wnd, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) return 0;      /* minimised */
        int x, y, w, h;
        present_fit(rc.right, rc.bottom, SCREEN_W, SCREEN_H, &x, &y, &w, &h);
        if (psp_ge_threaded()) { gl_show_mt(g_rgba, SCREEN_W, SCREEN_H, x, y, w, h, rc.right, rc.bottom); return 0; }
        if (gl_blit_frame(g_rgba, SCREEN_W, SCREEN_H, x, y, w, h, rc.right, rc.bottom) != 0) return -1;
        gl_platform_swap();
        return 0;
    }
    return present_active() ? present_frame(g_rgba, SCREEN_W, SCREEN_H) : -1;
}

static void window_pump(void) {
    MSG m;   /* the game thread's own windows: the web page, SDL's */
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    for (;;) {                                /* the game window's, from the window thread */
        win_msg w;
        EnterCriticalSection(&g_winq_lock);
        const int have = g_winq_head != g_winq_tail;
        if (have) w = g_winq[g_winq_head++ % WIN_QUEUE];
        LeaveCriticalSection(&g_winq_lock);
        if (!have) break;
        game_msg(g_wnd, w.msg, w.wp, w.lp);
    }
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
static int      g_shot_by_flip;   /* --shot-by-flip: --overlay-shot numbers are display flips (game frames) */

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

/* ---- input recording and replay -------------------------------------------------
 * PSP2I_INPUT_RECORD=FILE records what the game reads -- buttons and left stick
 * after the menu filter, and the camera stick -- whenever it changes, keyed by
 * game frame (display flip; the game reads input once per frame), plus the
 * frame-rate setting:
 *   fps 60                     at the start, and after every menu change
 *   F <flip> <buttons> <lx> <ly>   buttons in hex, sticks 0..255 (128 centre)
 *   R <flip> <rx> <ry>
 * --replay-input FILE plays such a file back: the same input on the same game
 * frame, so a route a player walked once can be re-run headless (fps from the
 * file unless --fps is given). */
static FILE    *g_rec;
static uint32_t g_rec_bits = 0xFFFFFFFFu;
static uint8_t  g_rec_ax, g_rec_ay, g_rec_rx = 128, g_rec_ry = 128;

static void rec_open(const char *path, int fps) {
    g_rec = fopen(path, "w");
    if (!g_rec) { fprintf(stderr, "input: cannot write the recording %s\n", path); return; }
    fprintf(g_rec, "# psp2i input recording (see --replay-input)\nfps %d\n", fps);
    fflush(g_rec);
    fprintf(stderr, "input: recording to %s\n", path);
}

static void rec_pad(uint32_t bits, uint8_t ax, uint8_t ay) {
    if (!g_rec || (bits == g_rec_bits && ax == g_rec_ax && ay == g_rec_ay)) return;
    g_rec_bits = bits; g_rec_ax = ax; g_rec_ay = ay;
    fprintf(g_rec, "F %llu %04X %u %u\n", (unsigned long long)psp_display_flips(), bits, ax, ay);
    fflush(g_rec);
}

static void rec_cam(uint8_t rx, uint8_t ry) {
    if (!g_rec || (rx == g_rec_rx && ry == g_rec_ry)) return;
    g_rec_rx = rx; g_rec_ry = ry;
    fprintf(g_rec, "R %llu %u %u\n", (unsigned long long)psp_display_flips(), rx, ry);
    fflush(g_rec);
}

static void rec_fps(int fps) {
    if (!g_rec) return;
    fprintf(g_rec, "fps %d\n", fps);
    fflush(g_rec);
}

typedef struct { uint64_t flip; char kind; uint32_t a; uint8_t x, y; } rep_event;
static rep_event *g_rep;
static int      g_nrep, g_rep_pos, g_rep_on;
static uint32_t g_rep_bits;
static uint8_t  g_rep_ax = 128, g_rep_ay = 128, g_rep_rx = 128, g_rep_ry = 128;

/* Load a recording; returns its first fps (0 if none), -1 on error. */
static int replay_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "input: cannot read the recording %s\n", path); return -1; }
    char line[128];
    int cap = 0, first_fps = 0;
    uint64_t last_flip = 0;
    while (fgets(line, sizeof line, f)) {
        rep_event e;
        memset(&e, 0, sizeof e);
        unsigned long long fl;
        unsigned a, x, y;
        int fps;
        if (line[0] == 'F' && sscanf(line + 1, "%llu %x %u %u", &fl, &a, &x, &y) == 4) { e.kind = 'F'; e.flip = fl; e.a = a; e.x = (uint8_t)x; e.y = (uint8_t)y; }
        else if (line[0] == 'R' && sscanf(line + 1, "%llu %u %u", &fl, &x, &y) == 3) { e.kind = 'R'; e.flip = fl; e.x = (uint8_t)x; e.y = (uint8_t)y; }
        else if (sscanf(line, "fps %d", &fps) == 1) { if (!first_fps && !g_nrep) { first_fps = fps; continue; } e.kind = 'S'; e.flip = last_flip; e.a = (uint32_t)fps; }
        else continue;
        last_flip = e.flip;
        if (g_nrep == cap) {
            cap = cap ? cap * 2 : 1024;
            rep_event *n = (rep_event *)realloc(g_rep, sizeof *g_rep * (size_t)cap);
            if (!n) { fclose(f); return -1; }
            g_rep = n;
        }
        g_rep[g_nrep++] = e;
    }
    fclose(f);
    g_rep_on = 1;
    fprintf(stderr, "input: replaying %s (%d events, to game frame %llu)\n", path, g_nrep,
            g_nrep ? (unsigned long long)g_rep[g_nrep - 1].flip : 0ull);
    return first_fps;
}

static void ctrl_commit(void);
static void replay_step(void) {
    if (!g_rep_on) return;
    const uint64_t f = psp_display_flips();
    {   /* PSP2I_FLIP_MARKS=N: the time every N game frames (tests compare a stretch of the route) */
        static long long every = -2;
        static uint64_t last;
        if (every == -2) { const char *e = getenv("PSP2I_FLIP_MARKS"); every = e ? atoll(e) : -1; }
        if (every > 0 && f / (uint64_t)every != last / (uint64_t)every) {
            fprintf(stderr, "flip-mark: frame %llu at %.3f s\n", (unsigned long long)f, psp_sched_now_us() / 1e6);
            last = f;
        }
    }
    int pad = 0;
    while (g_rep_pos < g_nrep && g_rep[g_rep_pos].flip <= f) {
        const rep_event *e = &g_rep[g_rep_pos++];
        if (e->kind == 'F') { g_rep_bits = e->a; g_rep_ax = e->x; g_rep_ay = e->y; pad = 1; }
        else if (e->kind == 'R') { g_rep_rx = e->x; g_rep_ry = e->y; }
        else if (e->kind == 'S') { framerate_set((int)e->a); g_menu.fps = (int)e->a; fprintf(stderr, "input: replay sets %u fps\n", e->a); }
    }
    if (pad) ctrl_commit();
    if (g_rep_pos == g_nrep && g_nrep) {
        fprintf(stderr, "input: replay finished at game frame %llu (vblank %llu)\n",
                (unsigned long long)f, (unsigned long long)psp_sched_vblank_count());
        g_nrep = 0;                                       /* report once; the last state stays */
    }
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
    replay_step();
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
#ifdef _WIN32
    key_stick_apply(&ax, &ay);
#endif
    if (g_script_stick >= 0) { ax = g_stick[g_script_stick].x; ay = g_stick[g_script_stick].y; }
    if (g_rep_on) { bits |= g_rep_bits; ax = g_rep_ax; ay = g_rep_ay; }
    /* The settings menu takes all input while open (menu_filter_game). */
    if (g_login.state != LOGIN_CLOSED || web_overlay_active()) {
        /* The sign-in screen (or a web page over the game: the PS Store)
         * has the input; buttons still held when it closes stay hidden from
         * the game until released. */
        g_menu.game_mask |= bits & ~PRESS_MENU;
        bits = 0;
        ax = ay = 128;
    }
    bits = menu_filter_game(&g_menu, bits & ~PRESS_MENU);
    if (g_menu.open) ax = ay = 128;
    if (!g_rep_on) rec_pad(bits, ax, ay);
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

static int g_res_saved = RES_DEFAULT;       /* what psp2i_display.ini holds */
static int g_ren_saved = REN_D3D11;
static int g_vol_saved[VOL_N] = { VOL_STEPS, VOL_STEPS, VOL_STEPS };
static int g_att_saved = ATT_DEFAULT;
static int g_openh264 = 1;                  /* fetch Cisco's OpenH264 for movies (h264_openh264.c) */
static char g_font_key[260] = "system";     /* the game's text font (font_win.h): system, psp, or a file in fonts/ */

/* "80" -> 8 tenths (0..100, rounded to 10%). */
static int vol_parse(const char *s) {
    const int v = atoi(s);
    return v < 0 ? 0 : v > 100 ? VOL_STEPS : (v + 5) / 10;
}

static int display_load(void) {
    FILE *f = g_display_ini[0] ? fopen(g_display_ini, "r") : NULL;
    if (!f) return -1;
    char line[128];
    int res = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "resolution=", 11)) res = menu_res_parse(line + 11);
        else if (!strncmp(line, "renderer=", 9)) { const int r = menu_ren_parse(line + 9); if (r >= 0) g_ren_saved = r; }
        else if (!strncmp(line, "master_volume=", 14)) g_vol_saved[VOL_MASTER] = vol_parse(line + 14);
        else if (!strncmp(line, "music_volume=", 13))  g_vol_saved[VOL_MUSIC] = vol_parse(line + 13);
        else if (!strncmp(line, "sfx_volume=", 11))    g_vol_saved[VOL_SFX] = vol_parse(line + 11);
        else if (!strncmp(line, "attenuation=", 12))   g_att_saved = !strncmp(line + 12, "off", 3) ? ATT_OFF : menu_att_parse(atoi(line + 12));
        else if (!strncmp(line, "openh264=", 9))       g_openh264 = strncmp(line + 9, "off", 3) != 0;
        else if (!strncmp(line, "swap_circle_cross=", 18)) g_swap_oc = !strncmp(line + 18, "on", 2);
        else if (!strncmp(line, "rs_speed=", 9))       g_rs_saved = (float)atof(line + 9);
        else if (!strncmp(line, "right_stick=", 12))   g_rs_on = strncmp(line + 12, "off", 3) != 0;
        else if (!strncmp(line, "stick_deadzone=", 15)) g_deadzone_pct = atoi(line + 15);
        else if (!strncmp(line, "mouse_camera=", 13))  g_mouse_cam = !strncmp(line + 13, "on", 2);
#ifdef _WIN32
        else if (!strncmp(line, "key_", 4)) {
            for (int i = 0; i < KB_N; i++) {
                const size_t n = strlen(KEY_INI[i]);
                if (!strncmp(line + 4, KEY_INI[i], n) && line[4 + n] == '=') { g_bind[i] = key_parse(line + 5 + n); break; }
            }
        }
#endif
        else if (!strncmp(line, "test_tick_pacing=", 17)) g_test_ticks = !strncmp(line + 17, "on", 2);
        else if (!strncmp(line, "draw_distance=", 14))    g_draw_dist = !strncmp(line + 14, "far", 3);
        else if (!strncmp(line, "vertex_work=", 12))      g_gpu_vertex = strncmp(line + 12, "cpu", 3) != 0;
        else if (!strncmp(line, "load_models=", 12))      g_load_models = !strncmp(line + 12, "on", 2);
        else if (!strncmp(line, "ge_thread=", 10))        g_ge_thread = !strncmp(line + 10, "on", 2);
        else if (!strncmp(line, "scaling=", 8))  { const int v = postfx_scaling_parse(line + 8); if (v >= 0) g_post.scaling = v; }
        else if (!strncmp(line, "fxaa=", 5))     g_post.fxaa = !strncmp(line + 5, "on", 2);
        else if (!strncmp(line, "sharpen=", 8))  { const int v = postfx_level_parse(line + 8); if (v >= 0) g_post.sharpen = v; }
        else if (!strncmp(line, "scanlines=", 10)) { const int v = postfx_level_parse(line + 10); if (v >= 0) g_post.scanlines = v; }
        else if (!strncmp(line, "brightness=", 11)) g_post.brightness = (atoi(line + 11) + 5) / 10;
        else if (!strncmp(line, "saturation=", 11)) g_post.saturation = (atoi(line + 11) + 5) / 10;
        else if (!strncmp(line, "test_30hz_logic=", 16))  g_test_logic = !strncmp(line + 16, "on", 2);
        else if (!strncmp(line, "font=", 5)) {
            snprintf(g_font_key, sizeof g_font_key, "%s", line + 5);
            g_font_key[strcspn(g_font_key, "\r\n")] = '\0';
        }
    }
    fclose(f);
    if (res >= 0) g_res_saved = res;
    return res;
}

static void display_save(int res) {
    g_res_saved = res;
    FILE *f = g_display_ini[0] ? fopen(g_display_ini, "w") : NULL;
    if (!f) { fprintf(stderr, "display: cannot write %s\n", g_display_ini); return; }
    fprintf(f, "; psp2i display settings (the settings menu changes this)\n");
    fprintf(f, "resolution=%s\n", menu_res_name(res));
    fprintf(f, "; renderer: d3d11 or opengl (used from the next start)\n");
    fprintf(f, "renderer=%s\n", menu_ren_key(g_ren_saved));
    fprintf(f, "; volumes 0..100 (%%); attenuation while other programs play sound: off, -6db, -12db, -20db\n");
    fprintf(f, "master_volume=%d\nmusic_volume=%d\nsfx_volume=%d\n",
            g_vol_saved[VOL_MASTER] * 10, g_vol_saved[VOL_MUSIC] * 10, g_vol_saved[VOL_SFX] * 10);
    if (g_att_saved == ATT_OFF) fprintf(f, "attenuation=off\n");
    else fprintf(f, "attenuation=%ddb\n", menu_att_db(g_att_saved));
    fprintf(f, "; movies: on = download Cisco's OpenH264 decoder on first start (licenses/OpenH264-NOTICE.txt), off = black movies\n");
    fprintf(f, "openh264=%s\n", g_openh264 ? "on" : "off");
    fprintf(f, "; the game's text font: system (Windows' font), psp (flash/font, if there), or a .ttf/.otf/.ttc in the fonts folder\n");
    fprintf(f, "font=%s\n", g_font_key);
    fprintf(f, "; on = Circle and Cross exchanged (keys X/Z and controllers)\n");
    fprintf(f, "swap_circle_cross=%s\n", g_swap_oc ? "on" : "off");
    fprintf(f, "; the right stick: on = it turns the camera, off = it does nothing; rs_speed 1.00..2.00\n");
    fprintf(f, "right_stick=%s\nrs_speed=%.2f\n", g_rs_on ? "on" : "off", (double)menu_rs_speed(&g_menu));
    fprintf(f, "; controller sticks: dead zone in %% (0 = the game's own); the mouse turns the camera (on/off)\n");
    fprintf(f, "stick_deadzone=%d\nmouse_camera=%s\n", g_deadzone_pct, g_mouse_cam ? "on" : "off");
#ifdef _WIN32
    fprintf(f, "; the keys on the PSP's controls (menu CONTROLS > KEYBOARD): a letter, a digit, F1.., UP, ENTER, SPACE, NUM 5, MOUSE LEFT/RIGHT/MIDDLE/4/5, ...; none = no key\n");
    for (int i = 0; i < KB_N; i++) {
        char n[16];
        key_name(g_bind[i], n, sizeof n);
        fprintf(f, "key_%s=%s\n", KEY_INI[i], n[0] ? n : "none");
    }
#endif
    fprintf(f, "; menu TESTS at 60 / 120 fps (off by default): the game's tick counters paced as at 30; frame-counted logic at 30 Hz\n");
    fprintf(f, "test_tick_pacing=%s\ntest_30hz_logic=%s\n", g_test_ticks ? "on" : "off", g_test_logic ? "on" : "off");
    fprintf(f, "; far = the longer draw distance (off while saving), normal = the game's\n");
    fprintf(f, "draw_distance=%s\n", g_draw_dist ? "far" : "normal");
    fprintf(f, "; gpu = 3D vertices transformed and lit on the graphics card, cpu = on the processor (as before)\n");
    fprintf(f, "vertex_work=%s\n", g_gpu_vertex ? "gpu" : "cpu");
    fprintf(f, "; OpenGL: on = drawing and presenting run on a thread of their own, beside the game (used from the next start)\n");
    fprintf(f, "ge_thread=%s\n", g_ge_thread ? "on" : "off");
    fprintf(f, "; on = models in the Data folder (EXTRACT MODELS writes them) replace the game's as they load\n");
    fprintf(f, "load_models=%s\n", g_load_models ? "on" : "off");
    fprintf(f, "; post-processing: scaling nearest / bilinear / sharp_bilinear; fxaa on/off; sharpen and scanlines\n"
               "; off / low / high; brightness 70..130 and saturation 0..200 (%%)\n");
    char sc[24];
    snprintf(sc, sizeof sc, "%s", postfx_scaling_name(g_post.scaling));
    for (char *q = sc; *q; q++) *q = *q == ' ' ? '_' : (char)tolower((unsigned char)*q);
    char sh[8], sl[8];
    snprintf(sh, sizeof sh, "%s", postfx_level_name(g_post.sharpen));
    snprintf(sl, sizeof sl, "%s", postfx_level_name(g_post.scanlines));
    for (char *q = sh; *q; q++) *q = (char)tolower((unsigned char)*q);
    for (char *q = sl; *q; q++) *q = (char)tolower((unsigned char)*q);
    fprintf(f, "scaling=%s\nfxaa=%s\nsharpen=%s\nscanlines=%s\nbrightness=%d\nsaturation=%d\n", sc, g_post.fxaa ? "on" : "off",
            sh, sl, g_post.brightness * 10, g_post.saturation * 10);
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

/* Volumes and attenuation to where they act: the device output (master and
 * attenuation, audio_sdl.c), the music decoder and the sound-effect mixer. */
static void audio_apply(void) {
    audio_set_master((float)g_vol_saved[VOL_MASTER] / VOL_STEPS);
    atrac_at3_set_gain((float)g_vol_saved[VOL_MUSIC] / VOL_STEPS);
    psp_sas_set_gain((float)g_vol_saved[VOL_SFX] / VOL_STEPS);
    audio_set_attenuation_db(menu_att_db(g_att_saved));
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
#ifdef _WIN32
    key_stick_apply(&ax, &ay);
#endif
    if (g_script_stick >= 0) { ax = g_stick[g_script_stick].x; ay = g_stick[g_script_stick].y; }
    uint32_t held = menu_inputs(psp, ax, ay) | g_host_held;
    if (input_sdl_host_buttons() & (INPUT_HOST_GUIDE | INPUT_HOST_RSTICK)) held |= MI_TOGGLE;

    snprintf(g_menu.sega, sizeof g_menu.sega, "%s", online_sega_redirect());
    snprintf(g_menu.adhoc, sizeof g_menu.adhoc, "%s", online_adhoc_server());
    snprintf(g_menu.modern, sizeof g_menu.modern, "%s", online_modern_server());
    g_menu.adhoc_mode = online_adhoc_mode();
    if (g_sega_edit.open) {
        /* The text editor (SEGA SERVER or ADHOC SERVER) has the input (pad
         * buttons and navigation keys; typed characters arrive through WM_CHAR). */
        const uint32_t pad = g_script_bits | g_pad_bits;
        int r = textedit_update(&g_sega_edit, menu_inputs_from_psp(pad & 0xFFFF, ax, ay) | g_login_keys, pad);
        if (g_edit_enter) { g_edit_enter = 0; g_sega_edit.open = 0; r = TE_SAVE; }
        if (r == TE_SAVE && g_edit_adhoc == 2) {
            online_set_modern_server(g_sega_edit.text);
            fprintf(stderr, "menu: modern server %s\n", online_modern_server());
        } else if (r == TE_SAVE && g_edit_adhoc) {
            online_set_adhoc_server(g_sega_edit.text);
            fprintf(stderr, "menu: ad hoc server %s\n", online_adhoc_server());
        } else if (r == TE_SAVE) {
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
        g_edit_adhoc = 0;
    }
    if (fx & MFX_EDIT_ADHOC) {
        textedit_open(&g_sega_edit, "ADHOC SERVER", "HOST[:PORT] (DEFAULT PORT 27312)", online_adhoc_server());
        g_edit_adhoc = 1;
    }
    if (fx & MFX_EDIT_MODERN) {
        textedit_open(&g_sega_edit, "MODERN SERVER", "HOST, OR THE HOSTING PLAYER (IPV4 OR IPV6)", online_modern_server());
        g_edit_adhoc = 2;
    }
    if (fx & MFX_OPENED) {                    /* what the others type to join, if this game hosts modern */
        char v4[64], v6[64];
        psp_adhoc_host_addresses(v4, sizeof v4, v6, sizeof v6);
        const char *ms = online_modern_server();
        if (!ms[0] || !strcmp(ms, "host"))
            snprintf(g_menu.share, sizeof g_menu.share, "%s%s%s", v6[0] ? v6 : v4, v6[0] && v4[0] ? " / " : "", v6[0] ? v4 : "");
        else g_menu.share[0] = '\0';
    }
    if (fx & MFX_ADHOC_MODE) {
        online_set_adhoc_mode(g_menu.adhoc_mode);
        fprintf(stderr, "menu: ad hoc connection %d (0 PPSSPP direct, 1 PPSSPP relay, 2 modern)\n", g_menu.adhoc_mode);
    }
    if (fx & MFX_FPS_CHANGED) {
        framerate_set(g_menu.fps);
        rec_fps(g_menu.fps);
        fprintf(stderr, "menu: frame rate %d fps\n", g_menu.fps);
    }
    if (fx & MFX_RES_CHANGED) display_set(g_menu.res);
    if (fx & MFX_AUDIO) {
        for (int k = 0; k < VOL_N; k++) g_vol_saved[k] = g_menu.vol[k];
        g_att_saved = g_menu.att;
        audio_apply();
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: volume master %d%%, music %d%%, effects %d%%; attenuation %d dB\n",
                g_vol_saved[VOL_MASTER] * 10, g_vol_saved[VOL_MUSIC] * 10, g_vol_saved[VOL_SFX] * 10, menu_att_db(g_att_saved));
    }
    if (fx & MFX_RENDERER) {
        g_ren_saved = g_menu.renderer;
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: renderer %s (saved; used from the next start)\n", menu_ren_key(g_menu.renderer));
    }
    if (fx & MFX_SWAP_OC) {
        g_swap_oc = g_menu.swap_oc;
        input_sdl_set_swap_circle_cross(g_swap_oc);
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: Circle/Cross %s (saved)\n", g_swap_oc ? "swapped" : "normal");
    }
    if (fx & MFX_FONT) {
        g_menu.font = font_win_select(g_menu.font);
        snprintf(g_menu.font_name, sizeof g_menu.font_name, "%s", font_win_name(g_menu.font));
        snprintf(g_font_key, sizeof g_font_key, "%s", font_win_key(g_menu.font));
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: font %s (saved; text drawn from now on)\n", g_font_key);
    }
    if (fx & MFX_SHOW_FPS) fprintf(stderr, "menu: FPS counter %s\n", g_menu.show_fps ? "on" : "off");
    if (fx & MFX_POSTFX) {
        g_post = g_menu.post;
        postfx_set(&g_post);
        if (!g_headless) display_save(g_res_saved);
    }
    if (fx & MFX_LOAD_MODELS) {         /* used as models load: from the next area */
        g_load_models = g_menu.load_models;
        modelx_set_load(g_load_models);
        if (!g_headless) display_save(g_res_saved);
    }
    if (fx & MFX_EXTRACT) {             /* EXTRACT MODELS: the loaded models to Data/<group>/ */
        char dir[600];
        exe_dir(dir, sizeof dir);
        snprintf(g_menu.model_note, sizeof g_menu.model_note, "WORKING...");
        modelx_extract(dir, g_menu.model_note, sizeof g_menu.model_note);
    }
    if (fx & MFX_QUIT) {
        /* QUIT GAME: gone at once and silent; the game stops at its next
         * check, the report and the profile are written in full out of sight
         * (no time limit, unlike closing the window), then the process ends
         * (see the end of main). */
        g_quit_at = psp_sched_now_us() ? psp_sched_now_us() : 1;
        audio_set_master(0.0f);
#ifdef _WIN32
        if (g_wnd) ShowWindow(g_wnd, SW_HIDE);
#endif
        fprintf(stderr, "menu: quit game\n");
        online_log("exit: QUIT GAME from the engine menu");
        psp_request_exit();
    }
    if (fx & MFX_VERTEX) {
        g_gpu_vertex = g_menu.gpu_vertex;
        psp_gpu_set_gpu_transform(g_gpu_vertex);
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: vertex work on the %s (saved)\n", g_gpu_vertex ? "GPU" : "CPU");
    }
    if (fx & MFX_DRAW_DIST) {
        g_draw_dist = g_menu.draw_dist;
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: draw distance %s (saved)\n", g_draw_dist ? "far" : "normal");
    }
    if (fx & MFX_TESTS) {
        g_test_ticks = g_menu.test_ticks;
        g_test_logic = g_menu.test_logic;
        framerate_set_tests(g_test_ticks, g_test_logic);
        if (!g_headless) display_save(g_res_saved);
    }
    if (fx & MFX_RS_ON) {
        g_rs_on = g_menu.rs_on;
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: right stick %s (saved)\n", g_rs_on ? "turns the camera" : "off");
    }
    if (fx & MFX_DEADZONE) {
        g_deadzone_pct = menu_deadzone_pct(&g_menu);
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: stick dead zone %d%% (saved)\n", g_deadzone_pct);
    }
    if (fx & MFX_MOUSE_CAM) {
        g_mouse_cam = g_menu.mouse_cam;
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: mouse camera %s (saved)\n", g_mouse_cam ? "on" : "off");
    }
#ifdef _WIN32
    if (fx & MFX_KEYS_RESET) {
        memcpy(g_bind, KEY_DEFAULTS, sizeof g_bind);
        g_keys = 0;
        g_kstick = 0;
        keys_to_menu();
        g_keys_save = 1;
        fprintf(stderr, "menu: keyboard back to the default keys\n");
    }
    if (g_keys_save) {
        g_keys_save = 0;
        if (!g_headless) display_save(g_res_saved);
    }
#endif
    if (fx & MFX_RS_SPEED) {
        camera_set_speed(menu_rs_speed(&g_menu));
        if (!g_headless) display_save(g_res_saved);
        fprintf(stderr, "menu: right-stick sensitivity %.2fx\n", (double)menu_rs_speed(&g_menu));
    }
    if (fx & (MFX_OPENED | MFX_CLOSED)) {
        fprintf(stderr, "menu: %s at vblank %llu\n", (fx & MFX_OPENED) ? "opened" : "closed",
                (unsigned long long)psp_sched_vblank_count());
        ctrl_commit();                     /* take or give back the game's input now */
    }
}

#ifdef _WIN32
/* MOUSE CAMERA, once per vblank: ask the window thread to hold the cursor
 * while the game is being played, and hand the movement to the camera. */
static void mouse_camera_step(void) {
    static int asked = -1;
    static uint64_t last_us;
    const int want = g_menu.mouse_cam && !g_menu.open && g_login.state == LOGIN_CLOSED && !web_overlay_active() && !g_headless;
    if (want != asked && g_wnd) { PostMessageA(g_wnd, WM_APP_MOUSECAP, (WPARAM)want, 0); asked = want; }
    const uint64_t now = psp_sched_now_us();
    const LONG dx = InterlockedExchange(&g_mouse_dx, 0), dy = InterlockedExchange(&g_mouse_dy, 0);
    const int on = want && g_mouse_held;
    camera_mouse(on ? (int)dx : 0, on ? (int)dy : 0, last_us ? (double)(now - last_us) / 1e6 : 0.0);
    last_us = now;
}
#endif

/* The right stick for the camera (camera.c): SDL, or --rstick in scripts;
 * centred while the settings menu is open or the stick is turned off there. */
static void update_camera_stick(uint64_t vb) {
    uint8_t rx = 128, ry = 128;
    if (g_sdl_on) { input_sdl_right_stick(&rx, &ry); stick_deadzone(&rx, &ry, g_deadzone_pct); }
#ifdef _WIN32
    mouse_camera_step();
#endif
    for (int i = 0; i < g_nrstick; i++)
        if (vb >= g_rstick[i].at && vb < g_rstick[i].at + g_rstick[i].len) { rx = g_rstick[i].x; ry = g_rstick[i].y; }
    if (g_rep_on) { rx = g_rep_rx; ry = g_rep_ry; }
    if (g_menu.open || !g_menu.rs_on) rx = ry = 128;      /* the menu's RIGHT STICK off: no camera */
    if (!g_rep_on) rec_cam(rx, ry);
    camera_set_stick(rx, ry);
}

static void poll_controllers(void) {
    if (!g_sdl_on) return;
    uint32_t b;
    uint8_t ax, ay;
    input_sdl_poll(&b, &ax, &ay);
    stick_deadzone(&ax, &ay, g_deadzone_pct);
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

/* Once a minute, one "perf:" line in online_log.txt: frame rate, frame busy
 * times, GE and idle share and the process's memory over that minute -- so
 * slowdowns that only come in long sessions show what grew with them. */
static void perf_minute(double fps) {
    static double sum, mn = 1e9;
    static int n;
    static uint64_t last_fb[6], last_ge, last_idle, last_us;
    sum += fps; n++;
    if (fps < mn) mn = fps;
    if (n < 60) return;
    const uint64_t now = psp_sched_now_us(), ge = psp_ge_host_us(), idle = psp_sched_idle_us();
    uint64_t fb[6];
    psp_display_frame_busy(fb);
    const double span = last_us ? (double)(now - last_us) : (double)now;
    double mb = 0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) mb = (double)pmc.PagefileUsage / (1024.0 * 1024.0);
#endif
    char ge_line[300] = "";
    {   /* the GE thread: what the game waited for it (all, of which VRAM accesses), its host work */
        static uint64_t lw, lwu, lj, lvn, lvu;
        uint64_t w, wu, j, vn, vu;
        char top[96];
        psp_ge_thread_stats(&w, &wu, &j);
        gl_ge_vram_waits(&vn, &vu, top, sizeof top);
        if (psp_ge_threaded())
            snprintf(ge_line, sizeof ge_line, "; GE thread: waits %llu (%.0f ms), of which VRAM %llu (%.0f ms; %s), host work %.1f%%",
                     (unsigned long long)(w - lw), (wu - lwu) / 1000.0, (unsigned long long)(vn - lvn), (vu - lvu) / 1000.0, top,
                     (last_us ? 100.0 * (double)(j - lj) / (double)(now - last_us) : 0.0));
        lw = w; lwu = wu; lj = j; lvn = vn; lvu = vu;
        static uint64_t lm, ls;
        uint64_t m, sk;
        gl_ge_state_counts(&m, &sk);
        if (g_renderer == REN_GL) {
            const size_t len = strlen(ge_line);
            snprintf(ge_line + len, sizeof ge_line - len, "; GL state calls %llu, skipped %llu",
                     (unsigned long long)(m - lm), (unsigned long long)(sk - ls));
        }
        lm = m; ls = sk;
    }
    online_log("perf: %.1f min, fps avg %.1f min %.1f; frames <8ms %llu, <16.7 %llu, <25 %llu, <33 %llu, <50 %llu, 50+ %llu; "
               "GE %.1f%%, idle %.1f%%; memory %.0f MB%s",
               now / 60e6, sum / n, mn,
               (unsigned long long)(fb[0] - last_fb[0]), (unsigned long long)(fb[1] - last_fb[1]), (unsigned long long)(fb[2] - last_fb[2]),
               (unsigned long long)(fb[3] - last_fb[3]), (unsigned long long)(fb[4] - last_fb[4]),
               (unsigned long long)(fb[5] - last_fb[5]),
               span > 0 ? 100.0 * (double)(ge - last_ge) / span : 0.0,
               span > 0 ? 100.0 * (double)(idle - last_idle) / span : 0.0, mb, ge_line);
    memcpy(last_fb, fb, sizeof fb);
    last_ge = ge; last_idle = idle; last_us = now;
    sum = 0; n = 0; mn = 1e9;
}

static void sample_fps(uint64_t vb) {
    if (vb % 60) return;
    uint64_t now = psp_sched_now_us(), flips = psp_display_flips();
    if (g_fps_last_us) {
        double fps = (double)(flips - g_fps_last_flips) * 1e6 / (double)(now - g_fps_last_us);
        if (g_fps_n < FPS_MAX_WIN) g_fps_win[g_fps_n++] = fps;
        if (g_fps_log < 0) g_fps_log = getenv("PSP2I_FPS_LOG") != NULL;
        if (g_fps_log) fprintf(stderr, "fps: %5.1f at vblank %llu (counter shows %.1f)\n", fps,
                               (unsigned long long)vb, fps_meter_value(&g_fpsm));
        if (!g_headless) perf_minute(fps);
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

static int dir_exists(const char *p) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)p;
    return 1;
#endif
}

/* The first *.iso next to the exe, in the root, or in the root's parent
 * (FinalBuild keeps the root at ..\GameData, the image beside it). */
static void find_iso(const char *dir, const char *root, char *out, size_t cap) {
#ifdef _WIN32
    char parent[700];
    snprintf(parent, sizeof parent, "%s/..", root);
    const char *const where[3] = { dir, root, parent };
    for (int i = 0; i < 3; i++) {
        char pat[760];
        snprintf(pat, sizeof pat, "%s\\*.iso", where[i]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        snprintf(out, cap, "%s\\%s", where[i], fd.cFileName);
        return;
    }
#else
    (void)dir; (void)root; (void)out; (void)cap;
#endif
}

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
    g_prof_on = g_prof_keyed ? g_prof_keyon : vb >= g_prof_from && (!g_prof_to || vb < g_prof_to);
    {   /* PSP2I_DRAW_DUMP_AT=VBLANK: F12's dump at that vblank (scripted runs) */
        static long long at = -2;
        if (at == -2) { const char *e = getenv("PSP2I_DRAW_DUMP_AT"); at = e ? atoll(e) : -1; }
        if (at >= 0 && vb == (uint64_t)at) g_dump_key = 1;
    }
    if (g_dump_key) { g_dump_key = 0; take_dump(vb); }
    {   /* PSP2I_EXTRACT_AT=VBLANK: EXTRACT MODELS at that vblank (scripted runs) */
        static long long at = -2;
        if (at == -2) { const char *e = getenv("PSP2I_EXTRACT_AT"); at = e ? atoll(e) : -1; }
        if (at >= 0 && vb == (uint64_t)at) {
            char dir[600], note[32];
            exe_dir(dir, sizeof dir);
            modelx_extract(dir, note, sizeof note);
        }
    }
#ifndef PSP2I_PROD
    rateaudit_vblank();
#endif
    draw_dist_apply();
    modelx_vblank();                      /* LOAD MODELS: replaced textures not placed yet */
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
        case LOGIN_ACT_REGISTER: online_register_submit(); break;
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
        const int p = addr ? gpu_present(addr, stride, (int)fmt, g_frame, SCREEN_W, SCREEN_H) : 0;
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
        else if (p == 0 && !g_headless) {
            /* From VRAM (130,000 reads): only for a new frame, or every 4th
             * vblank for a picture the CPU changes without flipping. */
            static uint64_t grabbed_flip = (uint64_t)-1;
            const uint64_t fl = psp_display_flips();
            if (fl != grabbed_flip || (vb & 3) == 0) { grabbed_flip = fl; have_frame = grab_frame(); }
        }
#endif
    }
#ifdef _WIN32
    if (!g_headless) {
        if (have_frame) g_have_frame = 1;
        if (g_have_frame) {
            /* Shown again only when the composed picture changed (a new game
             * frame, or the overlay: menu, counters): at 120 Hz most vblanks
             * repeat the last frame, and each show is an upload and a swap. */
            static uint32_t shown[SCREEN_W * SCREEN_H];
            static int shown_ok;
            compose();
            if (!shown_ok || have_frame || memcmp(shown, g_rgba, sizeof shown)) {
                if (window_show() != 0) gdi_paint();
                memcpy(shown, g_rgba, sizeof shown);
                shown_ok = 1;
            }
        }
#ifndef PSP2I_PROD
        if ((vb % 30) == 0) {                                /* dev builds: progress in the title */
            char title[160];
            snprintf(title, sizeof title, "PSP2i (recompiled) - vblank %llu, GE cmds %llu",
                     (unsigned long long)vb, (unsigned long long)psp_ge_command_count());
            SetWindowTextA(g_wnd, title);
        }
#endif
        window_pump();
    }
#endif
    for (int i = 0; i < g_noverlay_shot; i++) {
        const uint64_t at = g_shot_by_flip ? psp_display_flips() : vb;
        if (!g_overlay_shot[i] || g_overlay_shot[i] != at || !grab_frame()) continue;
        g_overlay_shot[i] = 0;                                  /* once (a flip spans several vblanks) */
        compose();
        char path[700];
        snprintf(path, sizeof path, "%s/overlay_%06llu.ppm", g_capture_dir, (unsigned long long)at);
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
    gl_ge_report(stderr);
    psp_ge_thread_report(stderr);
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
/* Symbols for these logs, without stalling the game: DbgHelp is initialised
 * and the exe's 100 MB PDB loaded on a background thread at start
 * (sym_preload); until then stacks are simply left out. Each return address
 * is symbolized once and remembered (the same few repeat endlessly), and
 * DbgHelp, which is single-threaded, is only used under g_sym_lock. Looking
 * up every frame of every HTTP / NP log line -- the PDB loading on the first
 * -- was the game-thread stutter on SEGA server traffic. */
static volatile LONG     g_sym_ready;
static CRITICAL_SECTION  g_sym_lock;

static DWORD WINAPI sym_preload(LPVOID arg) {
    (void)arg;
    EnterCriticalSection(&g_sym_lock);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 d = 0;
    SymFromAddr(proc, (DWORD64)(uintptr_t)&sym_preload, &d, si);   /* loads the exe's PDB now */
    LeaveCriticalSection(&g_sym_lock);
    InterlockedExchange(&g_sym_ready, 1);
    return 0;
}

static void sym_start(void) {
    InitializeCriticalSection(&g_sym_lock);
    HANDLE t = CreateThread(NULL, 0, sym_preload, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* A return address -> 1 and *fn (a recompiled game function), 2 (the guest
 * thread's bottom: stop), 0 (anything else). Remembered per address. */
#define SYM_CACHE 8192
static struct { uintptr_t pc; uint32_t fn; uint8_t kind, used; } g_sym_cache[SYM_CACHE];

static int sym_kind(HANDLE proc, uintptr_t pc, uint32_t *fn) {
    uint32_t h = (uint32_t)((pc >> 2) * 2654435761u) & (SYM_CACHE - 1);
    for (int probe = 0; probe < 16; probe++, h = (h + 1) & (SYM_CACHE - 1)) {
        if (!g_sym_cache[h].used) break;
        if (g_sym_cache[h].pc == pc) { *fn = g_sym_cache[h].fn; return g_sym_cache[h].kind; }
    }
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 d = 0;
    int kind = 0;
    unsigned a = 0;
    if (SymFromAddr(proc, (DWORD64)pc, &d, si)) {
        if (!strcmp(si->Name, "thread_body")) kind = 2;
        else if (sscanf(si->Name, "psp_body_%8X", &a) == 1 || sscanf(si->Name, "psp_func_%8X", &a) == 1) kind = 1;
    }
    for (int probe = 0; probe < 16; probe++, h = (h + 1) & (SYM_CACHE - 1))
        if (!g_sym_cache[h].used) {
            g_sym_cache[h].pc = pc; g_sym_cache[h].fn = a; g_sym_cache[h].kind = (uint8_t)kind; g_sym_cache[h].used = 1;
            break;
        }
    *fn = a;
    return kind;
}

int game_stack(char *out, size_t cap, int max_frames) {
    out[0] = '\0';
    if (!InterlockedCompareExchange(&g_sym_ready, 0, 0)) return 0;   /* symbols still loading: no stack */
    HANDLE proc = GetCurrentProcess();
    void *frames[48];
    const USHORT n = CaptureStackBackTrace(1, 48, frames, NULL);
    size_t used = 0;
    int k = 0;
    uint32_t last = 0, seen[48];
    EnterCriticalSection(&g_sym_lock);
    for (USHORT i = 0; i < n && k < max_frames; i++) {
        uint32_t a;
        const int kind = sym_kind(proc, (uintptr_t)frames[i], &a);
        if (kind == 2) break;
        if (kind == 1 && a != last) {
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
    LeaveCriticalSection(&g_sym_lock);
    return k;
}

/* --watch hits in a release build: name the recompiled functions on the host
 * stack (the host stack is the game's call stack), innermost first. */
/* The recompiled functions among `frames`, innermost first, as " name name ...". */
static void stack_names(void *const *frames, USHORT n, char *line, size_t cap) {
    HANDLE proc = GetCurrentProcess();
    size_t len = 0;
    line[0] = '\0';
    if (!InterlockedCompareExchange(&g_sym_ready, 0, 0)) { snprintf(line, cap, " (symbols still loading)"); return; }
    EnterCriticalSection(&g_sym_lock);
    for (USHORT i = 0; i < n; i++) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 d = 0;
        if (SymFromAddr(proc, (DWORD64)(uintptr_t)frames[i], &d, si)) {
            if (!strncmp(si->Name, "psp_", 4) && strncmp(si->Name, "psp_write", 9) && strncmp(si->Name, "psp_read", 8) &&
                len + strlen(si->Name) + 2 < cap)
                len += (size_t)snprintf(line + len, cap - len, " %s", si->Name);
            if (!strcmp(si->Name, "thread_body")) break;
        }
    }
    LeaveCriticalSection(&g_sym_lock);
}

/* Read-watch hits (psp_mem_watch_read): each distinct call stack once, with a
 * count every 600th repeat, so a reader that runs every frame stays readable. */
static void read_watch_hit(uint32_t watched, uint32_t addr) {
    static struct { uint64_t key; uint32_t hits; } seen[64];
    static int nseen, busy;
    if (busy) return;                      /* our own psp_read32 below reads the watched word */
    void *frames[16];
    const USHORT n = CaptureStackBackTrace(1, 16, frames, NULL);
    uint64_t key = watched;
    for (USHORT i = 0; i < n; i++) key = key * 1000003u ^ (uint64_t)(uintptr_t)frames[i];
    int k = 0;
    while (k < nseen && seen[k].key != key) k++;
    if (k < nseen) {
        if (++seen[k].hits % 600 == 0)
            gamelog_note("[rwatch] 0x%08X: stack #%d read %u times", watched, k, seen[k].hits);
        return;
    }
    if (nseen == 64) return;
    seen[k].key = key; seen[k].hits = 1; nseen++;
    busy = 1;
    char line[900];
    stack_names(frames, n, line, sizeof line);
    gamelog_note("[rwatch] 0x%08X read (at 0x%08X, value %u) at vblank %llu, stack #%d:%s", watched, addr,
                 psp_read32(watched), (unsigned long long)psp_sched_vblank_count(), k, line);
    busy = 0;
}

/* Unmapped accesses (psp_mem_set_bad_hook): each distinct call stack once in
 * game_log.txt, with the address, so a null-pointer read in game code shows
 * which code does it. */
static void bad_access_hit(uint32_t addr, int write, int width) {
    static struct { uint64_t key; uint32_t hits; } seen[48];
    static int nseen;
    void *frames[16];
    const USHORT n = CaptureStackBackTrace(1, 16, frames, NULL);
    uint64_t key = (uint64_t)write;
    for (USHORT i = 0; i < n; i++) key = key * 1000003u ^ (uint64_t)(uintptr_t)frames[i];
    int k = 0;
    while (k < nseen && seen[k].key != key) k++;
    if (k < nseen) {
        if (++seen[k].hits % 1000 == 0) gamelog_note("[bad] stack #%d: %u accesses", k, seen[k].hits);
        return;
    }
    if (nseen == 48) return;
    seen[k].key = key; seen[k].hits = 1; nseen++;
    char line[900];
    stack_names(frames, n, line, sizeof line);
    gamelog_note("[bad] %s%d at 0x%08X at vblank %llu, stack #%d:%s", write ? "write" : "read", width * 8, addr,
                 (unsigned long long)psp_sched_vblank_count(), k, line);
}

/* PSP2I_TRACE_VALUE=V (PSP2I_TRACE_FROM=vblank): every distinct stack that
 * writes a word whose low 24 bits are V -- e.g. a colour going into a display
 * list, to find the code that computes it. Dev runs only (every write is
 * checked). */
static uint32_t g_tv_value;
static uint64_t g_tv_from;
static void value_trace(uint32_t addr, uint32_t width, uint32_t value) {
    static uint32_t last_at;
    if (width != 4 && last_at && addr >= last_at && addr < last_at + 4) {   /* a part of that word written next (an alpha byte) */
        static int n;
        if (n++ < 40) {
            void *frames[62];
            const USHORT k = CaptureStackBackTrace(1, 62, frames, NULL);
            char line[900];
            stack_names(frames, k, line, sizeof line);
            fprintf(stderr, "value-trace:   then %u-byte 0x%X to 0x%08X at vblank %llu; stack:%s\n", width, value, addr,
                    (unsigned long long)psp_sched_vblank_count(), line);
        }
        last_at = 0;
        return;
    }
    if (width != 4 || (value & 0xFFFFFFu) != g_tv_value || psp_sched_vblank_count() < g_tv_from) return;
    last_at = addr;
    static uint64_t keys[32];
    static int nkeys;
    void *frames[62];
    const USHORT n = CaptureStackBackTrace(1, 62, frames, NULL);
    uint64_t key = 0;
    for (USHORT i = 0; i < n; i++) key = key * 1000003u ^ (uint64_t)(uintptr_t)frames[i];
    for (int k = 0; k < nkeys; k++) if (keys[k] == key) return;
    if (nkeys == 32) return;
    keys[nkeys++] = key;
    char line[900];
    stack_names(frames, n, line, sizeof line);
    fprintf(stderr, "value-trace: 0x%08X to 0x%08X at vblank %llu; stack:%s\n", value, addr,
            (unsigned long long)psp_sched_vblank_count(), line);
}
static void value_trace_init(void) {
    const char *v = getenv("PSP2I_TRACE_VALUE"), *f = getenv("PSP2I_TRACE_FROM");
    if (!v) return;
    g_tv_value = (uint32_t)strtoul(v, NULL, 0) & 0xFFFFFFu;
    g_tv_from = f ? strtoull(f, NULL, 0) : 0;
    psp_mem_trace_writes(value_trace);
}

static void watch_backtrace(uint32_t addr, uint32_t value) {
    void *frames[24];
    const USHORT n = CaptureStackBackTrace(1, 24, frames, NULL);
    char line[900];
    stack_names(frames, n, line, sizeof line);
    fprintf(stderr, "    stack:%s\n", line);
    /* also in the logs the user sends back */
    gamelog_note("[watch] write 0x%08X to 0x%08X at vblank %llu; stack:%s", value, addr,
                 (unsigned long long)psp_sched_vblank_count(), line);
}
#endif

/* ---- crash reporting ---------------------------------------------------------- */

#ifdef _WIN32
#include <dbghelp.h>

/* A host-side fault (access violation, stack overflow) is a bug in the
 * runtime or the generated code. Report where -- host function, PSP state,
 * thread table -- before the process dies, so it can be found. */
static char g_crash_path[700];          /* logs/crash_log.txt next to the exe */
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

/* crash_dump.dmp next to crash_log.txt: the faulting thread's stack and the
 * memory it points at, for a debugger. Written first -- heap corruption may
 * leave too little working to finish the text report. */
static void crash_minidump(EXCEPTION_POINTERS *ep) {
    if (!g_crash_path[0]) return;
    char path[720];
    snprintf(path, sizeof path, "%s", g_crash_path);
    char *s = strrchr(path, '/');
    snprintf(s ? s + 1 : path, sizeof path - (size_t)(s ? s + 1 - path : 0), "crash_dump.dmp");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = g_crash_tid;
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                      &mei, NULL, NULL);
    CloseHandle(f);
}

static DWORD WINAPI crash_report_thread(LPVOID p) { crash_report((EXCEPTION_POINTERS *)p); return 0; }

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    static int once;
    if (once++) return EXCEPTION_CONTINUE_SEARCH;
    g_crash_tid = GetCurrentThreadId();
    crash_minidump(ep);
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

/* Failures the unhandled-exception filter never sees, seen in multiplayer
 * (2026-10-10, Windows event log, no crash_log.txt): heap corruption
 * (0xC0000374, raised from ntdll and then fatal) and the C runtime's fast
 * fail (0xC0000409 in ucrtbase: an invalid parameter, or abort). The first
 * reaches a vectored handler; the others call ours before failing. */
static LONG WINAPI on_vectored(EXCEPTION_POINTERS *ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xC0000374u || code == 0xC0000409u) on_crash(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void crash_from_here(DWORD code, const char *what) {
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD er;
    memset(&er, 0, sizeof er);
    er.ExceptionCode = code;
    er.ExceptionAddress = (void *)(uintptr_t)ctx.Rip;
    EXCEPTION_POINTERS ep = { &er, &ctx };
    fprintf(stderr, "psp2i: %s\n", what);
    on_crash(&ep);
}

static void on_invalid_parameter(const wchar_t *expr, const wchar_t *fn, const wchar_t *file, unsigned line, uintptr_t r) {
    (void)expr; (void)fn; (void)file; (void)line; (void)r;
    crash_from_here(0xC0000417u, "the C runtime was given an invalid parameter");
}

static void on_abort_signal(int sig) {
    (void)sig;
    crash_from_here(0xC0000409u, "abort()");
}

static void crash_handlers_install(void) {
    SetUnhandledExceptionFilter(on_crash);
    AddVectoredExceptionHandler(1, on_vectored);
    _set_invalid_parameter_handler(on_invalid_parameter);
    signal(SIGABRT, on_abort_signal);
}
#endif

/* ---- sceKernelLoadExec: "open website" ------------------------------------------
 * The game replaces itself with another program. PSP2i does it for one thing:
 * disc0:/PSP_GAME/USRDIR/module/browser.prx, which opened its website in the
 * PSP browser and then loaded the game again. Here the one window shows
 * whatever runs: the page (web_overlay.cpp; website_url= in
 * psp2i_online.ini) while it is open, then the game booting again in place
 * (main's reboot loop) -- as the PSP did. The game has already torn itself
 * down when it calls this, so there is nothing to return to. */
static int g_reboot;                  /* the run ended in LoadExec: boot the game again */

static void loadexec_host(const char *path, uint32_t argsize, uint32_t argp) {
    (void)argp;
    online_log("game: sceKernelLoadExec(%s, %u argument bytes)", path, argsize);
#ifdef _WIN32
    if (g_headless || !g_wnd) return;                   /* nothing to show; the game ends */
    const size_t n = strlen(path);
    if (n >= 11 && !_stricmp(path + n - 11, "browser.prx")) {
        audio_set_master(0.0f);                         /* the game is gone; no stuck sound under the page */
        web_overlay_show(g_wnd, online_website_url(), NULL);
        if (psp_exit_requested()) return;               /* the window was closed while the page was up */
    }
    /* browser.prx's own last step (and any other program): the game again */
    g_reboot = 1;
#else
    (void)path;
#endif
}

/* The game's PS Store (psp_np_set_store): on a PSP the firmware's store, drawn
 * over the game while it stays in the PSN lobby. Here: store_url= of
 * psp2i_online.ini ({title} = the NP title ID) over the running game
 * (web_overlay_open), the game's input held back meanwhile (ctrl path). */
static int store_open(const char *title_id) {
#ifdef _WIN32
    const char *url = online_store_url();
    if (!url[0] || g_headless || !g_wnd) return -1;     /* none configured: the dialog just finishes */
    char full[1024];
    const char *t = strstr(url, "{title}");
    if (t) snprintf(full, sizeof full, "%.*s%s%s", (int)(t - url), url, title_id, t + 7);
    else snprintf(full, sizeof full, "%s", url);
    online_log("game: PS Store -> %s (over the game)", full);
    return web_overlay_open(g_wnd, full);
#else
    (void)title_id;
    return -1;
#endif
}
static int store_showing(void) { return web_overlay_poll(); }

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

/* The guest side of a boot: fresh memory and CPU, the EBOOT (eboot, else the
 * opened disc image's), the firmware state, the recompiled functions, and the
 * boot path module_start receives (*argp, its length *arglen). Run at start
 * and again when the game reboots in place (g_reboot); the window, renderer,
 * sound, menu and online state are the host's and stay. 0 on success. */
static const char BOOT_PATH[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";

static int boot_guest(const char *eboot, const char *iso, const char *root, const char *ms_dir,
                      module_info *mi, uint32_t *argp) {
    if (!psp_mem.ram) {
        if (psp_mem_init() != 0) { fprintf(stderr, "psp2i: psp_mem_init failed\n"); return 1; }
    } else {                                  /* a reboot: the same buffers (the host may hold pointers), cleared */
        memset(psp_mem.ram, 0, PSP_RAM_SIZE);
        memset(psp_mem.vram, 0, PSP_VRAM_SIZE);
        memset(psp_mem.scratch, 0, PSP_SCRATCH_SIZE);
    }
    psp_cpu_reset();
    if (eboot[0]) {
        if (load_elf(eboot, mi) != 0) return 1;
    } else {
        uint32_t len = 0;
        uint8_t *b = psp_io_disc_file("PSP_GAME/SYSDIR/EBOOT.BIN", &len);
        if (!b) { fprintf(stderr, "psp2i: %s has no PSP_GAME/SYSDIR/EBOOT.BIN\n", iso); return 1; }
        if (load_elf_buf(b, len, "the disc image's EBOOT.BIN", mi) != 0) return 1;
    }
    printf("module '%s': entry 0x%08X, gp 0x%08X, image 0x%08X..0x%08X\n",
           mi->name, mi->entry, mi->gp, mi->load_lo, mi->load_hi);

    psp_hle_init();
    psp_io_set_root(root);
    {
        const char *e = getenv("PSP2I_VFPU_NOINLINE");     /* A/B: runtime VFPU calls only */
        if (e && *e == '1') { psp_vfpu_force_runtime(1); printf("vfpu: inline operations disabled (PSP2I_VFPU_NOINLINE=1)\n"); }
    }
    if (ms_dir[0]) { psp_io_set_ms_root(ms_dir); printf("ms:    %s\n", ms_dir); }
    psp_sysmem_set_heap(mi->load_hi, USER_PARTITION_TOP);
    psp_recomp_register();

    /* argv[0]: the path the kernel booted the module from, copied into guest
     * memory; module_start receives (length including NUL, pointer). */
    *argp = psp_sysmem_alloc(0x100, 1);
    psp_mem_write_block(*argp, BOOT_PATH, (uint32_t)sizeof BOOT_PATH);
    return 0;
}

int main(int argc, char **argv) {
    char dir[512], root[600] = "", eboot[700] = "", ms_dir[600] = "";   /* --ms: memory stick elsewhere (tests) */
    char iso[700] = "";                    /* --iso, or found: the disc image */
    int extracted = 0;                     /* --extracted: the disc folder even with an ISO about */
    uint32_t oracle_addr = 0;
    const char *renderer = NULL;            /* --renderer; else psp2i_display.ini */
    int res_arg = -1;                       /* --resolution; else psp2i_display.ini */
    int oracle_all = 0;
    int want_sdl = -1;                     /* -1: default (with a window) */
    int want_audio = -1;
    int fps = 30, fps_arg = 0;
    const char *replay_path = NULL;
    int show_fps = 1;                      /* the FPS counter: shown unless --hide-fps */
    float rs_speed = 1.0f;                 /* right-stick camera sensitivity */
    int rs_arg = 0;                        /* --rs-speed given: it wins over the saved one */
    uint32_t dump_addr = 0, args_addr = 0;
    uint64_t args_flip = 0;
    const char *dump_path = NULL;
    exe_dir(dir, sizeof dir);
#ifdef _WIN32
    log_path(g_crash_path, sizeof g_crash_path, dir, "crash_log.txt");   /* logs/crash_log.txt; the dump goes beside it */
#endif
#ifdef PSP2I_PROD
    /* Community build: no command line (see prod.c) except --console, which
     * shows the console window with the engine's messages. The exe is a
     * window-only program there, so without it no console appears. */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--console") && AllocConsole()) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            SetConsoleTitleA(WINDOW_TITLE " - console");
        }
    argc = 1;
#endif

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--root") && i + 1 < argc)          snprintf(root, sizeof root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--eboot") && i + 1 < argc)         snprintf(eboot, sizeof eboot, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--iso") && i + 1 < argc)           snprintf(iso, sizeof iso, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--extracted"))                     extracted = 1;
        else if (!strcmp(argv[i], "--ms") && i + 1 < argc)            snprintf(ms_dir, sizeof ms_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--headless"))                      g_headless = 1;
        else if (!strcmp(argv[i], "--sdl"))                           want_sdl = 1;
        else if (!strcmp(argv[i], "--no-sdl"))                        want_sdl = 0;
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)           { fps = atoi(argv[++i]); fps_arg = 1; }
        else if (!strcmp(argv[i], "--replay-input") && i + 1 < argc)  replay_path = argv[++i];
        else if (!strcmp(argv[i], "--show-fps"))                      show_fps = 1;
        else if (!strcmp(argv[i], "--rs-speed") && i + 1 < argc)      { rs_speed = (float)atof(argv[++i]); rs_arg = 1; }
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
        else if (!strcmp(argv[i], "--shot-by-flip"))                  g_shot_by_flip = 1;
        else if (!strcmp(argv[i], "--vertex-work") && i + 1 < argc)   g_vertex_arg = strcmp(argv[++i], "cpu") != 0;
        else if (!strcmp(argv[i], "--ge-thread") && i + 1 < argc)     g_ge_thread_arg = strcmp(argv[++i], "off") != 0;
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
        else { fprintf(stderr, "usage: %s [--root DIR] [--iso FILE | --extracted] [--eboot FILE] [--headless] [--seconds N] "
                               "[--capture-every N] [--capture-dir DIR]\n", argv[0]); return 2; }
    }
#ifdef PSP2I_PROD
    if (prod_check(dir, root, sizeof root, eboot, sizeof eboot, iso, sizeof iso) != 0) return 1;
#endif
    if (!root[0]) {
        snprintf(root, sizeof root, "%s/GameData", dir);
        if (!dir_exists(root)) snprintf(root, sizeof root, "GameData");
    }
    if (!iso[0] && !extracted) find_iso(dir, root, iso, sizeof iso);
    if (extracted) iso[0] = '\0';
    if (!eboot[0]) {
        snprintf(eboot, sizeof eboot, "%s/EBOOT.BIN", dir);
        if (!exists(eboot)) {
            if (iso[0]) eboot[0] = '\0';                     /* the image's own */
            else snprintf(eboot, sizeof eboot, "%s/disc/PSP_GAME/SYSDIR/EBOOT.BIN", root);
        }
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
    crash_handlers_install();
    /* The scheduler sleeps until the next vblank or timeout with Sleep(ms - 1)
     * and spins the rest (threadman.c host_sleep_us), which assumes the 1 ms
     * timer resolution. At Windows' default (15.6 ms) one Sleep(1) can take a
     * whole vblank: frames that finished in time were shown a vblank late,
     * costing about 20 fps in the lobby at 60 fps. Only --profile set this
     * before, which is why profiled runs looked faster. */
    timeBeginPeriod(1);
    /* No screensaver, display off or sleep while the game runs: a gamepad
     * doesn't count as activity to Windows, and a replay has no input at all
     * (the screensaver broke replays). */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    sym_start();                          /* symbols for the logs' game stacks, loaded in the background */
#ifndef PSP2I_PROD                        /* test hooks (--watch, the HP probe's read watch, bad accesses) */
    psp_mem_set_watch_hook(watch_backtrace);
    value_trace_init();
    { const char *rw = getenv("PSP2I_READ_WATCH"); if (rw) psp_mem_watch_read((uint32_t)strtoul(rw, NULL, 0)); }   /* dev: who reads this word */
    psp_mem_set_read_watch_hook(read_watch_hit);
    psp_mem_set_bad_hook(bad_access_hit);
#endif
    psp_set_loadexec_hook(loadexec_host);
    psp_np_set_store(store_open, store_showing);
#endif
    printf("root:  %s\ndisc:  %s\neboot: %s\n", root, iso[0] ? iso : "extracted (root/disc)",
           eboot[0] ? eboot : "from the disc image");

    module_info mi;
    if (iso[0] && psp_io_open_disc_image(iso) != 0) {
        fprintf(stderr, "psp2i: %s is not a PSP disc image (.iso)\n", iso);
        return 1;
    }
    uint32_t argp = 0;
    if (boot_guest(eboot, iso, root, ms_dir, &mi, &argp) != 0) return 1;
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

    snprintf(g_display_ini, sizeof g_display_ini, "%s/psp2i_display.ini", dir);
    int res = display_load();
    if (res_arg >= 0) res = res_arg;
    if (res < 0) res = RES_DEFAULT;
    if (res != RES_FULLSCREEN) g_last_windowed = res;
    /* The renderer: --renderer for this run, else the saved choice. GPU
     * rendering by default; the software rasterizer is the fallback and the
     * reference (--renderer software). */
    g_renderer = renderer ? menu_ren_parse(renderer) : g_ren_saved;
    if (renderer && !strcmp(renderer, "software")) g_renderer = REN_SOFTWARE;
    if (g_renderer < 0) { fprintf(stderr, "psp2i: unknown renderer '%s'; using d3d11\n", renderer); g_renderer = REN_D3D11; }
    if (g_renderer == REN_GL && (gl_platform_init() != 0 || gl_ge_init() != 0)) {
        fprintf(stderr, "psp2i: OpenGL unavailable; falling back to Direct3D 11\n");
        g_renderer = REN_D3D11;
    }
#ifdef _WIN32
    if (!g_headless) window_open(res);
    if (g_ge_thread_arg >= 0) g_ge_thread = g_ge_thread_arg;
    if (g_renderer == REN_GL && g_wnd) {
        g_gl_window = gl_platform_attach(g_wnd) == 0;
        if (!g_gl_window) present_init(g_wnd);
    }
    if (g_renderer == REN_GL && g_ge_thread) {          /* the context goes with the lists */
        gl_platform_release();
        if (psp_ge_thread_start(ge_thread_gl_init) != 0) { gl_platform_bind(); fprintf(stderr, "gl: no GE thread\n"); }
    }
    if (g_renderer == REN_D3D11 && d3d11_init() != 0) {
        fprintf(stderr, "psp2i: falling back to the software renderer\n");
        g_renderer = REN_SOFTWARE;
    }
#endif
    fprintf(stderr, "renderer: %s\n", g_renderer == REN_GL ? "OpenGL" : g_renderer == REN_D3D11 ? "Direct3D 11" : "software");
    if (want_sdl == 1 || (want_sdl < 0 && !g_headless)) g_sdl_on = input_sdl_init() == 0;
    audio_init(want_audio == 1 || (want_audio < 0 && !g_headless));
    atrac_at3_init(dir);           /* ATRAC music (psp2i_atrac.dll); silent without it */
    h264_openh264_init(dir, g_openh264);   /* movies; black (with sound) until it is ready */
    if (replay_path) {                         /* a recorded route: its fps unless --fps */
        const int rf = replay_load(replay_path);
        if (rf < 0) return 1;
        if (rf > 0 && !fps_arg) fps = rf;
    }
    {
        const char *rp = getenv("PSP2I_INPUT_RECORD");
        if (rp && *rp && !replay_path) rec_open(rp, fps);
    }
    menu_init(&g_menu, fps, show_fps);
    g_menu.res = res;
    g_menu.renderer = g_ren_saved;
    g_menu.renderer_now = g_renderer;
    for (int k = 0; k < VOL_N; k++) g_menu.vol[k] = g_vol_saved[k];
    g_menu.att = g_att_saved;
    g_menu.swap_oc = g_swap_oc;
    input_sdl_set_swap_circle_cross(g_swap_oc);
    audio_apply();
    {   /* the game's text font: the saved choice, else Windows' own */
        char fonts_dir[700], pgf_dir[700];
        snprintf(fonts_dir, sizeof fonts_dir, "%s/fonts", dir);
        snprintf(pgf_dir, sizeof pgf_dir, "%s/flash/font", root);
#ifdef _WIN32
        CreateDirectoryA(fonts_dir, NULL);     /* where extra fonts go */
#endif
        g_menu.font_n = font_win_init(fonts_dir, pgf_dir);
        int fi = font_win_find(g_font_key);
        if (fi < 0) { fprintf(stderr, "font: \"%s\" not found; using the system font\n", g_font_key); fi = 0; }
        g_menu.font = font_win_select(fi);
        snprintf(g_menu.font_name, sizeof g_menu.font_name, "%s", font_win_name(g_menu.font));
    }
    fps_meter_init(&g_fpsm);
    framerate_init(fps);
    framerate_set_log(gamelog_note);
    menu_set_rs_speed(&g_menu, !rs_arg && g_rs_saved > 0.0f ? g_rs_saved : rs_speed);
    g_menu.rs_on = g_rs_on;
    menu_set_deadzone_pct(&g_menu, g_deadzone_pct);
    g_menu.mouse_cam = g_mouse_cam;
#ifdef _WIN32
    keys_to_menu();
#else
    g_menu.hidden |= MENU_BIT(MENU_KEYS);
#endif
    g_menu.test_ticks = g_test_ticks;
    g_menu.draw_dist = g_draw_dist;
    if (g_vertex_arg >= 0) g_gpu_vertex = g_vertex_arg;
    g_menu.gpu_vertex = g_gpu_vertex;
    psp_gpu_set_gpu_transform(g_gpu_vertex);
    postfx_set(&g_post);                  /* clamps what the ini held */
    g_post = *postfx_get();
    g_menu.post = g_post;
    g_menu.test_logic = g_test_logic;
#ifdef PSP2I_PROD
    g_menu.hidden |= MENU_BIT(MENU_TESTS);              /* the frame-rate tests are for development builds */
    g_test_ticks = g_test_logic = 0;
#endif
    framerate_set_tests(g_test_ticks, g_test_logic);
    camera_init(menu_rs_speed(&g_menu));
    /* PSP savedata encryption: saves interchangeable with a PSP / PPSSPP */
    static const psp_savedata_crypto SAVE_CRYPTO = { savecrypt_decrypt, savecrypt_encrypt, savecrypt_sfo_hash };
    psp_savedata_set_crypto(&SAVE_CRYPTO);
    online_init(dir, &g_login);
    gamelog_init(dir);
    modelx_init();                        /* model names as packages load (EXTRACT MODELS) */
    {
        char xd[600];
        exe_dir(xd, sizeof xd);
        modelx_start(xd, g_load_models);  /* LOAD MODELS: Data/ models in place of the game's */
        g_menu.load_models = g_load_models;
        if (g_renderer == REN_GL) gl_ge_set_texture_source(modelx_texture, modelx_texture_gen);
    }
    functrace_init(dir);
#ifndef PSP2I_PROD
    rateaudit_init(dir);                  /* the vblank-use census, and F7's frame-rate audit */
#endif
    psp_sched_set_vblank_hook(on_vblank);

    int rc;
    for (;;) {
        printf("starting module_start at 0x%08X\n", mi.entry);
        fflush(stdout);
        rc = psp_sched_run(mi.entry, (uint32_t)sizeof BOOT_PATH, argp, 0x20, 0x40000, mi.gp);
#ifdef _WIN32
        if (g_close_at) g_game_stopped = 1;
#endif
        if (!g_reboot || g_close_at || g_quit_at) break;
        /* LoadExec (the website): the game again, in the same window */
        g_reboot = 0;
        online_log("game: booting the game again (after the website)");
        if (boot_guest(eboot, iso, root, ms_dir, &mi, &argp) != 0) { rc = 1; break; }
        audio_apply();                          /* the volume muted while the page was up */
    }

#ifdef _WIN32
    /* Closing: how long each step took (a slow one shows in online_log.txt). */
    const ULONGLONG t_stop = GetTickCount64();
    if (g_close_at) online_log("exit: window closed; the game stopped after %llu ms", t_stop - g_close_at);
    else if (g_quit_at) online_log("exit: QUIT GAME; the game stopped after %llu ms",
                                   (unsigned long long)((psp_sched_now_us() - g_quit_at) / 1000));
#endif
    report();
#ifdef _WIN32
    timeEndPeriod(1);
    const ULONGLONG t_report = GetTickCount64();
#endif
    /* Sound first: input_sdl_shutdown ends with SDL_Quit, which frees the
     * audio streams too; destroying them after that freed them twice
     * (heap corruption 0xC0000374 on nearly every exit, and the crash
     * dump it wrote was the pause after closing the window). */
    audio_shutdown();
    if (g_sdl_on) input_sdl_shutdown();
#ifdef _WIN32
    const ULONGLONG t_devices = GetTickCount64();
#endif
    psp_mem_free();
#ifdef _WIN32
    if (g_close_at || g_quit_at)
        online_log("exit: report %llu ms, controllers and sound %llu ms, memory %llu ms",
                   t_report - t_stop, t_devices - t_report, GetTickCount64() - t_devices);
    /* What is left (Windows unloading the libraries, their threads) only
     * delays the process ending; the logs are written, so end it here. */
    fflush(NULL);
    if (g_close_at || g_quit_at) TerminateProcess(GetCurrentProcess(), (UINT)rc);
#endif
    return rc;
}
