/*
 * PAINT.APP - a GEOBENCH paint/drawing app (#114).
 *
 * The editable canvas is a 100x100 packed bitmap buffer. Normal 100x100 .PIC files
 * use it as the whole document; larger banked .PIC files open in a view mode, then
 * Edit/Select loads a selected area into that buffer and Save writes it back through
 * GB_PICEDIT before saving the full picture.
 *
 * Phase 1: window + canvas + freehand pencil.
 * Phase 3: the floating toolchest. The 5 tools (pencil/square/circle/fill/undo)
 *   are loaded at runtime from PAINT.IST and drawn as a Paint-owned floating
 *   palette, so it stays above the canvas without adding resident WM topmost code.
 * Phase 2 (here): files. A top-bar File/Edit/View menu and the .PIC format - a
 *   versioned header ("GBPC", v2: mode, width/height in px, a 4-ink palette) then
 *   raw picture bytes. Save writes either the small in-page canvas or the whole
 *   banked picture after committing the current tile.
 *
 * Pixel addressing is always the portable CPC Mode-1 layout: pixel i has bit0
 * at 7-i and bit1 at 3-i. MSX2/PCW translate rows only while displaying them,
 * so canvas, undo, banked tiles, and saved .PIC files stay byte-identical.
 * gb_my is pixel-accurate (rows); gb_mxp gives the pixel x (#114, gb_mx only
 * resolves to the byte column). A kernel-MANAGED window (gb_wm_managed; the WM
 * owns the frame/title/close/drag) - p_draw/p_frame/p_click/p_drag/p_close, like
 * apps/iconed/main.c (#146).
 */
#include "gb.h"

#define DEF_X     0
#define DEF_Y     8
#define TITLE_H   14

#define CANVAS_WB 25                    /* canvas width in bytes (100 px / 4)    */
#define CANVAS_H  100                   /* canvas height in rows (px)            */
#define CANVAS_W  (CANVAS_WB * 4)       /* 100 px                                */
#define WIN_W     GB_COLS
#define WIN_H     (GB_LINES - 8)
#define SB_W      3                     /* vertical scrollbar width, byte cols   */
#define HSB_H     7                     /* horizontal scrollbar height, rows     */

#define WHITE_BYTE 0xF0                 /* 4 px of pen 1 (white) - the blank canvas */

/* ---- tools (PAINT.IST icon order: pencil,square,circle,fill,undo) ----------- */
#define TOOL_PENCIL 0
#define TOOL_SQUARE 1
#define TOOL_CIRCLE 2
#define TOOL_FILL   3
#define TOOL_UNDO   4
#define N_TOOLS     5
#define TOOL_WB     6                   /* tool icon width in bytes (24 px)      */
#define TOOL_H      24                  /* tool icon height in rows              */
#define TOOL_SY     27                  /* tool row stride (24 + 3 gap)          */

static unsigned char win_x = DEF_X, win_y = DEF_Y;
static unsigned char winw, winh;                   /* live managed-window size */
static unsigned char view_x, view_y, view_w, view_h, view_vsb, view_hsb;
static unsigned char hbar_x, hbar_y, hbar_w;
static unsigned char scroll_x;                     /* byte-column scroll       */
static unsigned char scroll_y;                     /* row scroll               */
static unsigned char tile_x, tile_y;               /* editable tile origin in picture coords */
static unsigned char edit_x, edit_y, edit_visible; /* editable canvas screen rect */
#define CVX  edit_x                                /* editable canvas screen x */
#define CVY  edit_y                                /* editable canvas screen y */

/* Paint-owned floating tool window. It is not a WM window: the main Paint window
   draws it last and handles its drag/tools, so the palette always stays on top. */
#define TC_TITLE_H 12
#define TC_WIN_W   (TOOL_WB * 2 + 2)
#define TC_WIN_H   (TC_TITLE_H + 1 + 3 * TOOL_SY + PAL_H + 2 + 8 + 2)
static unsigned char tc_x = 64, tc_y = 28;
#define TCX  (unsigned char)(tc_x + 1)              /* tools left, byte column  */
#define TCY  (unsigned char)(tc_y + TC_TITLE_H + 1) /* tools top                */
#define PAL_Y (unsigned char)(TCY + 3 * TOOL_SY)    /* ink swatches below tools */
#define PAL_H 10
#define SW_WB 3                                      /* swatch width in bytes    */
#define WID_Y (unsigned char)(PAL_Y + PAL_H + 2)     /* pencil-width control row */

/* .PIC v2 = a 14-byte header then the packed canvas, all in one buffer so Save/Load
   are a single fs call (#114):
     0  "GBPC" magic     4  version = 2     5  mode (1=canonical Mode-1)
     6  width_px (2, LE) 8  height_px (2, LE)
     10 inks[4]          pen 0..3 -> CPC hardware ink (the picture's palette)
  14 canvas bytes (width_px/4 stride * height rows) */
#define PIC_HDR  14
#define PIC_LEN  (PIC_HDR + CANVAS_WB * CANVAS_H)    /* 2514 */
#define PIC_MAX  PIC_LEN
static unsigned char picbuf[PIC_MAX];
#define canvas (picbuf + PIC_HDR)                     /* the canvas body */
/* the GEOBENCH 4-pen palette (kernel set_palette): pen0..3 CPC hardware inks */
static const unsigned char pic_inks[4] = { 1, 26, 0, 6 };  /* blue, white, black, red */

static unsigned char pic_wb = CANVAS_WB;
static unsigned int  pic_h = CANVAS_H;
static unsigned int  pic_off = PIC_HDR;
static unsigned int  pic_file_len = PIC_LEN;
static unsigned char editable = 1;     /* 1 = picture can be edited */
static unsigned char banked, banked2;  /* borrowed viewer-style picture banks */
static unsigned char tile_dirty;       /* banked mode: canvas tile differs from picture bank */
static unsigned char bank_dirty;       /* banked picture memory differs from disk */
static unsigned char pic_toobig;
#define PIC_PAGE_K  (*(volatile unsigned char *)0x130B)
#define PIC_PAGE2_K (*(volatile unsigned char *)0x1348)
#define PIC_SIZE_K  (*(volatile unsigned int  *)0x1349)
#define PIC_WB_K    (*(volatile unsigned char *)0x130C)
#define PIC_H_K     (*(volatile unsigned int  *)0x130D)
#define PIC_OFF_K   (*(volatile unsigned char *)0x130F)
#define FS_XFLAGS_K (*(volatile unsigned char *)0x144F)
#define FS_SAVE_LEN_K (*(volatile unsigned int *)0x14FD)
#define UI_MODAL_K  (*(volatile unsigned char *)0x1705)

static unsigned char pen = 2;          /* current ink: 0 blue 1 white 2 black 3 red */
static unsigned char tool = TOOL_PENCIL;
static unsigned char pen_w = 1;        /* pencil width 1..4                     */
#define MODE_VIEW   0
#define MODE_SELECT 1
#define MODE_EDIT   2
static unsigned char paint_mode;
static unsigned char edit_wb = CANVAS_WB, edit_pw = CANVAS_W, edit_ph = CANVAS_H;
static unsigned char sel_on, sel_done, sel_xb0, sel_y0, sel_xb1, sel_y1;

/* window-title scratch: current file name + " *" when unsaved. */
static char cur_name[11];
static unsigned char dirty;
static char fbase[14];                 /* "NAME.PIC" */
static char wtitle[18];                /* fbase + " *" when modified            */
static void fmt83(char *dst, const char *n11);   /* defined below; used by win_title */
static void draw(void);
static void draw_picture(void);
static void damage_toolchest_move(unsigned char ox, unsigned char oy);
static unsigned char get_pen(unsigned char x, unsigned char y);
static void p_close(void);

/* PAINT.IST loaded whole at startup; tools blit straight out of it. 2 sectors. PAINT's
 * 16K bank (canvas + this) caps the set at ~6 icons, so the built PAINT.IST carries just
 * the N_TOOLS PAINT uses; the full 14-tool source set lives in assets/paint/ (#246). */
#define IST_MAX 756
static unsigned char ist[IST_MAX];
static unsigned char ist_ok = 0;       /* a valid GBIS set with >=N_TOOLS icons? */

#if defined(GB_MSX2) || defined(GB_PCW)
#define picrow ((unsigned char *)gb_copybuf + CANVAS_WB * CANVAS_H)

#ifdef GB_PCW
static unsigned char native_pic_byte(unsigned char b)
{
    unsigned char i, p, out = 0;
    for (i = 0; i < 4; i++) {
        p = (unsigned char)(((b >> (7 - i)) & 1) | (((b >> (3 - i)) & 1) << 1));
        out |= (unsigned char)(p << (6 - 2 * i));
    }
    out = (unsigned char)(((out & 0x55) << 1) | (((out ^ 0xFF) & 0xAA) >> 1));
    return out;
}
#endif

static void restore_packed(unsigned char x, unsigned char y, unsigned char w,
                           unsigned char rows, const unsigned char *src,
                           unsigned char stride)
{
    unsigned char r;
#ifdef GB_PCW
    unsigned char c;
#endif
    for (r = 0; r < rows; r++) {
#ifdef GB_MSX2
        gb_pic_edit_buf = (unsigned int)src;
        gb_pic_edit_off = (unsigned int)picrow;
        FS_SAVE_LEN_K = w;
        if (!gb_pic_edit(GB_PICEDIT_NATIVE)) return;
#else
        for (c = 0; c < w; c++) picrow[c] = native_pic_byte(src[c]);
#endif
        gb_restorerect(x, (unsigned char)(y + r), w, 1, picrow);
        src += stride;
    }
}
#endif

/* ---- packed pixel helpers --------------------------------------------------- */
/* Replace pixel i's pen: clear its two bits first, else drawing over a non-zero
   pen blends (e.g. pen 2 over white pen 1 -> pen 3 red). */
static unsigned char set_pixel(unsigned char b, unsigned char i, unsigned char p)
{
    b &= (unsigned char)~((1 << (7 - i)) | (1 << (3 - i)));
    if (p & 1) b |= (unsigned char)(1 << (7 - i));
    if (p & 2) b |= (unsigned char)(1 << (3 - i));
    return b;
}

static void canvas_clear(void)         /* New: blank to white (pen 1) */
{
    unsigned int n;
    for (n = 0; n < (unsigned int)CANVAS_WB * CANVAS_H; n++) canvas[n] = WHITE_BYTE;
}

/* ---- PAINT.IST directory (16-byte header, then 4-byte entries: off,wb,h) ----- */
static unsigned int tool_off(unsigned char k)
{
    unsigned int p = 16 + (unsigned int)k * 4;
    return (unsigned int)ist[p] | ((unsigned int)ist[p + 1] << 8);
}
static unsigned char tool_wb(unsigned char k) { return ist[16 + (unsigned int)k * 4 + 2]; }
static unsigned char tool_h(unsigned char k)  { return ist[16 + (unsigned int)k * 4 + 3]; }

static void load_tools(void)
{
    unsigned int n;
    gb_set_name("PAINT   IST");
    n = gb_fs_load((char *)ist, IST_MAX);
    ist_ok = (n >= 16 && ist[0] == 'G' && ist[1] == 'B' && ist[2] == 'I' &&
              ist[3] == 'S' && ist[4] == 2 && ist[5] >= N_TOOLS);
}

#if defined(GB_MSX2) || defined(GB_PCW)
static void load_picedit_helper(void) {}
#else
static void load_picedit_helper(void)
{
    unsigned int n, i;
    unsigned char *src;
    unsigned char *dst = (unsigned char *)0x1600;

    gb_set_name("DEFAULT SPR");
    n = gb_fs_load(gb_copybuf, 512);
    gb_set_name(cur_name);
    if (n <= 256) return;
    n -= 256;
    if (n > 256) n = 256;
    src = (unsigned char *)gb_copybuf + 256;
    for (i = 0; i < n; i++) dst[i] = src[i];
}
#endif

static void mark_dirty(void) { dirty = 1; if (banked) tile_dirty = 1; }

static void edit_full_canvas(void)
{
    edit_wb = CANVAS_WB;
    edit_pw = CANVAS_W;
    edit_ph = CANVAS_H;
}

/* tool k's top-left in byte col / row (2 per row) */
static unsigned char tool_x(unsigned char k) { return (unsigned char)(TCX + (k & 1) * TOOL_WB); }
static unsigned char tool_y(unsigned char k) { return (unsigned char)(TCY + (k >> 1) * TOOL_SY); }

/* ---- rendering -------------------------------------------------------------- */
/* render one canvas row to the screen */
static void blit_row(unsigned char row)
{
    gb_curhide();
#if defined(GB_MSX2) || defined(GB_PCW)
    restore_packed(CVX, (unsigned char)(CVY + row), edit_wb, 1,
                   canvas + (unsigned int)row * CANVAS_WB, CANVAS_WB);
#else
    gb_restorerect(CVX, (unsigned char)(CVY + row), edit_wb, 1,
                   canvas + (unsigned int)row * CANVAS_WB);
#endif
    gb_curshow();
}

/* render the whole canvas */
static void blit_canvas(void)
{
#if !defined(GB_MSX2) && !defined(GB_PCW)
    unsigned char y;
#endif
    gb_curhide();
#if defined(GB_MSX2) || defined(GB_PCW)
    restore_packed(CVX, CVY, edit_wb, edit_ph, canvas, CANVAS_WB);
#else
    if (edit_wb == CANVAS_WB) gb_restorerect(CVX, CVY, edit_wb, edit_ph, canvas);
    else for (y = 0; y < edit_ph; y++)
        gb_restorerect(CVX, (unsigned char)(CVY + y), edit_wb, 1,
                       canvas + (unsigned int)y * CANVAS_WB);
#endif
    gb_curshow();
}

static void draw_toolchest(void)
{
    unsigned char k;
    char wbuf[2];
    gb_curhide();
    gb_fill(tc_x, tc_y, TC_WIN_W, TC_WIN_H, 1);
    gb_fill(tc_x, tc_y, TC_WIN_W, TC_TITLE_H, 3);
    gb_frame(tc_x, tc_y, TC_WIN_W, TC_WIN_H, 2);
    gb_text((unsigned char)(tc_x + 1), (unsigned char)(tc_y + 2), "Tools");
    if (ist_ok)
        for (k = 0; k < N_TOOLS; k++) {
#if defined(GB_MSX2) || defined(GB_PCW)
            restore_packed(tool_x(k), tool_y(k), tool_wb(k), tool_h(k),
                           ist + tool_off(k), tool_wb(k));
#else
            gb_restorerect(tool_x(k), tool_y(k), tool_wb(k), tool_h(k),
                           ist + tool_off(k));
#endif
        }
    gb_frame(tool_x(tool), tool_y(tool), TOOL_WB, TOOL_H, 3);   /* selected: red */

    for (k = 0; k < 4; k++)
        gb_fill((unsigned char)(TCX + k * SW_WB), PAL_Y, SW_WB, PAL_H, k);
    gb_frame((unsigned char)(TCX + pen * SW_WB), PAL_Y, SW_WB, PAL_H, 2); /* sel: black */

    gb_fill(TCX, WID_Y, TOOL_WB * 2, 8, 1);          /* white strip for the width row */
    gb_textbw(TCX, WID_Y, "-");
    wbuf[0] = (char)('0' + pen_w); wbuf[1] = 0;
    gb_textbw((unsigned char)(TCX + 5), WID_Y, wbuf);
    gb_textbw((unsigned char)(TCX + TOOL_WB + 3), WID_Y, "+");
    gb_curshow();
}

/* plot the brush (pen_w x pen_w) at canvas pixel (cx,cy) with the current pen */
static void plot(unsigned char cx, unsigned char cy)
{
    unsigned char dx, dy, x, y;
    unsigned int off;
    mark_dirty();
    for (dy = 0; dy < pen_w; dy++) {
        y = (unsigned char)(cy + dy);
        if (y >= edit_ph) break;
        for (dx = 0; dx < pen_w; dx++) {
            x = (unsigned char)(cx + dx);
            if (x >= edit_pw) break;
            off = (unsigned int)y * CANVAS_WB + (x >> 2);
            canvas[off] = set_pixel(canvas[off], (unsigned char)(x & 3), pen);
        }
        blit_row(y);
    }
}

/* ---- Phase 4: undo, flood fill, shapes (#114) ------------------------------- */
#define CANVAS_SZ (CANVAS_WB * CANVAS_H)
#define undo ((unsigned char *)gb_copybuf) /* one-level snapshot in low-RAM scratch */
static unsigned char undo_valid;
static unsigned char fire_prev;         /* edge-detect the fire press                */

static void save_undo(void)             /* snapshot the canvas before an operation */
{
    unsigned int n;
    for (n = 0; n < CANVAS_SZ; n++) undo[n] = canvas[n];
    undo_valid = 1;
}
static void do_undo(void)               /* swap canvas <-> snapshot (so Undo redoes) */
{
    unsigned int n; unsigned char t;
    if (paint_mode != MODE_EDIT || !undo_valid) return;
    for (n = 0; n < CANVAS_SZ; n++) { t = canvas[n]; canvas[n] = undo[n]; undo[n] = t; }
    mark_dirty();
    if (edit_visible) blit_canvas();
}

/* read / write a canvas pixel's pen */
static unsigned char get_pen(unsigned char x, unsigned char y)
{
    unsigned char b = canvas[(unsigned int)y * CANVAS_WB + (x >> 2)], i = (unsigned char)(x & 3);
    return (unsigned char)(((b >> (7 - i)) & 1) | (((b >> (3 - i)) & 1) << 1));
}
static void cpix(unsigned char x, unsigned char y)            /* in-bounds set (fill) */
{
    unsigned int off = (unsigned int)y * CANVAS_WB + (x >> 2);
    canvas[off] = set_pixel(canvas[off], (unsigned char)(x & 3), pen);
}
static void cpset(int x, int y)                               /* bounds-checked (shapes) */
{
    if (x >= 0 && x < edit_pw && y >= 0 && y < edit_ph) {
        unsigned int off = (unsigned int)y * CANVAS_WB + ((unsigned int)x >> 2);
        canvas[off] = set_pixel(canvas[off], (unsigned char)(x & 3), pen);
    }
}

/* 4-connected scanline flood fill from (x,y). A bounded seed stack (drops seeds when
   full -> at worst a partial fill, never a crash). */
#define FILL_STK 256
#define fstk ((unsigned int *)gb_copybuf)
static unsigned int fsp;
static void fpush(unsigned char x, unsigned char y)          /* pack as (y<<8)|x */
{
    if (fsp < FILL_STK) fstk[fsp++] = (unsigned int)((unsigned int)y << 8) | x;
}
static void flood_fill(unsigned char x, unsigned char y)
{
    unsigned char target = get_pen(x, y), xl, xr, xx, inside, fx, fy;
    unsigned int v;
    if (target == pen) return;
    fsp = 0; fpush(x, y);
    while (fsp) {
        v = fstk[--fsp]; fx = (unsigned char)(v & 0xFF); fy = (unsigned char)(v >> 8);
        if (get_pen(fx, fy) != target) continue;          /* already filled (dup seed) */
        xl = fx; while (xl > 0 && get_pen((unsigned char)(xl - 1), fy) == target) xl--;
        xr = fx; while (xr < edit_pw - 1 && get_pen((unsigned char)(xr + 1), fy) == target) xr++;
        for (xx = xl; xx <= xr; xx++) cpix(xx, fy);
        if (fy > 0) { inside = 0; for (xx = xl; xx <= xr; xx++)
            if (get_pen(xx, (unsigned char)(fy - 1)) == target) { if (!inside) { fpush(xx, (unsigned char)(fy - 1)); inside = 1; } } else inside = 0; }
        if (fy < edit_ph - 1) { inside = 0; for (xx = xl; xx <= xr; xx++)
            if (get_pen(xx, (unsigned char)(fy + 1)) == target) { if (!inside) { fpush(xx, (unsigned char)(fy + 1)); inside = 1; } } else inside = 0; }
    }
    mark_dirty();
}

/* ---- shapes (outline, into the canvas buffer) ------------------------------- */
static int isqrt(unsigned int v) { unsigned int r = 0; while ((unsigned int)(r + 1) * (r + 1) <= v) r++; return (int)r; }

static void draw_rect(int x0, int y0, int x1, int y1)         /* rectangle outline */
{
    int x, y, t;
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
    for (x = x0; x <= x1; x++) { cpset(x, y0); cpset(x, y1); }
    for (y = y0; y <= y1; y++) { cpset(x0, y); cpset(x1, y); }
}
static void draw_circle(int cx, int cy, int r)               /* midpoint circle outline */
{
    int x = r, y = 0, err = 1 - r;
    if (r <= 0) { cpset(cx, cy); return; }
    while (x >= y) {
        cpset(cx + x, cy + y); cpset(cx - x, cy + y); cpset(cx + x, cy - y); cpset(cx - x, cy - y);
        cpset(cx + y, cy + x); cpset(cx - y, cy + x); cpset(cx + y, cy - x); cpset(cx - y, cy - x);
        y++; if (err < 0) err += 2 * y + 1; else { x--; err += 2 * (y - x) + 1; }
    }
}

/* rubber_band: drag a square/circle from (sx,sy); the outline previews against the
   pre-shape snapshot until release commits it. */
static void rubber_band(unsigned char sx, unsigned char sy)
{
    unsigned char cx = sx, cy = sy, ncx, ncy, my, first = 1, fl;
    unsigned int px, n, cxl = (unsigned int)CVX * 4;
    save_undo();                                          /* base = pre-shape + undo */
    for (;;) {
        fl = gb_poll();
        if (!(fl & GB_FIRE)) break;                       /* release -> commit */
        my = gb_my();
        ncy = (my < CVY) ? 0 : (unsigned char)(my - CVY); if (ncy >= edit_ph) ncy = edit_ph - 1;
        px = gb_mxp();
        if (px < cxl) ncx = 0; else { px -= cxl; ncx = (px >= edit_pw) ? edit_pw - 1 : (unsigned char)px; }
        if (ncx >= edit_pw) ncx = edit_pw - 1;
        if (first || ncx != cx || ncy != cy) {
            first = 0; cx = ncx; cy = ncy;
            for (n = 0; n < CANVAS_SZ; n++) canvas[n] = undo[n];   /* restore base */
            if (tool == TOOL_SQUARE) draw_rect(sx, sy, cx, cy);
            else { int dx = (int)cx - sx, dy = (int)cy - sy;
                   draw_circle(sx, sy, isqrt((unsigned int)(dx * dx + dy * dy))); }
            blit_canvas();
        }
    }
    mark_dirty();
}

/* win_title: the window title = the file name, " *" appended when unsaved. */
static const char *win_title(void)
{
    unsigned char j = 0;
    char c;
    fmt83(fbase, cur_name);
    while ((c = fbase[j]) != 0) { wtitle[j] = c; j++; }
    if (dirty) { wtitle[j++] = ' '; wtitle[j++] = '*'; }
    wtitle[j] = 0;
    return wtitle;
}

/* sync_rect: pull the live WM-owned geometry before we use it (#146). */
static void sync_rect(void)
{
    win_x = gb_wm_x(); win_y = gb_wm_y(); winw = gb_wm_w(); winh = gb_wm_h();
}

static void close_banked(void)
{
    if (!banked) return;
    PIC_PAGE_K = banked;
    PIC_PAGE2_K = banked2;
    gb_pic_close();
    banked = banked2 = 0;
    tile_dirty = 0;
}

static void setup_tile_request(void)
{
    PIC_PAGE_K = banked;
    PIC_PAGE2_K = banked2;
    gb_pic_edit_buf = (unsigned int)canvas;
    gb_pic_edit_off = pic_off + (unsigned int)tile_y * pic_wb + tile_x;
}

static unsigned char load_tile(void)
{
    if (!banked || !editable) return 1;
    undo_valid = 0;                         /* GB_PICEDIT uses gb_copybuf as scratch */
    setup_tile_request();
    if (gb_pic_edit(GB_PICEDIT_GET)) {
        tile_dirty = 0;
        return 1;
    }
    load_picedit_helper();
    setup_tile_request();
    if (gb_pic_edit(GB_PICEDIT_GET)) {
        tile_dirty = 0;
        return 1;
    }
    return 0;
}

static unsigned char commit_tile(void)
{
    if (!banked || !editable || !tile_dirty) return 1;
    undo_valid = 0;                         /* GB_PICEDIT uses gb_copybuf as scratch */
    setup_tile_request();
    if (gb_pic_edit(GB_PICEDIT_PUT)) {
        tile_dirty = 0;
        bank_dirty = 1;
        return 1;
    }
    load_picedit_helper();
    setup_tile_request();
    if (gb_pic_edit(GB_PICEDIT_PUT)) {
        tile_dirty = 0;
        bank_dirty = 1;
        return 1;
    }
    return 0;
}

static void clamp_toolchest(void)
{
    unsigned char min_y = (unsigned char)(win_y + TITLE_H + 1);
    unsigned char max_x = (winw > (unsigned char)(TC_WIN_W + 1)) ?
        (unsigned char)(win_x + winw - TC_WIN_W - 1) : win_x;
    unsigned char max_y = (winh > (unsigned char)(TITLE_H + TC_WIN_H + 1)) ?
        (unsigned char)(win_y + winh - TC_WIN_H - 1) : min_y;
    if (tc_x < win_x) tc_x = win_x;
    if (tc_x > max_x) tc_x = max_x;
    if (tc_y < min_y) tc_y = min_y;
    if (tc_y > max_y) tc_y = max_y;
}

static void calc_view(void)
{
    unsigned char cx = (unsigned char)(win_x + 1);
    unsigned char cy = (unsigned char)(win_y + TITLE_H + 1);
    unsigned char cw = (winw > 2) ? (unsigned char)(winw - 2) : 0;
    unsigned char ch = (winh > (unsigned char)(TITLE_H + 2)) ? (unsigned char)(winh - TITLE_H - 2) : 0;
    unsigned char i, off = 0, vw = 0, vh = 0;

    view_vsb = view_hsb = 0;
    for (i = 0; i < 2; i++) {
        vh = (ch > (view_hsb ? HSB_H : 0)) ? (unsigned char)(ch - (view_hsb ? HSB_H : 0)) : 0;
        view_vsb = (unsigned char)(pic_h > vh);
        off = (view_vsb ? SB_W : 0);
        vw = (cw > off) ? (unsigned char)(cw - off) : 0;
        view_hsb = (unsigned char)(pic_wb > vw);
    }
    if (pic_h <= vh) scroll_y = 0;
    else if (scroll_y > (unsigned char)(pic_h - vh)) scroll_y = (unsigned char)(pic_h - vh);
    if (pic_wb <= vw) scroll_x = 0;
    else if (scroll_x > (unsigned char)(pic_wb - vw)) scroll_x = (unsigned char)(pic_wb - vw);

    view_w = (pic_wb < vw) ? pic_wb : vw;
    view_h = (pic_h < vh) ? (unsigned char)pic_h : vh;
    view_x = (unsigned char)(cx + off + ((pic_wb < vw) ? (unsigned char)((vw - pic_wb) / 2) : 0));
    view_y = (unsigned char)(cy + ((pic_h < vh) ? (unsigned char)((vh - (unsigned char)pic_h) / 2) : 0));
    if (view_hsb && vw) {
        hbar_x = (unsigned char)(cx + off);
        hbar_y = (unsigned char)(cy + vh);
        hbar_w = vw;
    } else hbar_w = 0;
}

static void blit_pic(unsigned char x, unsigned char y, unsigned char w,
                     unsigned char rows, unsigned int src)
{
    unsigned char r = 0;
    if (!w || !rows) return;
    if (w == pic_wb && !banked2) {
        if (banked) { PIC_PAGE_K = banked; PIC_PAGE2_K = 0; gb_pic_blit(x, y, w, rows, src); }
        else if (src + (unsigned int)w * rows <= pic_file_len) {
#if defined(GB_MSX2) || defined(GB_PCW)
            restore_packed(x, y, w, rows, picbuf + src, pic_wb);
#else
            gb_restorerect(x, y, w, rows, picbuf + src);
#endif
        }
        return;
    }
    while (r < rows) {
        if (banked) {
            PIC_PAGE_K = banked;
            PIC_PAGE2_K = banked2;
            gb_pic_blit(x, (unsigned char)(y + r), w, 1, src);
        } else if (src + w <= pic_file_len) {
#if defined(GB_MSX2) || defined(GB_PCW)
            restore_packed(x, (unsigned char)(y + r), w, 1, picbuf + src, pic_wb);
#else
            gb_restorerect(x, (unsigned char)(y + r), w, 1, picbuf + src);
#endif
        }
        src += pic_wb;
        r++;
    }
}

static void clamp_tile(void)
{
    if (pic_wb <= CANVAS_WB) tile_x = 0;
    else if (tile_x > (unsigned char)(pic_wb - CANVAS_WB)) tile_x = (unsigned char)(pic_wb - CANVAS_WB);
    if (pic_h <= CANVAS_H) tile_y = 0;
    else if (tile_y > (unsigned char)(pic_h - CANVAS_H)) tile_y = (unsigned char)(pic_h - CANVAS_H);
}

static unsigned char norm_sel(void)
{
    unsigned char x0 = sel_xb0, x1 = sel_xb1, y0 = sel_y0, y1 = sel_y1, t;
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
    if ((unsigned char)(x1 - x0) >= CANVAS_WB) x1 = (unsigned char)(x0 + CANVAS_WB - 1);
    if ((unsigned char)(y1 - y0) >= CANVAS_H) y1 = (unsigned char)(y0 + CANVAS_H - 1);
    tile_x = x0; tile_y = y0;
    edit_wb = (unsigned char)(x1 - x0 + 1);
    edit_pw = (unsigned char)(edit_wb * 4);
    edit_ph = (unsigned char)(y1 - y0 + 1);
    return 1;
}

static void sync_edit_rect(void)
{
    unsigned char cx = (unsigned char)(win_x + 1);
    unsigned char cy = (unsigned char)(win_y + TITLE_H + 1);
    unsigned char cw = (winw > 2) ? (unsigned char)(winw - 2) : 0;
    unsigned char ch = (winh > (unsigned char)(TITLE_H + 2)) ? (unsigned char)(winh - TITLE_H - 2) : 0;
    edit_x = (unsigned char)(cx + ((cw > edit_wb) ? (unsigned char)((cw - edit_wb) / 2) : 0));
    edit_y = (unsigned char)(cy + ((ch > edit_ph) ? (unsigned char)((ch - edit_ph) / 2) : 0));
    edit_visible = 1;
}

static void draw_edit_frame(void)
{
    if (edit_visible) gb_frame(edit_x, edit_y, edit_wb, edit_ph, 3);
}

static unsigned char image_hit(unsigned char *xb, unsigned char *yy)
{
    unsigned int px = gb_mxp(), left = (unsigned int)view_x * 4;
    unsigned int x;
    unsigned char y = gb_my();
    if (px < left || y < view_y) return 0;
    x = (px - left) >> 2;
    y = (unsigned char)(y - view_y);
    if (x >= view_w || y >= view_h) return 0;
    x += scroll_x; y = (unsigned char)(y + scroll_y);
    if (x >= pic_wb || y >= pic_h) return 0;
    *xb = (unsigned char)x; *yy = y;
    return 1;
}

static void draw_select_frame(void)
{
    unsigned char sx, sy;
    if (paint_mode != MODE_SELECT || !sel_on) return;
    norm_sel();
    if (tile_x < scroll_x || tile_y < scroll_y) return;
    if ((unsigned char)(tile_x + edit_wb) > (unsigned char)(scroll_x + view_w)) return;
    if ((unsigned int)tile_y + edit_ph > (unsigned int)scroll_y + view_h) return;
    sx = (unsigned char)(view_x + tile_x - scroll_x);
    sy = (unsigned char)(view_y + tile_y - scroll_y);
    gb_frame(sx, sy, edit_wb, edit_ph, 3);
}

static void cancel_select(void)
{
    sel_on = 0;
    sel_done = 0;
    if (banked) paint_mode = MODE_VIEW;
    tile_dirty = 0;
    dirty = bank_dirty;
}

static void begin_select(void)
{
    if (!banked || !editable || pic_toobig) return;
    if (paint_mode == MODE_EDIT && tile_dirty && !commit_tile()) return;
    paint_mode = MODE_SELECT;
    sel_on = 0;
    sel_done = 0;
}

static void enter_edit(void)
{
    if (!sel_on) return;
    sel_done = 1;
    norm_sel();
    clamp_tile();
    if (!load_tile()) { editable = 0; paint_mode = MODE_VIEW; return; }
    paint_mode = MODE_EDIT;
    undo_valid = 0;
}

static void select_click(void)
{
    unsigned char xb, yy;
    if (!image_hit(&xb, &yy)) return;
    if (sel_done) return;
    if (!sel_on) {
        sel_xb0 = sel_xb1 = xb;
        sel_y0 = sel_y1 = yy;
        sel_on = 1;
        return;
    }
    sel_xb1 = xb;
    sel_y1 = yy;
    sel_done = 1;
}

static unsigned char track_select(void)
{
    unsigned char xb, yy;
    if (paint_mode != MODE_SELECT || !sel_on || sel_done) return 0;
    if (!image_hit(&xb, &yy)) return 0;
    if (xb == sel_xb1 && ((yy ^ sel_y1) & 0xFC) == 0) return 0;
    sel_xb1 = xb;
    sel_y1 = yy;
    gb_curhide();
    draw_picture();
    gb_curshow();
    return 1;
}

static void draw_toobig(void)
{
    gb_text((unsigned char)(win_x + 3), (unsigned char)(win_y + TITLE_H + 8), "Pic not loaded");
    gb_text((unsigned char)(win_x + 3), (unsigned char)(win_y + TITLE_H + 19), "Need banks/<32K");
}

static void draw_scrollbars(void)
{
    if (view_vsb && pic_h > view_h) {
        unsigned char th = (unsigned char)((unsigned int)view_h * view_h / pic_h);
        unsigned char ty;
        if (th < 4) th = 4;
        if (th > view_h) th = view_h;
        ty = (unsigned char)(view_y + (unsigned int)scroll_y * (view_h - th) / (pic_h - view_h));
        gb_fill((unsigned char)(view_x - SB_W), view_y, SB_W, view_h, 1);
        gb_fill((unsigned char)(view_x - SB_W + 1), ty, 1, th, 3);
    }
    if (view_hsb && hbar_w && pic_wb > view_w) {
        unsigned char th = (unsigned char)((unsigned int)view_w * hbar_w / pic_wb);
        unsigned char tx;
        if (th < 4) th = 4;
        if (th > hbar_w) th = hbar_w;
        tx = (unsigned char)(hbar_x + (unsigned int)scroll_x * (hbar_w - th) / (pic_wb - view_w));
        gb_fill(hbar_x, hbar_y, hbar_w, (unsigned char)(HSB_H - 1), 1);
        gb_fill(tx, (unsigned char)(hbar_y + 2), th, 2, 3);
    }
}

static void draw_picture(void)
{
    unsigned char rows, draw_w;
    unsigned int src, r;
    if (paint_mode == MODE_EDIT) {
        sync_edit_rect();
        blit_canvas();
        draw_edit_frame();
        return;
    }
    if (pic_toobig) { draw_toobig(); return; }
    if (!view_w || !view_h) return;
    draw_w = (pic_wb - scroll_x > view_w) ? view_w : (unsigned char)(pic_wb - scroll_x);
    r = pic_h - scroll_y;
    rows = (r > view_h) ? view_h : (unsigned char)r;
    src = pic_off + (unsigned int)scroll_y * pic_wb + scroll_x;
    blit_pic(view_x, view_y, draw_w, rows, src);
    draw_scrollbars();
    draw_select_frame();
}

/* content redraw (picture + floating toolchest); the WM drew the frame/title (#146) */
static void draw(void)
{
    unsigned char body_y = (unsigned char)(win_y + TITLE_H);
    unsigned char body_h = (winh > TITLE_H) ? (unsigned char)(winh - TITLE_H - 1) : 0;
    calc_view();
    clamp_toolchest();
    gb_fill((unsigned char)(win_x + 1), body_y, (unsigned char)(winw - 2), body_h, 0);
    draw_picture();
    if (paint_mode != MODE_SELECT || sel_done) draw_toolchest();
}

static void p_draw(void) { sync_rect(); draw(); }   /* on_draw */

/* a discrete click in the toolchest -> select tool / ink / width. Returns 1 if
   it hit something (so the canvas-draw path is skipped). */
static unsigned char hit_toolchest(unsigned char mx, unsigned char my)
{
    unsigned char rx, ry, k;
    if (paint_mode == MODE_SELECT && !sel_done) return 0;
    if (mx < tc_x || mx >= (unsigned char)(tc_x + TC_WIN_W) ||
        my < tc_y || my >= (unsigned char)(tc_y + TC_WIN_H)) return 0;
    if (my < (unsigned char)(tc_y + TC_TITLE_H)) {
        unsigned char ox = tc_x, oy = tc_y;
        if (gb_drag_window(&tc_x, &tc_y, TC_WIN_W, TC_WIN_H)) {
            clamp_toolchest();
            damage_toolchest_move(ox, oy);
            gb_restore_parent();
        }
        return 1;
    }
    if (mx < TCX || mx >= (unsigned char)(TCX + TOOL_WB * 2)) return 1;
    rx = (unsigned char)(mx - TCX);

    if (my >= TCY && my < (unsigned char)(TCY + 3 * TOOL_SY)) {     /* tools */
        ry = (unsigned char)(my - TCY);
        if (ry % TOOL_SY < TOOL_H) {
            k = (unsigned char)((ry / TOOL_SY) * 2 + (rx >= TOOL_WB ? 1 : 0));
            if (k == TOOL_UNDO) do_undo();          /* undo is an action, not a mode */
            else if (k < N_TOOLS) { tool = k; draw_toolchest(); }
        }
        return 1;
    }
    if (my >= PAL_Y && my < (unsigned char)(PAL_Y + PAL_H)) {       /* palette */
        k = (unsigned char)(rx / SW_WB);
        if (k < 4) { pen = k; draw_toolchest(); }
        return 1;
    }
    if (my >= WID_Y && my < (unsigned char)(WID_Y + 8)) {           /* width -/+ */
        if (rx < TOOL_WB) { if (pen_w > 1) pen_w--; }
        else              { if (pen_w < 4) pen_w++; }
        draw_toolchest();
        return 1;
    }
    return 0;
}

static unsigned char over_toolchest(unsigned char mx, unsigned char my)
{
    return (unsigned char)(mx >= tc_x && mx < (unsigned char)(tc_x + TC_WIN_W) &&
        my >= tc_y && my < (unsigned char)(tc_y + TC_WIN_H));
}

static void damage_toolchest_move(unsigned char ox, unsigned char oy)
{
    unsigned char lx = (ox < tc_x) ? ox : tc_x;
    unsigned char ty = (oy < tc_y) ? oy : tc_y;
    unsigned char rx = ((unsigned char)(ox + TC_WIN_W) > (unsigned char)(tc_x + TC_WIN_W)) ?
        (unsigned char)(ox + TC_WIN_W) : (unsigned char)(tc_x + TC_WIN_W);
    unsigned char by = ((unsigned char)(oy + TC_WIN_H) > (unsigned char)(tc_y + TC_WIN_H)) ?
        (unsigned char)(oy + TC_WIN_H) : (unsigned char)(tc_y + TC_WIN_H);
    gb_wm_damage(lx, ty, (unsigned char)(rx - lx), (unsigned char)(by - ty));
}

static void sb_drag(void)
{
    unsigned char th = (unsigned char)((unsigned int)view_h * view_h / pic_h);
    unsigned char half;
    unsigned char max = (unsigned char)(pic_h - view_h), ny;
    unsigned int my, v;
    if (th < 4) th = 4;
    if (th > view_h) th = view_h;
    if (view_h <= th) return;
    half = (unsigned char)(th / 2);
    while (gb_poll() & GB_FIRE) {
        my = gb_my();
        if (my < (unsigned int)(view_y + half)) ny = 0;
        else { v = ((my - view_y - half) * max) / (view_h - th); ny = (v > max) ? max : v; }
        if (ny != scroll_y) {
            scroll_y = ny;
            gb_curhide();
            draw();
            gb_curshow();
        }
    }
}

static void hsb_drag(void)
{
    unsigned char th, half, nx;
    unsigned int max, mx, v;
    if (!view_hsb || !hbar_w || pic_wb <= view_w) return;
    th = (unsigned char)((unsigned int)view_w * hbar_w / pic_wb);
    if (th < 4) th = 4;
    if (th > hbar_w) th = hbar_w;
    if (hbar_w <= th) return;
    half = (unsigned char)(th / 2);
    max = pic_wb - view_w;
    while (gb_poll() & GB_FIRE) {
        mx = gb_mx();
        if (mx < (unsigned int)(hbar_x + half)) nx = 0;
        else { v = ((mx - hbar_x - half) * max) / (hbar_w - th); nx = (v > max) ? (unsigned char)max : (unsigned char)v; }
        if (nx != scroll_x) {
            scroll_x = nx;
            gb_curhide();
            draw();
            gb_curshow();
        }
    }
}

/* ---- file name helpers ------------------------------------------------------ */
/* fmt83: 11-byte space-padded 8.3 name -> "NAME.EXT" display string. */
static void fmt83(char *dst, const char *n11)
{
    unsigned char i, j = 0;
    for (i = 0; i < 8 && n11[i] && n11[i] != ' '; i++) dst[j++] = n11[i];
    if (n11[8] && n11[8] != ' ') {
        dst[j++] = '.';
        for (i = 8; i < 11 && n11[i] && n11[i] != ' '; i++) dst[j++] = n11[i];
    }
    dst[j] = 0;
}

static const char *const paint_exts[] = { "PIC", 0 };
static char name83[11];
static char namebuf[16];

static void copy11(char *d, const char *s)
{
    unsigned char i;
    for (i = 0; i < 11; i++) d[i] = s[i];
}

static unsigned char is_blank_name(const char *n)
{
    return (unsigned char)(n[0] == 0 || n[0] == ' ');
}

static unsigned char is_pic_name(const char *n)
{
    return (unsigned char)(n[8] == 'P' && n[9] == 'I' && n[10] == 'C');
}

static void set_doc_name(const char *n11)
{
    copy11(cur_name, n11);
    gb_set_name(cur_name);
    win_title();
}

static void to_83(const char *src)
{
    unsigned char i = 0, j;
    for (j = 0; j < 11; j++) name83[j] = ' ';
    for (j = 0; j < 8 && src[i] && src[i] != '.'; j++) name83[j] = src[i++];
    while (src[i] && src[i] != '.') i++;
    if (src[i] == '.') {
        i++;
        for (j = 0; j < 3 && src[i]; j++) name83[8 + j] = src[i++];
    }
    if (name83[8] == ' ') { name83[8] = 'P'; name83[9] = 'I'; name83[10] = 'C'; }
}

static void reset_editable_picture(void)
{
    close_banked();
    editable = 1;
    paint_mode = MODE_EDIT;
    sel_on = 0;
    edit_full_canvas();
    pic_toobig = 0;
    pic_wb = CANVAS_WB;
    pic_h = CANVAS_H;
    pic_off = PIC_HDR;
    pic_file_len = PIC_LEN;
    scroll_x = 0;
    scroll_y = 0;
    tile_x = 0;
    tile_y = 0;
    undo_valid = 0;
    tile_dirty = 0;
    bank_dirty = 0;
}

static unsigned char parse_picbuf(unsigned int got)
{
    unsigned int need, w;
    if (got < 6 ||
        picbuf[0] != 'G' || picbuf[1] != 'B' || picbuf[2] != 'P' || picbuf[3] != 'C')
        return 0;
    if (picbuf[4] == 2) {
        if (got < PIC_HDR) return 0;
        if (picbuf[5] != 1) return 0;        /* banked legacy mode-6 files normalize in the kernel */
        w = (unsigned char)picbuf[6] | ((unsigned int)(unsigned char)picbuf[7] << 8);
        pic_wb = (unsigned char)((w + 3) >> 2);
        pic_h = (unsigned char)picbuf[8] | ((unsigned int)(unsigned char)picbuf[9] << 8);
        pic_off = PIC_HDR;
        editable = (unsigned char)(w == CANVAS_W && pic_h == CANVAS_H);
    } else {
        pic_wb = (unsigned char)picbuf[4];
        pic_h = (unsigned char)picbuf[5];
        pic_off = 6;
        editable = 0;
    }
    need = pic_off + (unsigned int)pic_wb * pic_h;
    if (got < need) return 0;
    pic_file_len = got;
    pic_toobig = 0;
    scroll_x = 0;
    scroll_y = 0;
    tile_x = 0;
    tile_y = 0;
    undo_valid = 0;
    tile_dirty = 0;
    bank_dirty = 0;
    edit_full_canvas();
    sel_on = 0;
    paint_mode = editable ? MODE_EDIT : MODE_VIEW;
    return 1;
}

static void load_current_picture(void)
{
    unsigned int got;
    close_banked();
    got = gb_fs_load((char *)picbuf, PIC_MAX);
    if (parse_picbuf(got)) { dirty = 0; return; }

#ifndef GB_PCW
    load_picedit_helper();
    banked = gb_pic_open();
    UI_MODAL_K = 0;                      /* picture loader reuses UI scratch */
    if (banked) {
        banked2 = PIC_PAGE2_K;
        pic_wb = PIC_WB_K;
        pic_h = PIC_H_K;
        pic_off = PIC_OFF_K;
        pic_file_len = PIC_SIZE_K;
        editable = (unsigned char)(pic_wb >= CANVAS_WB && pic_h >= CANVAS_H);
        paint_mode = MODE_VIEW;
        edit_full_canvas();
        sel_on = 0;
        pic_toobig = 0;
        scroll_x = 0;
        scroll_y = 0;
        tile_x = 0;
        tile_y = 0;
        undo_valid = 0;
        tile_dirty = 0;
        bank_dirty = 0;
        dirty = 0;
        return;
    }
#endif

    reset_editable_picture();
    canvas_clear();
    dirty = 0;
    if (is_pic_name(cur_name)) {
        editable = 0;
        pic_toobig = 1;
        pic_wb = 26;
        pic_h = 32;
    }
}

/* stamp the editable 100x100 canvas as a v2 .PIC header. */
static unsigned int p_save(void)
{
    picbuf[0] = 'G'; picbuf[1] = 'B'; picbuf[2] = 'P'; picbuf[3] = 'C';
    picbuf[4] = 2;                       /* version */
    picbuf[5] = 1;                       /* canonical portable Mode-1 packing */
    picbuf[6] = CANVAS_W; picbuf[7] = 0; /* width_px  = 100 */
    picbuf[8] = CANVAS_H; picbuf[9] = 0; /* height_px = 100 */
    picbuf[10] = pic_inks[0]; picbuf[11] = pic_inks[1];
    picbuf[12] = pic_inks[2]; picbuf[13] = pic_inks[3];
    return PIC_LEN;
}

static unsigned char copy_banked_chunk(unsigned int off, unsigned int len)
{
    PIC_PAGE_K = banked;
    PIC_PAGE2_K = banked2;
    gb_pic_edit_buf = (unsigned int)gb_copybuf;
    gb_pic_edit_off = off;
    FS_SAVE_LEN_K = len;
    return gb_pic_edit(GB_PICEDIT_CHUNK);
}

static unsigned char save_banked_picture(void)
{
    unsigned int off = 0, n, room;
    if (!commit_tile()) {
        gb_alert("Save failed", "Tile write error");
        return 0;
    }
    while (off < pic_file_len) {
        n = (unsigned int)(pic_file_len - off);
        if (n > GB_COPYMAX) n = GB_COPYMAX;
        if (off < 0x4000) {
            room = (unsigned int)(0x4000 - off);
            if (n > room) n = room;
        }
        if (!n || !copy_banked_chunk(off, n)) break;
        FS_XFLAGS_K = off ? 0x06 : 0x04;       /* create first, append later */
        if (!gb_fs_save(gb_copybuf, n)) break;
        off += n;
    }
    FS_XFLAGS_K = 0;
    if (off == pic_file_len) { dirty = 0; bank_dirty = 0; undo_valid = 0; return 1; }
    gb_alert("Save failed", "Check disk");
    return 0;
}

static void do_new(void)
{
    reset_editable_picture();
    canvas_clear();
    set_doc_name("UNTITLEDPIC");
    dirty = 0;
}

static unsigned char do_save(void)
{
    unsigned int n;
    if (banked && editable) {
        if (save_banked_picture()) {
            paint_mode = MODE_VIEW;
            sel_on = 0;
            gb_wm_damage(win_x, win_y, winw, winh);
            return 1;
        }
        return 0;
    }
    if (!editable) {
        gb_alert("Read-only .PIC", "Open is view-only");
        return 0;
    }
    n = p_save();
    if (gb_fs_save((char *)picbuf, n)) { dirty = 0; undo_valid = 0; return 1; }
    gb_alert("Save failed", "Check disk");
    return 0;
}

static unsigned char do_saveas(void)
{
    if (!editable) {
        gb_alert("Read-only .PIC", "Open is view-only");
        return 0;
    }
    if (!gb_pickdir(paint_exts)) return 0;
    if (!gb_prompt("Save as:", namebuf, 12)) return 0;
    to_83(namebuf);
    set_doc_name(name83);
    return do_save();
}

static const char *const confirm_items[] = { "Save", "Don't Save", "Cancel" };
static unsigned char confirm_save(void)
{
    unsigned char sel;
    if (!dirty) return 1;
    sel = gb_popup(28, 90, confirm_items, 3);
    if (sel == 0) return do_save();
    if (sel == 1) return 1;
    return 0;
}

static void do_load(void)
{
    char nm[11];
    if (!confirm_save()) return;
    if (!gb_pickfile(nm, paint_exts)) return;
    set_doc_name(nm);
    load_current_picture();
}

/* ---- top-bar File/Edit/View menus ------------------------------------------ */
#define MENU_FILE_X 10
#define MENU_EDIT_X 18
#define MENU_VIEW_X 26
static const unsigned char menu_def[] = {
    3,
    MENU_FILE_X, 'F','i','l','e',0,0,0,0,
    MENU_EDIT_X, 'E','d','i','t',0,0,0,0,
    MENU_VIEW_X, 'V','i','e','w',0,0,0,0
};
static const char *const file_items[] = { "New", "Load", "Save", "Save As", "Quit" };
static const char *const edit_items[] = { "Undo", "Select", "Paint" };
static const char *const view_items[] = { "Reset View", "Center Tools" };
static unsigned char want_menu;

static unsigned char in_title(unsigned char col, unsigned char x, unsigned char chars)
{
    unsigned char w = (unsigned char)((chars * 6 + 3) / 4);
    return (unsigned char)(col >= x && col < (unsigned char)(x + w));
}

static void center_tools(void)
{
    tc_x = (unsigned char)(win_x + winw - TC_WIN_W - 3);
    tc_y = (unsigned char)(win_y + TITLE_H + 6);
    clamp_toolchest();
}

static void reset_view(void)
{
    if (paint_mode == MODE_EDIT && tile_dirty && !commit_tile()) return;
    if (banked) paint_mode = MODE_VIEW;
    sel_on = 0;
    scroll_x = 0;
    scroll_y = 0;
    tile_x = 0;
    tile_y = 0;
}

static void run_menu(void)
{
    unsigned char m = want_menu, sel;
    want_menu = 0;
    if (m == 1) {
        sel = gb_popup(MENU_FILE_X, 8, file_items, 5);
        if      (sel == 0) { if (confirm_save()) do_new(); }
        else if (sel == 1) do_load();
        else if (sel == 2) do_save();
        else if (sel == 3) do_saveas();
        else if (sel == 4) { p_close(); return; }
    } else if (m == 2) {
        sel = gb_popup(MENU_EDIT_X, 8, edit_items, 3);
        if (sel == 0 && editable && paint_mode == MODE_EDIT) do_undo();
        else if (sel == 1) begin_select();
        else if (sel == 2) enter_edit();
    } else if (m == 3) {
        sel = gb_popup(MENU_VIEW_X, 8, view_items, 2);
        if (sel == 0) reset_view();
        else if (sel == 1) center_tools();
    }
    win_title();
    gb_wm_damage(win_x, win_y, winw, winh);
}

static void on_menu(void)
{
    unsigned char col;
    if (gb_modal()) { gb_popup_close(); return; }
    col = gb_msg.p0;
    if      (in_title(col, MENU_FILE_X, 4)) want_menu = 1;
    else if (in_title(col, MENU_EDIT_X, 4)) want_menu = 2;
    else if (in_title(col, MENU_VIEW_X, 4)) want_menu = 3;
}

static unsigned char shortcuts(void)
{
    unsigned char c = gb_getkey();
    if (!c) return 0;
    c |= 0x20;
    if (c == 'u' && (paint_mode == MODE_SELECT || (paint_mode == MODE_EDIT && banked))) {
        cancel_select();
        return 1;
    }
    if (c == 'e') { enter_edit(); return 1; }
    if (c == 's') return do_save();
    return 0;
}

/* on_frame (#146): the WM handled close/drag; run the menu framework, then the
   CONTINUOUS canvas drawing (fire held) - the toolchest/title are discrete (on_click). */
static void p_frame(void)
{
    unsigned char my, cy;
    unsigned int px, cxl;

    sync_rect();
    calc_view();
    clamp_toolchest();
    if (paint_mode == MODE_EDIT) sync_edit_rect();
    win_title();                                       /* keep wtitle fresh for the WM title */
    if (want_menu) { run_menu(); gb_restore_parent(); return; }
    if (track_select()) return;
    if (shortcuts()) { gb_restore_parent(); return; }

    if (paint_mode != MODE_EDIT || !editable || !edit_visible || !(gb_flags() & GB_FIRE)) { fire_prev = 0; return; }   /* canvas drawing */
    my = gb_my();
    if (over_toolchest(gb_mx(), my)) { fire_prev = 1; return; }
    if (my < CVY) { fire_prev = 1; return; }
    cy = (unsigned char)(my - CVY);
    if (cy >= edit_ph) { fire_prev = 1; return; }
    px = gb_mxp();
    cxl = (unsigned int)CVX * 4;                       /* canvas left, in pixels */
    if (px < cxl) { fire_prev = 1; return; }
    px -= cxl;
    if (px >= edit_pw) { fire_prev = 1; return; }
    if (tool == TOOL_PENCIL) {                         /* continuous freehand */
        if (!fire_prev) save_undo();                   /* once, at the stroke start */
        plot((unsigned char)px, cy);
    } else if (!fire_prev) {                           /* fill / shapes: once per press */
        if (tool == TOOL_FILL) { save_undo(); flood_fill((unsigned char)px, cy); blit_canvas(); }
        else if (tool == TOOL_SQUARE || tool == TOOL_CIRCLE) rubber_band((unsigned char)px, cy);
    }
    fire_prev = 1;
}

/* on_click (#146): a content press - only the toolchest is discrete (a canvas press is
   handled continuously by the fire path in p_frame). */
static void p_click(void)
{
    sync_rect();
    calc_view();
    clamp_toolchest();
    if (paint_mode == MODE_EDIT) sync_edit_rect();
    if (hit_toolchest(gb_mx(), gb_my())) return;
    if (paint_mode == MODE_SELECT) {
        select_click();
        draw();
        return;
    }
    if (paint_mode == MODE_EDIT) return;
    if (view_vsb && gb_mx() >= (unsigned char)(view_x - SB_W) && gb_mx() < view_x &&
        gb_my() >= view_y && gb_my() < (unsigned char)(view_y + view_h)) {
        sb_drag();
        return;
    }
    if (view_hsb && hbar_w &&
        gb_mx() >= hbar_x && gb_mx() < (unsigned char)(hbar_x + hbar_w) &&
        gb_my() >= hbar_y && gb_my() < (unsigned char)(hbar_y + HSB_H)) {
        hsb_drag();
        return;
    }
}

/* on_drag (#146): a title-bar press -> move the window. */
static void p_drag(void)
{
    sync_rect();
    if (gb_drag_window(&win_x, &win_y, winw, winh)) {
        gb_wm_setpos(win_x, win_y);
        gb_restore_parent();
    }
}

/* on_close (#146): offer to save, then close (or repaint on cancel). */
static void p_close(void)
{
    if (confirm_save()) { close_banked(); gb_wm_close(); }
    else gb_restore_parent();
}

/* the window's single handler (#148). */
static void p_proc(void)
{
    switch (gb_msg.type) {
        case GB_MSG_DRAW:  p_draw();  break;
        case GB_MSG_CLICK: p_click(); break;
        case GB_MSG_FRAME: p_frame(); break;
        case GB_MSG_CLOSE: p_close(); break;
        case GB_MSG_DRAG:  p_drag();  break;
        case GB_MSG_MENU:
        case GB_MSG_DROP:  on_menu(); break;
    }
}

static const gb_mwin_t pmw = {
    DEF_X, DEF_Y, WIN_W, WIN_H, 0, 0,                  /* min_w=0: not grip-resizable */
    p_proc, wtitle
};

void main(void)
{
    unsigned char i;

    pen = 2;
    tool = TOOL_PENCIL;
    pen_w = 1;
    undo_valid = 0;
    fire_prev = 0;
    canvas_clear();
    gb_wm_managed(&pmw);                          /* register FIRST (no draw): captures our arg */
    sync_rect();
    gb_get_name(cur_name);                        /* preserve launch file while loading PAINT.IST */
    load_tools();                                 /* PAINT.IST -> ist[] (sets fs name) */
    if (is_pic_name(cur_name)) {
        set_doc_name(cur_name);
        load_current_picture();
    } else {
        set_doc_name("UNTITLEDPIC");
        reset_editable_picture();
        editable = 0;
        paint_mode = MODE_VIEW;
        pic_wb = 0;
        pic_h = 0;
        dirty = 0;
    }
    center_tools();
    gb_menu(menu_def);
    win_title();                                 /* build wtitle before the first paint */
    for (i = 64; i; i--) if (!gb_getkey()) break;   /* drop buffered keys */
    gb_restore_parent();                         /* first paint: WM chrome + p_draw */
}
