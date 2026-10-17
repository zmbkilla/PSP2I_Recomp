/* The game's text font on Windows, drawn by GDI. See font_win.h. */

#include "font_win.h"

#include "psprecomp/font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define MAX_CHOICES 64
#define NFALLBACK   4

/* The PSP font's size (jpn0.pgf): capitals about 14 pixels tall, full-width
 * characters 18 pixels apart, an 18 pixel line. */
#define CAP_PX   14
#define CJK_EM   18
#define LINE_PX  18

enum { CH_SYSTEM, CH_PSP, CH_FILE };
typedef struct {
    int     kind;
    char    name[40];          /* menu */
    char    key[MAX_PATH];     /* ini */
    wchar_t path[MAX_PATH];    /* CH_FILE: the font file */
    int     added;             /* CH_FILE: registered with GDI (AddFontResourceEx) */
    wchar_t family[LF_FACESIZE];
} choice;

static choice g_ch[MAX_CHOICES];
static int    g_n;

/* Characters the chosen font lacks: Japanese, then Korean. */
static const wchar_t *const FALLBACK[NFALLBACK] = { L"Yu Gothic UI", L"Meiryo", L"MS Gothic", L"Malgun Gothic" };

/* The fonts drawing now: [0] the chosen one, then the fallbacks present. */
static HDC   g_dc;
static HFONT g_font[1 + NFALLBACK];
static int   g_nfont;

/* ---- the choices ------------------------------------------------------------ */

static int has_ext(const wchar_t *s, const wchar_t *ext) {
    const size_t n = wcslen(s), e = wcslen(ext);
    return n > e && !_wcsicmp(s + n - e, ext);
}

/* "My Font.ttf" -> "MY FONT" (the menu draws ASCII; others become '?'). */
static void menu_name(const wchar_t *file, char *out, size_t cap) {
    size_t o = 0;
    const wchar_t *dot = wcsrchr(file, L'.');
    for (const wchar_t *p = file; *p && p != dot && o + 1 < cap; p++) {
        wchar_t c = *p;
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - 32);
        out[o++] = c < 0x80 ? (char)c : '?';
    }
    out[o] = '\0';
}

int font_win_init(const char *fonts_dir, const char *pgf_dir) {
    g_n = 0;
    choice *c = &g_ch[g_n++];
    memset(c, 0, sizeof *c);
    c->kind = CH_SYSTEM;
    snprintf(c->name, sizeof c->name, "SYSTEM");
    snprintf(c->key, sizeof c->key, "system");

    char p[MAX_PATH + 32];
    snprintf(p, sizeof p, "%s/jpn0.pgf", pgf_dir);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
        c = &g_ch[g_n++];
        memset(c, 0, sizeof *c);
        c->kind = CH_PSP;
        snprintf(c->name, sizeof c->name, "PSP");
        snprintf(c->key, sizeof c->key, "psp");
    }

    wchar_t dir[MAX_PATH], pat[MAX_PATH + 4];
    if (!MultiByteToWideChar(CP_UTF8, 0, fonts_dir, -1, dir, MAX_PATH)) return g_n;
    _snwprintf(pat, MAX_PATH + 4, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return g_n;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!has_ext(fd.cFileName, L".ttf") && !has_ext(fd.cFileName, L".otf") && !has_ext(fd.cFileName, L".ttc")) continue;
        if (g_n >= MAX_CHOICES) break;
        c = &g_ch[g_n];
        memset(c, 0, sizeof *c);
        c->kind = CH_FILE;
        _snwprintf(c->path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        c->path[MAX_PATH - 1] = 0;
        menu_name(fd.cFileName, c->name, sizeof c->name);
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, c->key, sizeof c->key, NULL, NULL)) continue;
        g_n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    fprintf(stderr, "font: %d choice(s); extra fonts from %s\n", g_n, fonts_dir);
    return g_n;
}

int font_win_count(void) { return g_n; }
const char *font_win_name(int i) { return i >= 0 && i < g_n ? g_ch[i].name : "?"; }
const char *font_win_key(int i)  { return i >= 0 && i < g_n ? g_ch[i].key : "system"; }

int font_win_find(const char *key) {
    char k[MAX_PATH];
    snprintf(k, sizeof k, "%s", key);
    k[strcspn(k, "\r\n")] = '\0';
    for (int i = 0; i < g_n; i++)
        if (!_stricmp(g_ch[i].key, k)) return i;
    return -1;
}

/* ---- a font file's family name ------------------------------------------------
 * GDI selects a font by family, so a file from the fonts folder is registered
 * for this process (AddFontResourceEx) and its family read from the 'name'
 * table: name ID 1, Windows platform, US English preferred. */

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }

static int family_from_file(const wchar_t *path, wchar_t *out, size_t cap) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 12 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return -1; }
    fclose(f);
    const size_t size = (size_t)n;
    int rc = -1;
    size_t font = 0;
    if (!memcmp(d, "ttcf", 4) && size >= 16) font = be32(d + 12);    /* a collection: its first font */
    if (font + 12 > size) goto done;
    const unsigned ntab = be16(d + font + 4);
    size_t name = 0, name_len = 0;
    for (unsigned t = 0; t < ntab && font + 12 + (t + 1) * 16 <= size; t++) {
        const uint8_t *r = d + font + 12 + t * 16;
        if (!memcmp(r, "name", 4)) { name = be32(r + 8); name_len = be32(r + 12); }
    }
    if (!name || name + 6 > size || name + name_len > size) goto done;
    const unsigned count = be16(d + name + 2), strings = be16(d + name + 4);
    int best = -1, best_score = 0;
    for (unsigned i = 0; i < count && name + 6 + (i + 1) * 12 <= size; i++) {
        const uint8_t *r = d + name + 6 + i * 12;
        if (be16(r) != 3 || be16(r + 6) != 1) continue;              /* Windows platform, family */
        const int score = be16(r + 4) == 0x409 ? 2 : 1;
        if (score > best_score) { best = (int)i; best_score = score; }
    }
    if (best >= 0) {
        const uint8_t *r = d + name + 6 + (size_t)best * 12;
        const size_t len = be16(r + 8), off = name + strings + be16(r + 10);
        if (off + len <= size) {
            size_t o = 0;
            for (size_t k = 0; k + 1 < len && o + 1 < cap; k += 2) out[o++] = (wchar_t)be16(d + off + k);
            out[o] = 0;
            rc = o ? 0 : -1;
        }
    }
done:
    free(d);
    return rc;
}

/* ---- drawing ------------------------------------------------------------------ */

/* A font of `face` with an em of em_px pixels; NULL unless GDI has that face
 * (it would otherwise substitute another without saying so). */
static HFONT make_font(const wchar_t *face, int em_px) {
    HFONT f = CreateFontW(-em_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    if (!f) return NULL;
    wchar_t got[LF_FACESIZE] = L"";
    HGDIOBJ old = SelectObject(g_dc, f);
    GetTextFaceW(g_dc, LF_FACESIZE, got);
    SelectObject(g_dc, old);
    if (_wcsicmp(got, face) != 0) { DeleteObject(f); return NULL; }
    return f;
}

static const MAT2 IDENTITY = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };

/* The em that gives `face` capitals CAP_PX tall (measured on 'H' at 100 px). */
static int em_for_caps(const wchar_t *face) {
    HFONT f = make_font(face, 100);
    if (!f) return CJK_EM;
    HGDIOBJ old = SelectObject(g_dc, f);
    GLYPHMETRICS gm;
    int em = CJK_EM;
    WORD gi = 0;
    if (GetGlyphIndicesW(g_dc, L"H", 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) == 1 && gi != 0xFFFF &&
        GetGlyphOutlineW(g_dc, L'H', GGO_METRICS, &gm, 0, NULL, &IDENTITY) != GDI_ERROR && gm.gmBlackBoxY > 20)
        em = (int)((CAP_PX * 100 + gm.gmBlackBoxY / 2) / gm.gmBlackBoxY);
    SelectObject(g_dc, old);
    DeleteObject(f);
    return em < 12 ? 12 : em > 26 ? 26 : em;
}

static void fonts_free(void) {
    for (int i = 0; i < g_nfont; i++) DeleteObject(g_font[i]);
    g_nfont = 0;
}

/* Load `face` (and the fallbacks) for drawing. 0 if the face itself is there. */
static int fonts_load(const wchar_t *face) {
    fonts_free();
    HFONT f = make_font(face, em_for_caps(face));
    if (!f) return -1;
    g_font[g_nfont++] = f;
    for (int i = 0; i < NFALLBACK; i++)
        if (_wcsicmp(face, FALLBACK[i]) != 0 && (f = make_font(FALLBACK[i], CJK_EM)) != NULL) g_font[g_nfont++] = f;
    return 0;
}

/* psp_font_host_fn: the first font that has the character draws it. */
static int host_glyph(uint32_t code, psp_pgf_glyph *g, uint8_t *px, size_t cap) {
    if (code > 0xFFFF || !g_dc) return -1;
    const wchar_t wc = (wchar_t)code;
    for (int k = 0; k < g_nfont; k++) {
        SelectObject(g_dc, g_font[k]);
        WORD gi = 0;
        if (GetGlyphIndicesW(g_dc, &wc, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) != 1 || gi == 0xFFFF) continue;
        if (gi == 0 && code != 0x20) continue;                       /* .notdef */
        GLYPHMETRICS gm;
        const DWORD need = GetGlyphOutlineW(g_dc, wc, GGO_GRAY4_BITMAP, &gm, 0, NULL, &IDENTITY);
        if (need == GDI_ERROR) continue;
        const int w = need ? (int)gm.gmBlackBoxX : 0, h = need ? (int)gm.gmBlackBoxY : 0;
        g->w = w;
        g->h = h;
        g->left = w ? gm.gmptGlyphOrigin.x : 0;
        g->top  = h ? gm.gmptGlyphOrigin.y : 0;
        g->flags = 1;
        g->dimension[0] = w * 64;          g->dimension[1] = h * 64;
        g->x_adjust[0]  = g->left * 64;    g->x_adjust[1]  = -(w * 64) / 2;
        g->y_adjust[0]  = g->top * 64;     g->y_adjust[1]  = 0;
        g->advance[0]   = gm.gmCellIncX * 64;
        g->advance[1]   = LINE_PX * 64;
        if (!px || !need) return 0;
        if (cap < (size_t)w * (size_t)h) return -1;
        uint8_t *buf = (uint8_t *)malloc(need);
        if (!buf) return -1;
        if (GetGlyphOutlineW(g_dc, wc, GGO_GRAY4_BITMAP, &gm, need, buf, &IDENTITY) == GDI_ERROR) { free(buf); return -1; }
        const int pitch = (w + 3) & ~3;                              /* rows are DWORD-aligned, a byte per pixel */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t v = buf[y * pitch + x];               /* 0..16 */
                px[y * w + x] = (uint8_t)(v > 15 ? 15 : v);
            }
        free(buf);
        return 0;
    }
    return -1;
}

/* The user's message font (Settings > fonts, or Segoe UI). */
static void system_face(wchar_t *out, size_t cap) {
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0) && ncm.lfMessageFont.lfFaceName[0])
        _snwprintf(out, cap, L"%ls", ncm.lfMessageFont.lfFaceName);
    else
        _snwprintf(out, cap, L"Segoe UI");
    out[cap - 1] = 0;
}

int font_win_select(int i) {
    if (!g_dc) g_dc = CreateCompatibleDC(NULL);
    if (i < 0 || i >= g_n) i = 0;
    choice *c = &g_ch[i];
    if (c->kind == CH_PSP) {
        psp_font_set_host(NULL);
        fonts_free();
        fprintf(stderr, "font: the PSP fonts\n");
        return i;
    }
    wchar_t face[LF_FACESIZE];
    if (c->kind == CH_FILE) {
        if (!c->added) {
            c->added = AddFontResourceExW(c->path, FR_PRIVATE, NULL) > 0 ? 1 : -1;
            if (c->added > 0 && family_from_file(c->path, c->family, LF_FACESIZE) != 0) c->added = -1;
        }
        if (c->added > 0 && fonts_load(c->family) == 0) {
            psp_font_set_host(host_glyph);
            fprintf(stderr, "font: %s (family \"%ls\"), %d fallback(s)\n", c->key, c->family, g_nfont - 1);
            return i;
        }
        fprintf(stderr, "font: cannot use %s; using the system font\n", c->key);
        i = 0;
    }
    system_face(face, LF_FACESIZE);
    if (fonts_load(face) != 0) {                /* the default when the user's font is not there */
        fprintf(stderr, "font: system font \"%ls\" unavailable\n", face);
        const wchar_t *def = fonts_load(L"Segoe UI") == 0 ? L"Segoe UI" : fonts_load(FALLBACK[0]) == 0 ? FALLBACK[0] : L"none";
        _snwprintf(face, LF_FACESIZE, L"%ls", def);
    }
    psp_font_set_host(g_nfont ? host_glyph : NULL);
    fprintf(stderr, "font: system (\"%ls\"), %d fallback(s)\n", face, g_nfont ? g_nfont - 1 : 0);
    return i;
}

#else  /* not Windows: the PGF files only */

int font_win_init(const char *fonts_dir, const char *pgf_dir) { (void)fonts_dir; (void)pgf_dir; return 1; }
int font_win_count(void) { return 1; }
const char *font_win_name(int i) { (void)i; return "PSP"; }
const char *font_win_key(int i) { (void)i; return "psp"; }
int font_win_find(const char *key) { (void)key; return 0; }
int font_win_select(int i) { (void)i; return 0; }

#endif
