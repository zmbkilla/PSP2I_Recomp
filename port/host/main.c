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
 *   --audio / --no-audio  sound through SDL3.dll (audio_sdl.c): on by default
 *               with a window, off headless; PSP2I_AUDIO_DUMP=file.wav records
 *   --sdl / --no-sdl  controllers through SDL3.dll (input_sdl.c): on by default
 *               with a window, off headless unless --sdl
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
#include <psprecomp/render.h>

#include "recomp_funcs.h"
#include "input_sdl.h"
#include "audio_sdl.h"
#include "atrac_ffmpeg.h"

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

static uint32_t g_rgba[SCREEN_W * SCREEN_H];   /* 0xAARRGGBB for GDI */

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
            g_rgba[y * SCREEN_W + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return 1;
}

static void ctrl_commit(void);
static int g_dump_key;                     /* F12 pressed: capture on the next vblank */

#ifdef _WIN32
static HWND g_wnd;
static uint32_t g_keys;

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
    case WM_KEYDOWN:
        if (wp == VK_F12 && !(lp & (1 << 30))) g_dump_key = 1;   /* not on auto-repeat */
        g_keys |= key_bit(wp);  ctrl_commit(); return 0;
    case WM_KEYUP:   g_keys &= ~key_bit(wp); ctrl_commit(); return 0;
    case WM_CLOSE:   psp_request_exit(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = SCREEN_W;
        bi.bmiHeader.biHeight = -SCREEN_H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, SCREEN_W, SCREEN_H,
                      g_rgba, &bi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void window_open(void) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "psp2i";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = { 0, 0, SCREEN_W * 2, SCREEN_H * 2 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA("psp2i", "PSP2i (recompiled)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                          NULL, NULL, wc.hInstance, NULL);
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

/* Scripted input for headless runs: --press BUTTON@VBLANK[+FRAMES] holds a
 * button from that vblank for FRAMES vblanks (default 6). Repeatable. */
#define MAX_PRESS 128
static struct { uint32_t bits; uint64_t at, len; } g_press[MAX_PRESS];
static int g_npress;
static uint32_t g_script_bits;

static int add_press(const char *spec) {
    static const struct { const char *name; uint32_t bit; } B[] = {
        { "select", 0x0001 }, { "start", 0x0008 }, { "up", 0x0010 }, { "right", 0x0020 },
        { "down", 0x0040 }, { "left", 0x0080 }, { "l", 0x0100 }, { "r", 0x0200 },
        { "triangle", 0x1000 }, { "circle", 0x2000 }, { "cross", 0x4000 }, { "square", 0x8000 },
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

static void apply_script(uint64_t vb) {
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
    psp_ctrl_set(bits, g_pad_ax, g_pad_ay);
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
        if (g_fps_log) fprintf(stderr, "fps: %5.1f at vblank %llu\n", fps, (unsigned long long)vb);
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

static void on_vblank(void) {
    uint64_t vb = psp_sched_vblank_count();
    if (g_dump_key) { g_dump_key = 0; take_dump(vb); }
    if (g_find_vblank && vb == g_find_vblank) find_word();
    if (g_watch_late_addr && psp_display_flips() >= g_watch_late_flip) {
        /* --watch-from-flip: arm the write watch only once the game is there. */
        psp_mem_watch_write(g_watch_late_addr);
        g_watch_late_addr = 0;
    }
    apply_script(vb);
    poll_controllers();
    sample_fps(vb);
    {
        /* The displayed framebuffer is about to be read (window, captures):
         * bring emulated VRAM up to date with the GPU copy. A no-op unless the
         * GPU drew to it since the last sync, so this costs one readback per
         * new frame -- and is done headless too, so benchmarks pay it. */
        const psp_gpu_backend *be = psp_gpu_get_backend();
        uint32_t addr, stride, fmt;
        psp_display_get(&addr, &stride, &fmt);
        if (be && addr) be->sync_vram(addr, 272u * stride * (fmt == 3 ? 4u : 2u));
    }
#ifdef _WIN32
    if (!g_headless) {
        if (grab_frame()) InvalidateRect(g_wnd, NULL, FALSE);
        if ((vb % 30) == 0) {
            char title[160];
            snprintf(title, sizeof title, "PSP2i (recompiled) - vblank %llu, GE cmds %llu",
                     (unsigned long long)vb, (unsigned long long)psp_ge_command_count());
            SetWindowTextA(g_wnd, title);
        }
        window_pump();
    }
#endif
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

static DWORD WINAPI prof_thread(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    while (g_prof_run) {
        Sleep(1);
        if (SuspendThread(g_prof_target) == (DWORD)-1) continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_prof_target, &ctx)) {
            DWORD64 ip = ctx.Rip;
            uint32_t h = (uint32_t)((ip >> 2) * 2654435761u) & (PROF_SLOTS - 1);
            for (int k = 0; k < 64; k++, h = (h + 1) & (PROF_SLOTS - 1)) {
                if (g_prof[h].ip == ip) { g_prof[h].n++; break; }
                if (!g_prof[h].ip) { g_prof[h].ip = ip; g_prof[h].n = 1; break; }
            }
            g_prof_total++;
        }
        ResumeThread(g_prof_target);
    }
    timeEndPeriod(1);
    return 0;
}

static void prof_start(void) {
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
    for (int i = 0; i < nfn && i < 25; i++)
        fprintf(stderr, "    %5.1f%%  %s\n", 100.0 * fn[i].n / (double)g_prof_total, fn[i].name);
    fprintf(stderr, "  top lines:\n");
    for (int i = 0; i < nln && i < 40; i++)
        fprintf(stderr, "    %5.1f%%  %s\n", 100.0 * ln[i].n / (double)g_prof_total, ln[i].name);
}
#endif

/* ---- crash reporting ---------------------------------------------------------- */

#ifdef _WIN32
#include <dbghelp.h>

/* A host-side fault (access violation, stack overflow) is a bug in the
 * runtime or the generated code. Report where -- host function, PSP state,
 * thread table -- before the process dies, so it can be found. */
static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    static int once;
    if (once++) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    fprintf(stderr, "\n==== host exception 0x%08lX at %p ====\n",
            (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, "  %s of %p\n", er->ExceptionInformation[0] ? "write" : "read",
                (void *)er->ExceptionInformation[1]);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (SymInitialize(proc, NULL, TRUE)) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, (DWORD64)(uintptr_t)er->ExceptionAddress, &disp, si))
            fprintf(stderr, "  in %s+0x%llx\n", si->Name, (unsigned long long)disp);
    }
    fprintf(stderr, "  last recompiled function entered: 0x%08X\n", psp_trace_last());
    for (int i = 0; i < 32; i += 4)
        fprintf(stderr, "  %-4s 0x%08X  %-4s 0x%08X  %-4s 0x%08X  %-4s 0x%08X\n",
                psp_reg_names[i], psp_cpu.r[i], psp_reg_names[i + 1], psp_cpu.r[i + 1],
                psp_reg_names[i + 2], psp_cpu.r[i + 2], psp_reg_names[i + 3], psp_cpu.r[i + 3]);
    psp_sched_dump(stderr);
    fflush(stderr);
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
    char dir[512], root[600] = "", eboot[700] = "";
    uint32_t oracle_addr = 0;
    const char *renderer = "d3d11";
    int oracle_all = 0;
    int want_sdl = -1;                     /* -1: default (with a window) */
    int want_audio = -1;
    uint32_t dump_addr = 0, args_addr = 0;
    uint64_t args_flip = 0;
    const char *dump_path = NULL;
    exe_dir(dir, sizeof dir);

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--root") && i + 1 < argc)          snprintf(root, sizeof root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--eboot") && i + 1 < argc)         snprintf(eboot, sizeof eboot, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--headless"))                      g_headless = 1;
        else if (!strcmp(argv[i], "--sdl"))                           want_sdl = 1;
        else if (!strcmp(argv[i], "--no-sdl"))                        want_sdl = 0;
        else if (!strcmp(argv[i], "--audio"))                         want_audio = 1;
        else if (!strcmp(argv[i], "--no-audio"))                      want_audio = 0;
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
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) {
            if (add_press(argv[++i]) != 0) { fprintf(stderr, "bad --press %s (want e.g. cross@600+6)\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--oracle-all"))                    oracle_all = 1;
#ifdef _WIN32
        else if (!strcmp(argv[i], "--profile"))                       prof_start();
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
    if (g_capture_every) host_mkdir(g_capture_dir);
    {
        char ms[700];
        snprintf(ms, sizeof ms, "%s/ms", root);          host_mkdir(ms);
        snprintf(ms, sizeof ms, "%s/ms/PSP", root);      host_mkdir(ms);
        snprintf(ms, sizeof ms, "%s/ms/PSP/SAVEDATA", root); host_mkdir(ms);
    }

#ifdef _WIN32
    SetUnhandledExceptionFilter(on_crash);
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

#ifdef _WIN32
    if (!g_headless) window_open();
#endif
    if (want_sdl == 1 || (want_sdl < 0 && !g_headless)) g_sdl_on = input_sdl_init() == 0;
    audio_init(want_audio == 1 || (want_audio < 0 && !g_headless));
    atrac_ffmpeg_init(dir);           /* ATRAC music; silent if FFmpeg is absent */
    psp_sched_set_vblank_hook(on_vblank);

    printf("starting module_start at 0x%08X\n", mi.entry);
    fflush(stdout);
    int rc = psp_sched_run(mi.entry, (uint32_t)sizeof boot_path, argp, 0x20, 0x40000, mi.gp);

    report();
    if (g_sdl_on) input_sdl_shutdown();
    audio_shutdown();
    psp_mem_free();
    return rc;
}
