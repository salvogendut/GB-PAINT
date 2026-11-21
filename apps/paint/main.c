/*
 * PAINT.APP - a banked, three-pane GEOBENCH picture editor.
 *
 * Paint owns one invisible full-screen WM workspace and draws three ordinary
 * GEOBENCH windows inside it. This avoids registering several windows against
 * one application page (closing any such window would release the shared page):
 *
 *   Toolchest    always visible and always painted on top
 *   Area selector 1:1 scrollable picture preview with a fixed 20x20 navigator
 *   Canvas       the selected 20x20 pixels enlarged to a 160x160 work area
 *
 * The document itself lives in one borrowed 16 KiB application-pool page.
 * Only the selected 20x20 pixels and undo snapshot are retained in
 * this application page. Mode-1 pictures are portable across CPC/MSX/PCW;
 * mode-7 pictures and sixteen-colour editing are accepted only by MSX Paint
 * running under Screen 7.
 */
#include "gb.h"
#include <string.h>

#define TITLE_H 14
#define PIC_HDR 14
#define DOC_MAX 0x4000U

#define APP_NPAGES    (*(volatile unsigned char *)0x1437)
#define APP_PAGES     ((volatile unsigned char *)0x1438)
#define APP_BUSY      ((volatile unsigned char *)0x1440)
#define PIC_PAGE_K    (*(volatile unsigned char *)0x130B)
#define PIC_PAGE2_K   (*(volatile unsigned char *)0x1348)
#define PIC_SIZE_K    (*(volatile unsigned int  *)0x1349)
#define PIC_WB_K      (*(volatile unsigned char *)0x130C)
#define PIC_H_K       (*(volatile unsigned int  *)0x130D)
#define PIC_OFF_K     (*(volatile unsigned char *)0x130F)
#define FS_LOAD_OFS   ((volatile unsigned char *)0x144C)
#define FS_XFLAGS_K   (*(volatile unsigned char *)0x144F)
#define FS_SAVE_LEN_K (*(volatile unsigned int  *)0x14FD)
#define UI_MODAL_K    (*(volatile unsigned char *)0x1705)

#ifdef GB_MSX2
#define PIC_PAGE3_K   (*(volatile unsigned char *)0x1291)
#define PIC_PAGE4_K   (*(volatile unsigned char *)0x1292)
#define PIC_MODE_K    (*(volatile unsigned char *)0x1293)
#define PIC_STRIDE_K  (*(volatile unsigned int  *)0x1294)
#define MSX_SCRMOD    (*(volatile unsigned char *)0xFCAF)
#endif

#define TILE_SIDE 20
#define TILE_PIXELS (TILE_SIDE * TILE_SIDE)
#define TILE_PACKED (TILE_PIXELS / 2)
#define WORK_SCALE 8
#define WORK_PIXELS (TILE_SIDE * WORK_SCALE)
#define WORK_WB (WORK_PIXELS / 4)
#define PAINT_CLIP_MAGIC 0x50
#define PAINT_CLIP_MAX (TILE_PIXELS + 3)

/* PAINT.IST order. Action tools are buttons rather than persistent modes. */
#define TOOL_PENCIL  0
#define TOOL_LINE    1
#define TOOL_RECT    2
#define TOOL_RECTF   3
#define TOOL_CIRCLE  4
#define TOOL_CIRCLEF 5
#define TOOL_BUCKET  6
#define TOOL_SPRAY   7
#define TOOL_SELECT  8
#define TOOL_CUT     9
#define TOOL_COPY   10
#define TOOL_PASTE  11
#define TOOL_UNDO   12
#define N_TOOLS     13

#define TOOL_WB 6
#define TOOL_H  21
#define TOOL_STEP_X 7                 /* 24 px icon + one 4 px byte-column gutter */
#define TOOL_STEP_Y 23                /* 21 px icon + two scan-line gutter */
#define TOOL_ROWS 4

#define TC_W 29
#define TC_H 126
#define PV_W 33
#define PV_H 92
#define WK_W (WORK_WB + 2)
#define WK_H (TITLE_H + WORK_PIXELS + 1)
#define SB_W 3
#define HSB_H 6

#define PANE_PREVIEW 0
#define PANE_WORK    1

#define IST_MAX 1800
#define TOOL_BITS_PER ((TOOL_WB * 4 * TOOL_H) / 8)
#define TOOL_BITS_LEN (N_TOOLS * TOOL_BITS_PER)
/* Keep packed tool artwork above the bounded file-I/O staging area. The CPC
 * picture helper uses only the lower 2500 bytes of gb_copybuf; every Paint file
 * transfer is capped below this cache on all targets. A clobbered marker simply
 * causes PAINT.IST to be reloaded before the next Toolchest repaint. */
#define TOOL_CACHE_MARK 0xA7
#define TOOL_CACHE_OFFSET (GB_COPYMAX - TOOL_BITS_LEN - 2)
#define TOOL_CACHE ((unsigned char *)gb_copybuf + TOOL_CACHE_OFFSET)
#define TOOL_BITS (TOOL_CACHE + 1)
#define PAINT_IO_MAX 512
#define NATIVE_STAGE ((unsigned char *)gb_copybuf + 1024)

#define IO_IDLE 0
#define IO_LOAD 1
#define IO_NEW  2
#define IO_SAVE 3

#define AFTER_NONE      0
#define AFTER_NEW       1
#define AFTER_LOAD      2
#define AFTER_CLOSE_DOC 3
#define AFTER_CLOSE_APP 4

#define CONFIRM_CANCEL 0
#define CONFIRM_NOW    1
#define CONFIRM_WAIT   2

/* The executable icon preamble is no longer needed after entry. Reuse nine of
 * its payload bytes for transient job state instead of consuming scarce app
 * data space on dual-icon MSX builds. */
#define io_job         (*(volatile unsigned char *)0x4010)
#define io_after       (*(volatile unsigned char *)0x4011)
#define io_first       (*(volatile unsigned char *)0x4012)
#define io_off         (*(volatile unsigned int  *)0x4013)
#define pending_width  (*(volatile unsigned int  *)0x4015)
#define pending_height (*(volatile unsigned int  *)0x4017)

static unsigned char tc_x, tc_y;
static unsigned char pv_x, pv_y;
static unsigned char wk_x, wk_y;
static unsigned char front_pane;
static unsigned char work_visible;

static unsigned char doc_page;
static unsigned int doc_len;
static unsigned int pic_width;
static unsigned int pic_height;
static unsigned int pic_stride;
static unsigned char pic_wb;
static unsigned char pic_off;
static unsigned char pic_mode;
static unsigned char loaded;
static unsigned char named;
static unsigned char dirty;
static char cur_name[11];
static char launch_name[11];

static unsigned int scroll_x;
static unsigned int scroll_y;
static unsigned int tile_x;
static unsigned int tile_y;
static unsigned char tile_w, tile_h;
static unsigned char tile[TILE_PIXELS];
/* Undo retains all sixteen MSX pens while using one nibble per source pixel.
 * Its 200 bytes reuse the consumed executable-icon payload after app entry;
 * 0x4030 follows the I/O/drag scratch and ends safely below code at 0x4110. */
#define undo_tile ((unsigned char *)0x4030)
static unsigned char undo_valid;
static unsigned char work_sel_on;
static unsigned char work_sel_x0, work_sel_y0, work_sel_x1, work_sel_y1;

static unsigned char current_tool;
static unsigned char current_pen;
static unsigned char stroke_active;
static unsigned char stroke_x, stroke_y;
static unsigned char random_state = 0x5D;

static unsigned char ist_ok;
static unsigned char pv_view_x, pv_view_y, pv_view_w, pv_view_h;
static unsigned char pv_image_x, pv_image_y;
static unsigned int sel_px;
static unsigned char sel_py;
static unsigned char pv_hbar_x, pv_hbar_y, pv_hbar_w;
static unsigned char want_menu;

static void repaint_all(void);
static void draw_toolchest(void);
static void draw_preview(void);
static void draw_work(void);
static void close_app(void);
static unsigned char save_document(unsigned char after);
static unsigned char commit_tile(void);
static unsigned char unpack_mode1(unsigned char value, unsigned char pixel);
static void do_load(void);


/* ---- exact-pixel line primitive ------------------------------------------ */

static volatile unsigned int fx0, fy0, fx1, fy1;
static volatile unsigned char fpen;

#if defined(GB_MSX2) || defined(GB_PCW)
#ifdef GB_PCW
#define GLINE_BASE 0x0F10
#else
#define GLINE_BASE 0xC030
#endif
#define GLINE_X0  (*(volatile unsigned int  *)(GLINE_BASE + 0))
#define GLINE_Y0  (*(volatile unsigned int  *)(GLINE_BASE + 2))
#define GLINE_X1  (*(volatile unsigned int  *)(GLINE_BASE + 4))
#define GLINE_Y1  (*(volatile unsigned int  *)(GLINE_BASE + 6))
#define GLINE_PEN (*(volatile unsigned char *)(GLINE_BASE + 8))
static void fw_line(void) __naked
{
__asm
    call 0x8009
    ret
__endasm;
}

static void line(int x0, int y0, int x1, int y1, unsigned char pen)
{
    GLINE_X0 = (unsigned int)x0;
    GLINE_Y0 = (unsigned int)y0;
    GLINE_X1 = (unsigned int)x1;
    GLINE_Y1 = (unsigned int)y1;
    GLINE_PEN = pen;
    fw_line();
}

#ifdef GB_PCW
static void fw_frame(void) __naked
{
__asm
    ld   a, (_fpen)
    ld   (0x0f18), a
    ld   hl, (_fx0)
    ld   (0x0f10), hl
    ld   hl, (_fy0)
    ld   (0x0f12), hl
    ld   (0x0f16), hl
    ld   hl, (_fx1)
    ld   (0x0f14), hl
    call 0x8009
    ld   hl, (_fy1)
    ld   (0x0f12), hl
    ld   (0x0f16), hl
    call 0x8009
    ld   hl, (_fx0)
    ld   (0x0f10), hl
    ld   (0x0f14), hl
    ld   hl, (_fy0)
    ld   (0x0f12), hl
    call 0x8009
    ld   hl, (_fx1)
    ld   (0x0f10), hl
    ld   (0x0f14), hl
    call 0x8009
    ret
__endasm;
}
#else
static void fw_frame(void) __naked
{
__asm
    ld   a, (_fpen)
    ld   (0xc038), a
    ld   hl, (_fx0)
    ld   (0xc030), hl
    ld   hl, (_fy0)
    ld   (0xc032), hl
    ld   (0xc036), hl
    ld   hl, (_fx1)
    ld   (0xc034), hl
    call 0x8009
    ld   hl, (_fy1)
    ld   (0xc032), hl
    ld   (0xc036), hl
    call 0x8009
    ld   hl, (_fx0)
    ld   (0xc030), hl
    ld   (0xc034), hl
    ld   hl, (_fy0)
    ld   (0xc032), hl
    call 0x8009
    ld   hl, (_fx1)
    ld   (0xc030), hl
    ld   (0xc034), hl
    call 0x8009
    ret
__endasm;
}
#endif
#else
static void fw_line(void) __naked
{
__asm
    ld   a, (_fpen)
    call 0xBBDE
    ld   de, (_fx0)
    ld   hl, (_fy0)
    call 0xBBC0
    ld   de, (_fx1)
    ld   hl, (_fy1)
    call 0xBBF6
    ret
__endasm;
}

static void line(int x0, int y0, int x1, int y1, unsigned char pen)
{
    fx0 = (unsigned int)(x0 * 2);
    fy0 = (unsigned int)((199 - y0) * 2);
    fx1 = (unsigned int)(x1 * 2);
    fy1 = (unsigned int)((199 - y1) * 2);
    fpen = pen;
    fw_line();
}

static void fw_frame(void) __naked
{
__asm
    ld   hl, (_fx0)
    add  hl, hl
    ld   (_fx0), hl
    ld   hl, (_fx1)
    add  hl, hl
    ld   (_fx1), hl
    ld   hl, #199
    ld   de, (_fy0)
    xor  a
    sbc  hl, de
    add  hl, hl
    ld   (_fy0), hl
    ld   hl, #199
    ld   de, (_fy1)
    xor  a
    sbc  hl, de
    add  hl, hl
    ld   (_fy1), hl

    ld   a, (_fpen)
    call 0xBBDE
    ld   de, (_fx0)
    ld   hl, (_fy0)
    call 0xBBC0
    ld   de, (_fx1)
    call 0xBBF6
    ld   de, (_fx0)
    ld   hl, (_fy1)
    call 0xBBC0
    ld   de, (_fx1)
    call 0xBBF6
    ld   de, (_fx0)
    ld   hl, (_fy0)
    call 0xBBC0
    ld   de, (_fx0)
    ld   hl, (_fy1)
    call 0xBBF6
    ld   de, (_fx1)
    ld   hl, (_fy0)
    call 0xBBC0
    ld   de, (_fx1)
    ld   hl, (_fy1)
    call 0xBBF6
    ret
__endasm;
}
#endif

/* ---- small helpers ------------------------------------------------------- */

static void copy11(char *dst, const char *src)
{
    unsigned char i;
    for (i = 0; i < 11; i++) dst[i] = src[i];
}

static unsigned char is_pic_name(const char *name)
{
    return (unsigned char)(name[8] == 'P' && name[9] == 'I' &&
                           name[10] == 'C');
}

static void to_83(const char *src, char *dst)
{
    unsigned char i = 0, j;
    for (j = 0; j < 11; j++) dst[j] = ' ';
    for (j = 0; j < 8 && src[i] && src[i] != '.'; j++)
        dst[j] = src[i++];
    while (src[i] && src[i] != '.') i++;
    if (src[i] == '.') {
        i++;
        for (j = 0; j < 3 && src[i]; j++) dst[8 + j] = src[i++];
    }
    if (dst[8] == ' ') {
        dst[8] = 'P';
        dst[9] = 'I';
        dst[10] = 'C';
    }
}

static unsigned char inside(unsigned char x, unsigned char y,
                            unsigned char w, unsigned char h,
                            unsigned char mx, unsigned char my)
{
    return (unsigned char)(mx >= x && mx < (unsigned char)(x + w) &&
                           my >= y && my < (unsigned char)(y + h));
}

static unsigned char close_hit(unsigned char x, unsigned char y,
                               unsigned char mx, unsigned char my)
{
    return (unsigned char)(mx >= (unsigned char)(x + 1) &&
                           mx <  (unsigned char)(x + 4) &&
                           my >= (unsigned char)(y + 2) &&
                           my <  (unsigned char)(y + 12));
}

static unsigned char title_hit(unsigned char x, unsigned char y,
                               unsigned char w,
                               unsigned char mx, unsigned char my)
{
    return (unsigned char)(mx >= x && mx < (unsigned char)(x + w) &&
                           my >= y && my < (unsigned char)(y + TITLE_H));
}

static void clear_name(void)
{
    unsigned char i;
    for (i = 0; i < 11; i++) cur_name[i] = ' ';
}

static const char *tool_title(void)
{
    return dirty ? "Toolchest *" : "Toolchest";
}

/* ---- portable Mode-1 resource display ----------------------------------- */

#ifdef GB_PCW
static unsigned char pcw_byte(unsigned char value)
{
    unsigned char i, pen, native = 0;
    for (i = 0; i < 4; i++) {
        pen = (unsigned char)(((value >> (7 - i)) & 1) |
                              (((value >> (3 - i)) & 1) << 1));
        native |= (unsigned char)(pen << (6 - 2 * i));
    }
    return (unsigned char)(((native & 0x55) << 1) |
                           (((native ^ 0xFF) & 0xAA) >> 1));
}
#endif

static void blit_mode1(unsigned char x, unsigned char y,
                       unsigned char w, unsigned char h,
                       const unsigned char *source,
                       unsigned char stride)
{
#if !defined(GB_MSX2) && !defined(GB_PCW)
    unsigned char row;
    if (w == stride) {
        gb_restorerect(x, y, w, h, source);
        return;
    }
    for (row = 0; row < h; row++) {
        gb_restorerect(x, (unsigned char)(y + row), w, 1, source);
        source += stride;
    }
#else
    unsigned char row;
#ifdef GB_PCW
    unsigned char col;
#endif
    for (row = 0; row < h; row++) {
#ifdef GB_MSX2
        gb_pic_edit_buf = (unsigned int)source;
        gb_pic_edit_off = (unsigned int)NATIVE_STAGE;
        FS_SAVE_LEN_K = w;
        if (!gb_pic_edit(GB_PICEDIT_NATIVE)) return;
#else
        for (col = 0; col < w; col++) NATIVE_STAGE[col] = pcw_byte(source[col]);
#endif
        gb_restorerect(x, (unsigned char)(y + row), w, 1, NATIVE_STAGE);
        source += stride;
    }
#endif
}

#ifdef GB_MSX2
static void native_blit(unsigned char x, unsigned char y,
                        unsigned char w, unsigned char h,
                        unsigned char *source)
{
    gb_pic_edit_buf = (unsigned int)source;
    gb_pic_edit_off = (unsigned int)x | ((unsigned int)y << 8);
    FS_SAVE_LEN_K = (unsigned int)w | ((unsigned int)h << 8);
    (void)gb_pic_edit(GB_PICEDIT_NATIVE16);
}

static void native_solid(unsigned char x, unsigned char y,
                         unsigned char w, unsigned char h,
                         unsigned char pen)
{
    unsigned int i, length = (unsigned int)w * 2U * h;
    unsigned char value = (unsigned char)((pen << 4) | pen);
    for (i = 0; i < length; i++) ((unsigned char *)gb_copybuf)[i] = value;
    native_blit(x, y, w, h, (unsigned char *)gb_copybuf);
}
#endif

/* ---- PAINT.IST ----------------------------------------------------------- */

static void load_tools(void)
{
    unsigned int got, off;
    unsigned char icon, x, y, value, nibble;
    unsigned char *source = (unsigned char *)gb_copybuf;
    unsigned char *dest = TOOL_BITS;
    TOOL_CACHE[0] = 0;
    TOOL_CACHE[TOOL_BITS_LEN + 1] = 0;
    gb_set_name("PAINT   IST");
    got = gb_fs_load((char *)source, IST_MAX);
    ist_ok = (unsigned char)(got >= 16 && source[0] == 'G' &&
        source[1] == 'B' && source[2] == 'I' && source[3] == 'S' &&
        source[4] == 2 && source[5] >= N_TOOLS);
    if (ist_ok) {
        for (icon = 0; icon < N_TOOLS; icon++) {
            unsigned int entry = 16U + (unsigned int)icon * 4U;
            off = (unsigned int)source[entry] |
                  ((unsigned int)source[entry + 1] << 8);
            if (source[entry + 2] != TOOL_WB || source[entry + 3] != TOOL_H ||
                off + TOOL_WB * TOOL_H > got) {
                ist_ok = 0;
                break;
            }
            for (y = 0; y < TOOL_H; y++)
                for (x = 0; x < TOOL_WB; x += 2) {
                    value = source[off + (unsigned int)y * TOOL_WB + x];
                    nibble = (unsigned char)((value >> 4) & ~value & 15);
                    value = source[off + (unsigned int)y * TOOL_WB + x + 1];
                    *dest++ = (unsigned char)((nibble << 4) |
                        ((value >> 4) & ~value & 15));
                }
        }
    }
    if (ist_ok) {
        TOOL_CACHE[0] = TOOL_CACHE_MARK;
        TOOL_CACHE[TOOL_BITS_LEN + 1] = (unsigned char)~TOOL_CACHE_MARK;
    }
    if (named) gb_set_name(cur_name);
}

static void ensure_tools(void)
{
    if (TOOL_CACHE[0] != TOOL_CACHE_MARK ||
        TOOL_CACHE[TOOL_BITS_LEN + 1] != (unsigned char)~TOOL_CACHE_MARK)
        load_tools();
}

#if !defined(GB_MSX2) && !defined(GB_PCW)
static void load_picedit_helper(void)
{
    unsigned int got, i;
    unsigned char *source;
    unsigned char *dest = (unsigned char *)0x1600;
    gb_set_name("DEFAULT SPR");
    got = gb_fs_load(gb_copybuf, 512);
    if (named) gb_set_name(cur_name);
    if (got <= 256) return;
    got -= 256;
    if (got > 256) got = 256;
    source = (unsigned char *)gb_copybuf + 256;
    for (i = 0; i < got; i++) dest[i] = source[i];
}
#endif

/* ---- borrowed document page --------------------------------------------- */

static void select_document(void)
{
    PIC_PAGE_K = doc_page;
    PIC_PAGE2_K = 0;
    PIC_SIZE_K = doc_len;
    PIC_WB_K = pic_wb;
    PIC_H_K = pic_height;
    PIC_OFF_K = pic_off;
#ifdef GB_MSX2
    PIC_PAGE3_K = 0;
    PIC_PAGE4_K = 0;
    PIC_MODE_K = pic_mode;
    PIC_STRIDE_K = pic_stride;
#endif
}

static unsigned char document_xfer(unsigned int off, void *buffer,
                                   unsigned int length, unsigned char op)
{
    if (!doc_page || !length || off >= DOC_MAX ||
        length > (unsigned int)(DOC_MAX - off)) return 0;
    select_document();
    gb_pic_edit_buf = (unsigned int)buffer;
    gb_pic_edit_off = off;
    FS_SAVE_LEN_K = length;
#if defined(GB_MSX2) || defined(GB_PCW)
    return gb_pic_edit(op);
#else
    if (gb_pic_edit(op)) return 1;
    load_picedit_helper();
    select_document();
    gb_pic_edit_buf = (unsigned int)buffer;
    gb_pic_edit_off = off;
    FS_SAVE_LEN_K = length;
    return gb_pic_edit(op);
#endif
}

static unsigned char document_read(unsigned int off, void *buffer,
                                   unsigned int length)
{
    return document_xfer(off, buffer, length, GB_PICEDIT_CHUNK);
}

static unsigned char document_write(unsigned int off, const void *buffer,
                                    unsigned int length)
{
    return document_xfer(off, (void *)buffer, length, GB_PICEDIT_WRITE);
}

static unsigned char allocate_document_page(void)
{
    unsigned char i;
    for (i = 0; i < APP_NPAGES; i++) {
        if (!APP_BUSY[i]) {
            APP_BUSY[i] = 1;
            doc_page = APP_PAGES[i];
            return 1;
        }
    }
    return 0;
}

static void release_document_page(void)
{
    if (!doc_page) return;
    select_document();
    gb_pic_close();
    doc_page = 0;
}

static void reset_editor_state(void)
{
    loaded = 0;
    named = 0;
    dirty = 0;
    work_visible = 0;
    undo_valid = 0;
    work_sel_on = 0;
    stroke_active = 0;
    scroll_x = scroll_y = 0;
    tile_x = tile_y = 0;
    clear_name();
}

static void close_document(void)
{
    release_document_page();
    reset_editor_state();
}

static unsigned char unpack_mode1(unsigned char value, unsigned char pixel)
{
    return (unsigned char)(((value >> (7 - pixel)) & 1) |
                           (((value >> (3 - pixel)) & 1) << 1));
}

static unsigned char replace_mode1(unsigned char value, unsigned char pixel,
                                   unsigned char pen)
{
    value &= (unsigned char)~((1 << (7 - pixel)) | (1 << (3 - pixel)));
    if (pen & 1) value |= (unsigned char)(1 << (7 - pixel));
    if (pen & 2) value |= (unsigned char)(1 << (3 - pixel));
    return value;
}

static unsigned char transfer_tile(unsigned char write)
{
    unsigned char row, col, first, last, span, position;
    unsigned int remain, i;
    unsigned char *buf = (unsigned char *)gb_copybuf;
    unsigned char *pixel;

    if (!write)
        for (i = 0; i < TILE_PIXELS; i++) tile[i] = 1;
    if (!loaded) return 0;
    if (tile_x >= pic_width || tile_y >= pic_height) return 0;
    remain = pic_width - tile_x;
    tile_w = (unsigned char)(remain > TILE_SIDE ? TILE_SIDE : remain);
    remain = pic_height - tile_y;
    tile_h = (unsigned char)(remain > TILE_SIDE ? TILE_SIDE : remain);
    if (pic_mode == 7) {
        first = (unsigned char)(tile_x >> 1);
        last = (unsigned char)((tile_x + tile_w - 1) >> 1);
        position = (unsigned char)tile_x & 1;
    } else {
        first = (unsigned char)(tile_x >> 2);
        last = (unsigned char)((tile_x + tile_w - 1) >> 2);
        position = (unsigned char)tile_x & 3;
    }
    span = (unsigned char)(last - first + 1);
    /* SDCC's high-allocation pass folds the three-term row offset through a
       stack address on Z80. Keep the live offset in fixed scratch words. */
    fx1 = tile_y;
    fx0 = fx1 * pic_stride;
    fx0 += pic_off;
    fx0 += first;
    for (row = 0; row < tile_h; row++) {
        if (!document_read(fx0, buf, span))
            return 0;
        pixel = &tile[(unsigned int)row * TILE_SIDE];
        for (col = 0; col < tile_w; col++) {
            unsigned char packed = (unsigned char)(position + col);
            if (pic_mode == 7) {
                unsigned char *value = &buf[packed >> 1];
                if (write) {
                    if (packed & 1)
                        *value = (unsigned char)((*value & 0xF0) | *pixel);
                    else
                        *value = (unsigned char)((*value & 0x0F) |
                                                 (*pixel << 4));
                } else {
                    *pixel = (unsigned char)((packed & 1) ?
                                             (*value & 15) : (*value >> 4));
                }
            } else {
                unsigned char index = packed >> 2;
                if (write)
                    buf[index] = replace_mode1(buf[index],
                        (unsigned char)(packed & 3), *pixel);
                else
                    *pixel = unpack_mode1(buf[index],
                                         (unsigned char)(packed & 3));
            }
            pixel++;
        }
        if (write && !document_write(fx0, buf, span))
            return 0;
        fx1++;
        fx0 += pic_stride;
    }
    if (write) dirty = 1;
    if (!write) {
        undo_valid = 0;
        work_sel_on = 0;
    }
    return 1;
}

static unsigned char load_tile(void)
{
    return transfer_tile(0);
}

static unsigned char commit_tile(void)
{
    return transfer_tile(1);
}

/* ---- drawing ------------------------------------------------------------- */

static unsigned char tool_x(unsigned char index)
{
    return (unsigned char)(tc_x + 1 + (index & 3) * TOOL_STEP_X);
}

static unsigned char tool_y(unsigned char index)
{
    return (unsigned char)(tc_y + TITLE_H +
                           (index >> 2) * TOOL_STEP_Y);
}

static void draw_tool_icon(unsigned char index)
{
    unsigned char x, y, bits, value;
    unsigned char *source = &TOOL_BITS[(unsigned int)index * TOOL_BITS_PER];
    unsigned char *out = (unsigned char *)gb_copybuf;
    for (y = 0; y < TOOL_H; y++)
        for (x = 0; x < TOOL_WB; x += 2) {
            bits = *source++;
            value = bits >> 4;
            *out++ = (unsigned char)((value << 4) | (~value & 15));
            value = bits & 15;
            *out++ = (unsigned char)((value << 4) | (~value & 15));
        }
    blit_mode1(tool_x(index), tool_y(index), TOOL_WB, TOOL_H,
               (unsigned char *)gb_copybuf, TOOL_WB);
}

static void draw_swatch(unsigned char index, unsigned char x,
                        unsigned char y, unsigned char w,
                        unsigned char h)
{
#ifdef GB_MSX2
    native_solid(x, y, w, h, index);
#else
    gb_fill(x, y, w, h, index);
#endif
    gb_frame(x, y, w, h, 2);
    if (index == current_pen)
        gb_frame(x, (unsigned char)(y - 1), w,
                 (unsigned char)(h + 2), 3);
}

static void draw_palette(void)
{
    unsigned char i;
    unsigned char py = (unsigned char)(tc_y + TITLE_H +
                                       TOOL_ROWS * TOOL_STEP_Y + 1);
#ifdef GB_MSX2
    for (i = 0; i < 16; i++)
        draw_swatch(i,
            (unsigned char)(tc_x + 1 + (i & 7) * 3),
            (unsigned char)(py + (i >> 3) * 9), 3, 8);
#else
    for (i = 0; i < 4; i++)
        draw_swatch(i, (unsigned char)(tc_x + 1 + i * 6), py, 6, 9);
#endif
}

static void draw_toolchest(void)
{
    unsigned char i;
    ensure_tools();
    gb_window(tc_x, tc_y, TC_W, TC_H, tool_title());
    if (ist_ok) for (i = 0; i < N_TOOLS; i++) draw_tool_icon(i);
    for (i = 0; i < N_TOOLS; i++)
        gb_frame(tool_x(i), tool_y(i), TOOL_WB, TOOL_H,
                 (i == current_tool) ? 3 : 2);
    draw_palette();
}

/* Compact 16-bit scroll tracks. A fixed five-pixel thumb keeps the mapper small
 * while still making the full document range reachable on very tall pictures. */
static unsigned char scroll_scale(unsigned int value, unsigned int limit,
                                  unsigned char travel)
{
    if (!limit || value >= limit) return travel;
    while (limit > 255U) {
        value >>= 1;
        limit = (unsigned int)((limit + 1U) >> 1);
    }
    return (unsigned char)((value * travel) / limit);
}

static unsigned int scroll_value(unsigned char origin, unsigned char track,
                                 unsigned int total, unsigned int page,
                                 unsigned char pointer)
{
    unsigned char rel, travel;
    unsigned int limit, q, r;
    if (page >= total || track <= 5) return 0;
    travel = (unsigned char)(track - 5);
    if (pointer <= (unsigned char)(origin + 2)) return 0;
    rel = (unsigned char)(pointer - origin - 2);
    limit = total - page;
    if (rel >= travel) return limit;
    q = limit / travel;
    r = limit % travel;
    return (unsigned int)((unsigned int)rel * q +
                          ((unsigned int)rel * r) / travel);
}

static void draw_vscroll(unsigned char x, unsigned char y,
                         unsigned char w, unsigned char h,
                         unsigned int pos, unsigned int total,
                         unsigned int page)
{
    unsigned char start = 0, length = h;
    if (page < total && h > 5) {
        length = 5;
        start = scroll_scale(pos, total - page, (unsigned char)(h - length));
    }
    gb_fill(x, y, w, h, 1);
    if (w > 2)
        gb_fill((unsigned char)(x + 1), (unsigned char)(y + start),
                (unsigned char)(w - 2), length, 3);
}

static void draw_hscroll(unsigned char x, unsigned char y,
                         unsigned char w, unsigned char h,
                         unsigned int pos, unsigned int total,
                         unsigned int page)
{
    unsigned char start = 0, length = w;
    if (page < total && w > 5) {
        length = 5;
        start = scroll_scale(pos, total - page, (unsigned char)(w - length));
    }
    gb_fill(x, y, w, h, 1);
    if (h > 2)
        gb_fill((unsigned char)(x + start), (unsigned char)(y + 1),
                length, (unsigned char)(h - 2), 3);
}

static void layout_preview(void)
{
    unsigned char body_w = (unsigned char)(PV_W - 2 - SB_W);
    unsigned char body_h = (unsigned char)(PV_H - TITLE_H - 1 - HSB_H);
    pv_view_x = (unsigned char)(pv_x + 1 + SB_W);
    pv_view_y = (unsigned char)(pv_y + TITLE_H);
    pv_view_w = body_w;
    pv_view_h = body_h;
    pv_hbar_x = pv_view_x;
    pv_hbar_y = (unsigned char)(pv_view_y + body_h);
    pv_hbar_w = body_w;
    if (loaded) {
        unsigned int maxx = (pic_wb > body_w) ? pic_wb - body_w : 0;
        unsigned int maxy = (pic_height > body_h) ? pic_height - body_h : 0;
        if (scroll_x > maxx) scroll_x = maxx;
        if (scroll_y > maxy) scroll_y = maxy;
        pv_image_x = (unsigned char)(pv_view_x +
            ((pic_wb < body_w) ? (unsigned char)((body_w - pic_wb) >> 1) : 0));
        pv_image_y = (unsigned char)(pv_view_y +
            ((pic_height < body_h) ?
             (unsigned char)((body_h - (unsigned char)pic_height) >> 1) : 0));
    } else {
        pv_image_x = pv_view_x;
        pv_image_y = pv_view_y;
    }
}

static void draw_selector(void)
{
    if (!loaded) return;
    sel_px = ((unsigned int)pv_image_x - scroll_x) * 4 + tile_x;
    sel_py = pv_image_y + tile_y - scroll_y;
    if (tile_x < scroll_x * 4 || tile_y < scroll_y ||
        sel_px + TILE_SIDE > (unsigned int)(pv_view_x + pv_view_w) * 4 ||
        (unsigned int)sel_py + TILE_SIDE > pv_view_y + pv_view_h) return;
    fx0 = sel_px;
    fx1 = sel_px + TILE_SIDE - 1;
    fy0 = sel_py;
    fy1 = sel_py + TILE_SIDE - 1;
    fpen = 3;
    fw_frame();
}

static void draw_preview(void)
{
    unsigned char draw_w, rows;
    unsigned int remain, source;
    gb_window(pv_x, pv_y, PV_W, PV_H, "Area selector");
    layout_preview();
    gb_fill((unsigned char)(pv_x + 1), (unsigned char)(pv_y + TITLE_H),
            (unsigned char)(PV_W - 2), (unsigned char)(PV_H - TITLE_H - 1), 1);
    if (!loaded) {
        gb_textbw((unsigned char)(pv_x + 4),
                  (unsigned char)(pv_y + TITLE_H + 8), "File > New or Load");
        return;
    }
    draw_w = (unsigned char)((pic_wb - scroll_x > pv_view_w) ?
                            pv_view_w : pic_wb - scroll_x);
    remain = pic_height - scroll_y;
    rows = (unsigned char)(remain > pv_view_h ? pv_view_h : remain);
    source = pic_off + scroll_y * pic_stride +
             (pic_mode == 7 ? scroll_x * 2U : scroll_x);
    if (draw_w && rows) {
        select_document();
#ifdef GB_MSX2
        gb_pic_blit(pv_image_x, pv_image_y, draw_w, rows, source);
#else
        if (draw_w == pic_wb) {
            gb_pic_blit(pv_image_x, pv_image_y, draw_w, rows, source);
        } else {
            fpen = pv_image_y;
            do {
                gb_pic_blit(pv_image_x, fpen, draw_w, 1, source);
                source += pic_stride;
                fpen++;
            } while (--rows);
        }
#endif
    }
    draw_vscroll((unsigned char)(pv_x + 1), pv_view_y, SB_W, pv_view_h,
                 scroll_y, pic_height, pv_view_h);
    draw_hscroll(pv_hbar_x, pv_hbar_y, pv_hbar_w, HSB_H,
                 scroll_x, pic_wb, pv_view_w);
    draw_selector();
}

/* Edit snapshots live above the bank-transfer scratch, below the tool cache.
 * They are used synchronously, never across event callbacks. */
#define BEFORE ((unsigned char *)gb_copybuf + 3072)

static unsigned char covered(unsigned char col, unsigned char row, unsigned char pane)
{
    if (inside(tc_x, tc_y, TC_W, TC_H, col, row)) return 1;
    if (pane == front_pane) return 0;
    if (pane == PANE_WORK) return inside(pv_x, pv_y, PV_W, PV_H, col, row);
    return work_visible && inside(wk_x, wk_y, WK_W, WK_H, col, row);
}

#ifndef GB_MSX2
#ifdef GB_PCW
static const unsigned char solid[] = {0x55, 0xff, 0, 0xaa};
#define LEFT_MASK 0xc0
#define RIGHT_MASK 0x03
#else
static const unsigned char solid[] = {0, 0xf0, 0x0f, 0xff};
#define LEFT_MASK 0x88
#define RIGHT_MASK 0x11
#endif
#endif

/* Generate one magnified row in transfer scratch. Only its first, middle and
 * last scanlines differ: copy the repeated middle scanlines rather than
 * unpacking each source pixel eight times. */
static unsigned char run_x, run_end, run_y, run_pitch;
static unsigned char *scan_pixels;
static void work_scan(unsigned char *out, unsigned char row)
{
    unsigned char x, body, left, right;
    for (x = run_x; x < run_end; ++x) {
        body = !row && run_y ? 2 : scan_pixels[x];
        left = x ? 2 : body;
        right = body;
        if (work_sel_on && x >= work_sel_x0 && x <= work_sel_x1 &&
            run_y >= work_sel_y0 && run_y <= work_sel_y1) {
            if ((!row && run_y == work_sel_y0) || (row == 7 && run_y == work_sel_y1))
                body = left = right = 3;
            if (x == work_sel_x0) left = 3;
            if (x == work_sel_x1) right = 3;
        }
#ifdef GB_MSX2
        *out++ = (left << 4) | body;
        *out++ = body * 17;
        *out++ = body * 17;
        *out++ = (body << 4) | right;
#else
        *out++ = (solid[left] & LEFT_MASK) | (solid[body] & ~LEFT_MASK);
        *out++ = (solid[right] & RIGHT_MASK) | (solid[body] & ~RIGHT_MASK);
#endif
    }
}

#ifdef GB_MSX2
#define WORK_BLIT native_blit
#define NATIVE_BYTES 2
#else
#define WORK_BLIT gb_restorerect
#define NATIVE_BYTES 1
#endif

static unsigned char overlap(unsigned char x, unsigned char y, unsigned char w,
                             unsigned char h, unsigned char ox, unsigned char oy,
                             unsigned char ow, unsigned char oh)
{
    return x < ox + ow && ox < x + w && y < oy + oh && oy < y + h;
}

static void draw_work_run(unsigned char first, unsigned char end, unsigned char y)
{
    unsigned char *buf = (unsigned char *)gb_copybuf, *out = buf;
    unsigned char row, col, start;
    unsigned char sx = wk_x + 1 + first * 2, sy = wk_y + TITLE_H + y * 8;
    unsigned char width = (end - first) * 2;
    run_x = first; run_end = end; run_y = y; run_pitch = width * NATIVE_BYTES;
    scan_pixels = &tile[(unsigned int)y * TILE_SIDE];
    for (row = 0; row < 8; ++row, out += run_pitch) {
        if (row < 2 || row == 7) work_scan(out, row);
        else memcpy(out, out - run_pitch, run_pitch);
    }
    if (!overlap(sx, sy, width, 8, tc_x, tc_y, TC_W, TC_H) &&
        (front_pane == PANE_WORK || !overlap(sx, sy, width, 8, pv_x, pv_y, PV_W, PV_H))) {
        WORK_BLIT(sx, sy, width, 8, buf);
        return;
    }
    for (row = 0; row < 8; ++row, ++sy, buf += run_pitch) {
        col = 0;
        while (col < width) {
            while (col < width && covered(sx + col, sy, PANE_WORK)) ++col;
            start = col;
            while (col < width && !covered(sx + col, sy, PANE_WORK)) ++col;
            if (col != start)
                WORK_BLIT(sx + start, sy, col - start, 1, buf + start * NATIVE_BYTES);
        }
    }
}

static void normalize_work_selection(void)
{
    unsigned char value;
    if (work_sel_x0 > work_sel_x1) {
        value = work_sel_x0; work_sel_x0 = work_sel_x1; work_sel_x1 = value;
    }
    if (work_sel_y0 > work_sel_y1) {
        value = work_sel_y0; work_sel_y0 = work_sel_y1; work_sel_y1 = value;
    }
}

static void canvas_pixels(void)
{
    unsigned char y;
    for (y = 0; y < TILE_SIDE; ++y) draw_work_run(0, TILE_SIDE, y);
}

static void draw_work(void)
{
    if (!work_visible) return;
    gb_window(wk_x, wk_y, WK_W, WK_H, "Canvas 8x");
    canvas_pixels();
}

static void repaint_picture_panes(void)
{
    if (loaded) {
        if (work_visible && front_pane == PANE_PREVIEW) {
            draw_work();
            draw_preview();
        } else {
            draw_preview();
            draw_work();
        }
    }
}

static void repaint_all(void)
{
    gb_curhide();
    repaint_picture_panes();
    draw_toolchest();
    gb_curshow();
}

static void draw_preview_pixel(unsigned char x, unsigned char y)
{
    unsigned int px;
    unsigned char py;
    if (!x || !y || x == TILE_SIDE - 1 || y == TILE_SIDE - 1) return;
    if (x >= tile_w || y >= tile_h) return;
    px = sel_px + x;
    py = sel_py + y;
    if (!inside(pv_view_x,pv_view_y,pv_view_w,pv_view_h,px >> 2,py) ||
        covered(px >> 2, py, PANE_PREVIEW)) return;
    line(px, py, px, py, tile[(unsigned int)y * TILE_SIDE + x]);
}

static void remember_pixels(void)
{
    memcpy(BEFORE, tile, TILE_PIXELS);
}

static void update_changes(unsigned char preview)
{
    unsigned char x, start, y;
    unsigned int i = 0;
    gb_curhide();
    for (y = 0; y < TILE_SIDE; ++y) {
        x = 0;
        while (x < TILE_SIDE) {
            if (BEFORE[i] == tile[i]) { ++i; ++x; continue; }
            start = x;
            do {
                if (preview) draw_preview_pixel(x, y);
                ++i; ++x;
            } while (x < TILE_SIDE && BEFORE[i] != tile[i]);
            draw_work_run(start, x, y);
        }
    }
    gb_curshow();
}

/* ---- work tools ---------------------------------------------------------- */

static void save_undo(void)
{
    unsigned int i;
    remember_pixels();
    for (i = 0; i < TILE_PACKED; i++)
        undo_tile[i] = (unsigned char)((tile[i << 1] << 4) |
                                      tile[(i << 1) + 1]);
    undo_valid = 1;
}

static void restore_undo_base(void)
{
    unsigned int i;
    unsigned char value;
    for (i = 0; i < TILE_PACKED; i++) {
        value = undo_tile[i];
        tile[i << 1] = value >> 4;
        tile[(i << 1) + 1] = value & 15;
    }
}

static void live_pixel(unsigned char x, unsigned char y)
{
    unsigned char *p = &tile[(unsigned int)y * TILE_SIDE + x];
    if (*p == current_pen) return;
    *p = current_pen;
    draw_work_run(x, x + 1, y);
    draw_preview_pixel(x, y);
}

static void set_tile_pixel(signed char x, signed char y)
{
    if (x >= 0 && x < TILE_SIDE && y >= 0 && y < TILE_SIDE)
        tile[(unsigned int)y * TILE_SIDE + (unsigned char)x] = current_pen;
}

static void tile_line(signed char x0, signed char y0,
                      signed char x1, signed char y1,
                      unsigned char live)
{
    signed char dx = x1 > x0 ? x1 - x0 : x0 - x1;
    signed char sx = x0 < x1 ? 1 : -1;
    signed char dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    signed char sy = y0 < y1 ? 1 : -1;
    signed char err = dx + dy;
    for (;;) {
        if (live) live_pixel(x0, y0);
        else set_tile_pixel(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        {
            signed char twice = (signed char)(err << 1);
            if (twice >= dy) { err += dy; x0 += sx; }
            if (twice <= dx) { err += dx; y0 += sy; }
        }
    }
}

static void tile_rect(signed char x0, signed char y0,
                      signed char x1, signed char y1,
                      unsigned char filled)
{
    signed char x, y, value;
    if (x0 > x1) { value = x0; x0 = x1; x1 = value; }
    if (y0 > y1) { value = y0; y0 = y1; y1 = value; }
    if (filled) {
        for (y = y0; y <= y1; y++)
            for (x = x0; x <= x1; x++) set_tile_pixel(x, y);
    } else {
        for (x = x0; x <= x1; x++) {
            set_tile_pixel(x, y0);
            set_tile_pixel(x, y1);
        }
        for (y = y0; y <= y1; y++) {
            set_tile_pixel(x0, y);
            set_tile_pixel(x1, y);
        }
    }
}

static unsigned char small_sqrt(unsigned int value)
{
    unsigned char root = 0;
    while ((unsigned int)(root + 1) * (root + 1) <= value) root++;
    return root;
}

static void tile_span(signed char y, signed char x0, signed char x1)
{
    while (x0 <= x1) set_tile_pixel(x0++, y);
}

static void tile_circle(signed char cx, signed char cy,
                        unsigned char radius, unsigned char filled)
{
    signed char x = radius, y = 0;
    signed char err = (signed char)(1 - radius);
    if (!radius) { set_tile_pixel(cx, cy); return; }
    while (x >= y) {
        if (filled) {
            tile_span((signed char)(cy + y), (signed char)(cx - x),
                      (signed char)(cx + x));
            tile_span((signed char)(cy - y), (signed char)(cx - x),
                      (signed char)(cx + x));
            tile_span((signed char)(cy + x), (signed char)(cx - y),
                      (signed char)(cx + y));
            tile_span((signed char)(cy - x), (signed char)(cx - y),
                      (signed char)(cx + y));
        } else {
            set_tile_pixel(cx + x, cy + y);
            set_tile_pixel(cx - x, cy + y);
            set_tile_pixel(cx + x, cy - y);
            set_tile_pixel(cx - x, cy - y);
            set_tile_pixel(cx + y, cy + x);
            set_tile_pixel(cx - y, cy + x);
            set_tile_pixel(cx + y, cy - x);
            set_tile_pixel(cx - y, cy - x);
        }
        y++;
        if (err < 0) err = (signed char)(err + 2 * y + 1);
        else {
            x--;
            err = (signed char)(err + 2 * (y - x) + 1);
        }
    }
}

/* The target pen and frontier (0x80) are distinct from every replacement
 * pen. Scan bounded tile pointers; no recursion or shared stack workspace.
 * IX displacements keep neighbour addresses independent of SDCC spills. */
static unsigned char fill_rows, fill_again;
static void flood_fill(unsigned char x, unsigned char y) __naked
{
    x; y;
__asm
    push ix
    ld e,l
    ld d,#0
    ld h,d
    add hl,hl
    add hl,hl
    add hl,de
    add hl,hl
    add hl,hl
    ld e,a
    add hl,de
    ld de,#_tile
    add hl,de
    ld a,(_current_pen)
    cp (hl)
    jp z,ff_done
    ld b,(hl)                  ; target ink
    ld (hl),#0x80              ; caller checked the seed is inside the picture
    ld a,(_tile_w)
    ld c,a
ff_sweep:
    ld ix,#_tile
    xor a
    ld (_fill_again),a
    ld a,(_tile_h)
    ld (_fill_rows),a
ff_row:
    ld e,c
ff_pixel:
    bit 7,0(ix)
    jr z,ff_next
    ld a,(_current_pen)
    ld 0(ix),a
    ld a,e
    cp c
    jr z,ff_right
    ld a,-1(ix)
    cp b
    jr nz,ff_right
    ld -1(ix),#0x80
    ld a,#1
    ld (_fill_again),a
ff_right:
    ld a,e
    dec a
    jr z,ff_up
    ld a,1(ix)
    cp b
    jr nz,ff_up
    ld 1(ix),#0x80
ff_up:
    ld a,(_tile_h)
    ld d,a
    ld a,(_fill_rows)
    cp d
    jr z,ff_down
    ld a,-20(ix)
    cp b
    jr nz,ff_down
    ld -20(ix),#0x80
    ld a,#1
    ld (_fill_again),a
ff_down:
    ld a,(_fill_rows)
    dec a
    jr z,ff_next
    ld a,20(ix)
    cp b
    jr nz,ff_next
    ld 20(ix),#0x80
ff_next:
    inc ix
    dec e
    jr nz,ff_pixel
    ld a,#20
    sub c
    ld e,a
    ld d,#0
    add ix,de
    ld hl,#_fill_rows
    dec (hl)
    jr nz,ff_row
    ld a,(_fill_again)
    or a
    jr nz,ff_sweep
ff_done:
    pop ix
    ret
__endasm;
}

static unsigned char next_random(void)
{
    random_state = (unsigned char)((random_state >> 1) ^
        ((random_state & 1) ? 0xB8 : 0));
    return random_state;
}

static void spray_at(unsigned char x, unsigned char y)
{
    unsigned char i;
    for (i = 0; i < 3; i++) {
        signed char dx = (signed char)(next_random() % 5) - 2;
        signed char dy = (signed char)(next_random() % 5) - 2;
        int px = (int)x + dx;
        int py = (int)y + dy;
        if (px >= 0 && px < TILE_SIDE && py >= 0 && py < TILE_SIDE) {
            live_pixel(px, py);
        }
    }
}

static void refresh_tool_title(void)
{
    gb_curhide();
    gb_window(tc_x, tc_y, TC_W, TITLE_H, tool_title());
    gb_curshow();
}

static void finish_change(void)
{
    unsigned char was_dirty = dirty;
    if (!commit_tile()) gb_alert("Paint error", "Could not write tile");
    if (!stroke_active) update_changes(1);
    if (dirty != was_dirty) refresh_tool_title();
}

static void do_undo(void)
{
    unsigned int i;
    unsigned char value, replacement;
    if (!work_visible || !undo_valid) return;
    remember_pixels();
    for (i = 0; i < TILE_PACKED; i++) {
        value = undo_tile[i];
        replacement = (unsigned char)((tile[i << 1] << 4) |
                                      tile[(i << 1) + 1]);
        tile[i << 1] = value >> 4;
        tile[(i << 1) + 1] = value & 15;
        undo_tile[i] = replacement;
    }
    finish_change();
}

static void copy_selection(void)
{
    unsigned char x, y, clip_w, clip_h;
    unsigned char *clip = (unsigned char *)gb_copybuf;
    if (!work_visible) return;
    if (!work_sel_on) {
        work_sel_x0 = work_sel_y0 = 0;
        work_sel_x1 = work_sel_y1 = TILE_SIDE - 1;
    }
    normalize_work_selection();
    clip_w = (unsigned char)(work_sel_x1 - work_sel_x0 + 1);
    clip_h = (unsigned char)(work_sel_y1 - work_sel_y0 + 1);
    clip[0] = PAINT_CLIP_MAGIC;
    clip[1] = clip_w;
    clip[2] = clip_h;
    for (y = 0; y < clip_h; y++)
        for (x = 0; x < clip_w; x++)
            clip[3U + (unsigned int)y * TILE_SIDE + x] =
                tile[(unsigned int)(work_sel_y0 + y) * TILE_SIDE +
                     work_sel_x0 + x];
    gb_clip_set((char *)clip, PAINT_CLIP_MAX);
}

static void cut_selection(void)
{
    unsigned char x, y;
    if (!work_visible) return;
    copy_selection();
    save_undo();
    for (y = work_sel_y0; y <= work_sel_y1; y++)
        for (x = work_sel_x0; x <= work_sel_x1; x++)
            tile[(unsigned int)y * TILE_SIDE + x] = 1;
    finish_change();
}

static void paste_selection(void)
{
    unsigned int length;
    unsigned char x, y, clip_w, clip_h, ox = 0, oy = 0;
    unsigned char *clip = (unsigned char *)gb_copybuf;
    if (!work_visible) return;
    length = gb_clip_get((char *)clip, PAINT_CLIP_MAX);
    if (length != PAINT_CLIP_MAX || clip[0] != PAINT_CLIP_MAGIC) return;
    clip_w = clip[1];
    clip_h = clip[2];
    if (!clip_w || clip_w > TILE_SIDE ||
        !clip_h || clip_h > TILE_SIDE) return;
    if (work_sel_on) { ox = work_sel_x0; oy = work_sel_y0; }
    if (clip_w > (unsigned char)(TILE_SIDE - ox))
        clip_w = (unsigned char)(TILE_SIDE - ox);
    if (clip_h > (unsigned char)(TILE_SIDE - oy))
        clip_h = (unsigned char)(TILE_SIDE - oy);
    save_undo();
    for (y = 0; y < clip_h; y++)
        for (x = 0; x < clip_w; x++)
            tile[(unsigned int)(oy + y) * TILE_SIDE + ox + x] =
                clip[3U + (unsigned int)y * TILE_SIDE + x];
    finish_change();
}

static unsigned char work_point(unsigned char *x, unsigned char *y,
                                unsigned char clamp)
{
    int px = (int)gb_mxp() - (int)(wk_x + 1) * 4;
    int py = (int)gb_my() - (int)(wk_y + TITLE_H);
    if (!clamp &&
        (px < 0 || py < 0 || px >= WORK_PIXELS || py >= WORK_PIXELS))
        return 0;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= WORK_PIXELS) px = WORK_PIXELS - 1;
    if (py >= WORK_PIXELS) py = WORK_PIXELS - 1;
    *x = (unsigned char)(px / WORK_SCALE);
    *y = (unsigned char)(py / WORK_SCALE);
    return 1;
}

static void apply_shape(unsigned char sx, unsigned char sy,
                        unsigned char ex, unsigned char ey)
{
    if (current_tool == TOOL_LINE) tile_line(sx, sy, ex, ey, 0);
    else if (current_tool == TOOL_RECT) tile_rect(sx, sy, ex, ey, 0);
    else if (current_tool == TOOL_RECTF) tile_rect(sx, sy, ex, ey, 1);
    else {
        signed char dx = (signed char)ex - (signed char)sx;
        signed char dy = (signed char)ey - (signed char)sy;
        tile_circle(sx, sy,
            small_sqrt((unsigned int)(dx * dx + dy * dy)),
            (unsigned char)(current_tool == TOOL_CIRCLEF));
    }
}

static void drag_shape(unsigned char sx, unsigned char sy)
{
    unsigned char ex = sx, ey = sy, nx, ny;
    unsigned char flags;
    save_undo();
    for (;;) {
        flags = gb_poll();
        if (!(flags & GB_FIRE)) break;
        work_point(&nx, &ny, 1);
        if (nx == ex && ny == ey) continue;
        ex = nx; ey = ny;
        remember_pixels();
        restore_undo_base();
        apply_shape(sx, sy, ex, ey);
        update_changes(1);
    }
    remember_pixels();
    restore_undo_base();
    apply_shape(sx, sy, ex, ey);
    finish_change();
}

/* Invalidate only the cells along the old and new selection outlines. */
static void mark_selection(void)
{
    unsigned char x, y;
    if (!work_sel_on) return;
    for (y = work_sel_y0; y <= work_sel_y1; ++y)
        for (x = work_sel_x0; x <= work_sel_x1; ++x)
            if (x == work_sel_x0 || x == work_sel_x1 ||
                y == work_sel_y0 || y == work_sel_y1)
                BEFORE[(unsigned int)y * TILE_SIDE + x] = 0xff;
}

static void drag_work_selection(unsigned char sx, unsigned char sy)
{
    unsigned char nx = sx, ny = sy, x, y;
    do {
        remember_pixels();
        mark_selection();
        work_sel_on = 1;
        work_sel_x0 = sx; work_sel_y0 = sy;
        work_sel_x1 = nx; work_sel_y1 = ny;
        normalize_work_selection();
        mark_selection();
        update_changes(0);
        do {
            if (!(gb_poll() & GB_FIRE)) return;
            work_point(&x, &y, 1);
        } while (x == nx && y == ny);
        nx = x; ny = y;
    } while (1);
}

static void start_work_action(void)
{
    unsigned char x, y;
    if (!work_point(&x, &y, 0)) return;
    if (current_tool == TOOL_SELECT) {
        drag_work_selection(x, y);
        return;
    }
    if (current_tool == TOOL_BUCKET) {
        if (x >= tile_w || y >= tile_h ||
            tile[(unsigned int)y * TILE_SIDE + x] == current_pen) return;
        save_undo();
        flood_fill(x, y);
        finish_change();
        return;
    }
    if (current_tool == TOOL_LINE || current_tool == TOOL_RECT ||
        current_tool == TOOL_RECTF || current_tool == TOOL_CIRCLE ||
        current_tool == TOOL_CIRCLEF) {
        drag_shape(x, y);
        return;
    }
    if (current_tool == TOOL_PENCIL || current_tool == TOOL_SPRAY) {
        save_undo();
        stroke_active = 1;
        stroke_x = x; stroke_y = y;
        gb_curhide();
        if (current_tool == TOOL_PENCIL) {
            live_pixel(x, y);
        } else spray_at(x, y);
        gb_curshow();
    }
}

static void continue_stroke(void)
{
    unsigned char x, y;
    if (!(gb_flags() & GB_FIRE)) {
        finish_change();
        stroke_active = 0;

        return;
    }
    work_point(&x, &y, 1);
    gb_curhide();
    if (current_tool == TOOL_PENCIL) {
        if (x != stroke_x || y != stroke_y) {
            tile_line(stroke_x, stroke_y, x, y, 1);
            stroke_x = x; stroke_y = y;
        }
    } else {
        spray_at(x, y);
        stroke_x = x; stroke_y = y;
    }
    gb_curshow();
}

/* ---- preview interaction ------------------------------------------------- */

/* Only repaint an overlying pane if a newly drawn window actually covers it. */
static void preview_front(void)
{
    draw_preview();
    if (overlap(pv_x,pv_y,PV_W,PV_H,tc_x,tc_y,TC_W,TC_H)) draw_toolchest();
}

static void canvas_front(void)
{
    draw_work();
    if (overlap(wk_x,wk_y,WK_W,WK_H,tc_x,tc_y,TC_W,TC_H)) draw_toolchest();
}

static void focus_pane(unsigned char pane)
{
    if (pane == front_pane) return;
    front_pane = pane;
    if (!work_visible || !overlap(wk_x,wk_y,WK_W,WK_H,pv_x,pv_y,PV_W,PV_H)) return;
    gb_curhide();
    if (pane == PANE_WORK) canvas_front(); else preview_front();
    gb_curshow();
}

static unsigned int clamp_origin(int pos, unsigned int size)
{
    unsigned int limit = size > TILE_SIDE ? size - TILE_SIDE : 0;
    if (pos < 0) return 0;
    return (unsigned int)pos > limit ? limit : (unsigned int)pos;
}

static void drag_selector(void)
{
    int dx = (int)gb_mxp() - (int)sel_px;
    int dy = (int)gb_my() - sel_py;
    unsigned int nx, ny;
    unsigned char moved = 0;
    if (dx < 0 || dx >= TILE_SIDE || dy < 0 || dy >= TILE_SIDE)
        dx = dy = TILE_SIDE / 2;
    dx += (int)pv_image_x * 4 - (int)scroll_x * 4;
    dy += (int)pv_image_y - (int)scroll_y;
    do {
        nx = clamp_origin((int)gb_mxp() - dx, pic_width);
        ny = clamp_origin((int)gb_my() - dy, pic_height);
        if (nx != tile_x || ny != tile_y) {
            tile_x = nx; tile_y = ny;
            moved = 1;
            gb_curhide(); preview_front(); gb_curshow();
        }
    } while (gb_poll() & GB_FIRE);
    if ((!work_visible || moved) && !load_tile()) {
        gb_alert("Paint error", "Could not read tile");
        return;
    }
    if (!work_visible) {
        front_pane = PANE_WORK;
        work_visible = 1;
        gb_curhide(); canvas_front(); gb_curshow();
    } else if (moved) {
        gb_curhide(); canvas_pixels(); gb_curshow();
    }
}

static void drag_scroll(unsigned char horizontal)
{
    unsigned int value;
    unsigned char moved = 0;
    do {
        if (horizontal) {
            value = scroll_value(pv_hbar_x, pv_hbar_w, pic_wb, pv_view_w, gb_mx());
            if (value == scroll_x) continue;
            tile_x = clamp_origin((int)tile_x + ((int)value - (int)scroll_x) * 4, pic_width);
            scroll_x = value;
        } else {
            value = scroll_value(pv_view_y, pv_view_h, pic_height, pv_view_h, gb_my());
            if (value == scroll_y) continue;
            tile_y = clamp_origin((int)tile_y + ((int)value - (int)scroll_y), pic_height);
            scroll_y = value;
        }
        moved = 1;
        gb_curhide(); preview_front(); gb_curshow();
    } while (gb_poll() & GB_FIRE);
    if (moved && work_visible && load_tile()) {
        gb_curhide(); canvas_pixels(); gb_curshow();
    }
}

/* ---- files --------------------------------------------------------------- */

static const char *const pic_exts[] = { "PIC", 0 };
static char prompt_name[16];

static unsigned char parse_header(const unsigned char *header,
                                  unsigned int got) __naked
{
    (void)header; (void)got;
__asm
    ; HL=header, DE=bytes read. Preserve no caller registers; return A.
    ld   a,d
    or   a
    jr   nz,ph_got6
    ld   a,e
    cp   #6
    jp   c,ph_bad
ph_got6:
    ld   a,(hl)
    cp   #'G'
    jp   nz,ph_bad
    inc  hl
    ld   a,(hl)
    cp   #'B'
    jp   nz,ph_bad
    inc  hl
    ld   a,(hl)
    cp   #'P'
    jp   nz,ph_bad
    inc  hl
    ld   a,(hl)
    cp   #'C'
    jp   nz,ph_bad
    inc  hl                         ; header byte 4
    ld   a,(hl)
    cp   #2
    jr   nz,ph_v1

    ld   a,d
    or   a
    jr   nz,ph_v2_size
    ld   a,e
    cp   #14
    jp   c,ph_bad
ph_v2_size:
    inc  hl                         ; mode
    ld   a,(hl)
    cp   #1
    jr   z,ph_v2_mode
    cp   #7
    jp   nz,ph_bad
__endasm;
#ifndef GB_MSX2
__asm
    xor  a
    ret
__endasm;
#endif
__asm
ph_v2_mode:
    ld   (_pic_mode),a
    inc  hl                         ; width low
    ld   c,(hl)
    inc  hl
    ld   b,(hl)
    inc  hl                         ; height low
    ld   a,b
    or   c
    jp   z,ph_bad
    ld   a,b                        ; width <= 512
    cp   #2
    jr   c,ph_width_ok
    jp   nz,ph_bad
    ld   a,c
    or   a
    jp   nz,ph_bad
ph_width_ok:
    ld   (_pic_width),bc
    push hl
    ld   h,b
    ld   l,c
    inc  hl
    inc  hl
    inc  hl
    srl  h
    rr   l
    srl  h
    rr   l
    ld   a,l
    ld   (_pic_wb),a
    ld   a,(_pic_mode)
    cp   #7
    jr   nz,ph_stride_wb
    ld   h,b
    ld   l,c
    inc  hl
    srl  h
    rr   l
    jr   ph_stride_ready
ph_stride_wb:
    ld   a,(_pic_wb)
    ld   l,a
    ld   h,#0
ph_stride_ready:
    ld   (_pic_stride),hl
    pop  hl                         ; height low
    ld   e,(hl)
    inc  hl
    ld   d,(hl)
    ld   a,d
    or   e
    jp   z,ph_bad
    ld   (_pic_height),de
    ld   a,#14
    jr   ph_geometry

ph_v1:
    or   a
    jp   z,ph_bad
    ld   (_pic_wb),a
    ld   l,a
    ld   h,#0
    ld   (_pic_stride),hl
    add  hl,hl
    add  hl,hl
    ld   (_pic_width),hl
    ld   a,#1
    ld   (_pic_mode),a
    ; The original header pointer is gb_copybuf for every bounded load.
    ld   hl,#0x2205
    ld   e,(hl)
    ld   d,#0
    ld   a,e
    or   a
    jp   z,ph_bad
    ld   (_pic_height),de
    ld   a,#6

ph_geometry:
    ld   (_pic_off),a
    ld   c,a
    ld   b,#0
    push bc                         ; preserve bitmap offset
    ld   hl,#0x4000
    or   a
    sbc  hl,bc
    ld   de,(_pic_stride)
    call __divuint                  ; DE = maximum legal height
    ld   hl,(_pic_height)
    or   a
    sbc  hl,de
    jr   c,ph_fits
    jr   z,ph_fits
    pop  bc
    xor  a
    ret
ph_fits:
    ld   hl,(_pic_stride)
    ld   de,(_pic_height)
    call __mulint                   ; DE = bitmap byte length
    pop  bc
    ex   de,hl
    add  hl,bc
    ld   (_doc_len),hl
    ld   a,#1
    ret
ph_bad:
    xor  a
    ret
__endasm;
}

static unsigned char begin_job(unsigned char job)
{
    if (gb_drop_claimed()) {
        gb_alert("Storage busy", "Try again shortly");
        return 0;
    }
    gb_drop_claim();
    io_job = job;
    io_after = AFTER_NONE;
    io_first = 1;
    io_off = 0;
    return 1;
}

static void stop_job(unsigned char discard_output)
{
    unsigned char job = io_job;
    io_job = IO_IDLE;
    FS_XFLAGS_K = 0;
    if (job == IO_SAVE && discard_output && !io_first) {
        gb_set_name(cur_name);
        gb_file_delete(cur_name);
        UI_MODAL_K = 0;
    }
    gb_drop_release();
    if (job == IO_LOAD || job == IO_NEW) close_document();
}

static void job_error(const char *title, const char *message)
{
    stop_job(1);
    gb_alert(title, message);
    gb_restore_parent();
}

static void io_error(void)
{
    job_error("Paint I/O failed", "Check file or disk");
}

static unsigned int read_chunk(unsigned int off, unsigned int max)
{
    unsigned int got;
    gb_set_name(cur_name);
    FS_LOAD_OFS[0] = (unsigned char)off;
    FS_LOAD_OFS[1] = (unsigned char)(off >> 8);
    FS_LOAD_OFS[2] = 0;
    FS_XFLAGS_K = 1;
    got = gb_fs_load(gb_copybuf, max);
    FS_XFLAGS_K = 0;
    UI_MODAL_K = 0;
    return got;
}

static void retire_panes(void)
{
    unsigned char exposed = loaded;
    close_document();
    if (exposed) gb_restore_parent();
}

static unsigned char start_load(const char *name)
{
    if (!begin_job(IO_LOAD)) return 0;
    retire_panes();
    copy11(cur_name, name);
    named = 1;
    doc_len = DOC_MAX;
    if (!allocate_document_page()) {
        stop_job(0);
        gb_alert("Picture not loaded", "No free picture bank");
        return 0;
    }
    return 1;
}

static unsigned char start_new(unsigned int width, unsigned int height)
{
    unsigned char *header = (unsigned char *)gb_copybuf;
    if (!begin_job(IO_NEW)) return 0;
    close_document();
    gb_restore_parent();           /* Remove the size prompt (no save-under) and any old panes. */
#ifdef GB_MSX2
    pic_mode = 7;
    pic_stride = width >> 1;
#else
    pic_mode = 1;
    pic_stride = (width + 3U) >> 2;
#endif
    pic_width = width;
    pic_height = height;
    pic_wb = (unsigned char)((width + 3U) >> 2);
    pic_off = PIC_HDR;
    doc_len = PIC_HDR + pic_stride * height;
    if (!allocate_document_page()) {
        stop_job(0);
        gb_alert("New picture failed", "No free picture bank");
        return 0;
    }
    header[0] = 'G'; header[1] = 'B'; header[2] = 'P'; header[3] = 'C';
    header[4] = 2; header[5] = pic_mode;
    header[6] = (unsigned char)width;
    header[7] = (unsigned char)(width >> 8);
    header[8] = (unsigned char)height;
    header[9] = (unsigned char)(height >> 8);
    header[10] = 1; header[11] = 26; header[12] = 0; header[13] = 6;
    if (!document_write(0, header, PIC_HDR)) {
        io_error();
        return 0;
    }
    io_off = PIC_HDR;
    return 1;
}

static unsigned char start_save(unsigned char after)
{
    if (!loaded || !named || !begin_job(IO_SAVE)) return 0;
    io_after = after;
    return 1;
}

static unsigned char save_as(unsigned char after)
{
    char raw[11];
    if (!loaded) return 0;
    if (!gb_pickdir(pic_exts)) return 0;
    if (!gb_prompt("Save picture as:", prompt_name, 12)) return 0;
    to_83(prompt_name, raw);
    copy11(cur_name, raw);
    named = 1;
    gb_set_name(cur_name);
    return start_save(after);
}

static unsigned char save_document(unsigned char after)
{
    if (!loaded) return 0;
    if (!named) return save_as(after);
    return start_save(after);
}

static const char *const confirm_items[] = {
    "Save", "Don't Save", "Cancel"
};

static unsigned char confirm_discard(unsigned char after)
{
    unsigned char choice;
    if (!dirty) return CONFIRM_NOW;
    choice = gb_popup(28, 80, confirm_items, 3);
    if (choice == 0)
        return save_document(after) ? CONFIRM_WAIT : CONFIRM_CANCEL;
    if (choice == 1) return CONFIRM_NOW;
    return CONFIRM_CANCEL;
}

static void do_new(void)
{
    unsigned int width = 100, height = 100, stride;
    unsigned char action;
    if (!gb_size_prompt(&width, &height)) return;
    if (!width || width > 512 || !height) {
        gb_alert("Invalid dimensions", "Width 1..512");
        return;
    }
#ifdef GB_MSX2
    if ((width & 3) || height > 255) {
        gb_alert("Invalid Mode 7 size", "Width /4, height <=255");
        return;
    }
    stride = width >> 1;
#else
    stride = (width + 3U) >> 2;
#endif
    if (!stride || height > (unsigned int)((DOC_MAX - PIC_HDR) / stride)) {
        gb_alert("Picture too large", "16K maximum");
        return;
    }
    pending_width = width;
    pending_height = height;
    action = confirm_discard(AFTER_NEW);
    if (action == CONFIRM_NOW) start_new(width, height);
}

static void do_load(void)
{
    char name[11];
    unsigned char action = confirm_discard(AFTER_LOAD);
    if (action != CONFIRM_NOW) return;
    if (!gb_pickfile(name, pic_exts)) return;
    start_load(name);
}

/* ---- menus and window interaction --------------------------------------- */

#define MENU_FILE_X 10
#define MENU_EDIT_X 18
static const unsigned char menu_def[] = {
    2,
    MENU_FILE_X, 'F','i','l','e',0,0,0,0,
    MENU_EDIT_X, 'E','d','i','t',0,0,0,0
};
static const char *const file_items[] = {
    "New", "Load", "Save", "Save As", "Quit"
};
static const char *const edit_items[] = {
    "Undo", "Cut", "Copy", "Paste"
};

static unsigned char menu_title_hit(unsigned char col, unsigned char start)
{
    return (unsigned char)(col >= start && col < (unsigned char)(start + 7));
}

static void run_menu(void)
{
    unsigned char menu = want_menu;
    unsigned char choice;
    want_menu = 0;
    if (menu == 1) {
        choice = gb_popup(MENU_FILE_X, 8, file_items, 5);
        if (choice == 0) do_new();
        else if (choice == 1) do_load();
        else if (choice == 2) save_document(AFTER_NONE);
        else if (choice == 3) save_as(AFTER_NONE);
        else if (choice == 4) { close_app(); return; }
    } else if (menu == 2) {
        choice = gb_popup(MENU_EDIT_X, 8, edit_items, 4);
        if (choice == 0) do_undo();
        else if (choice == 1) cut_selection();
        else if (choice == 2) copy_selection();
        else if (choice == 3) paste_selection();
    }
    if (io_job == IO_IDLE) gb_restore_parent();
}

static void move_pane(unsigned char *x, unsigned char *y,
                      unsigned char w, unsigned char h) __naked
{
    (void)x; (void)y; (void)w; (void)h;
__asm
    ; Transient state lives in the consumed executable-icon payload. The app
    ; data area is full on dual-icon MSX builds, and these bytes are dead after
    ; entry just like the bounded-I/O state at 0x4010.
    ld   (0x4019),hl                  ; x pointer
    ld   (0x401b),de                  ; y pointer
    ld   a,(hl)
    ld   (0x401d),a                   ; original/last x
    ld   (0x401f),a
    ld   a,(de)
    ld   (0x401e),a                   ; original/last y
    ld   (0x4020),a
    ld   hl,#2
    add  hl,sp
    ld   a,(hl)
    ld   (0x4023),a                   ; pane width
    inc  hl
    ld   a,(hl)
    ld   (0x4024),a                   ; pane height
    call _gb_mx
    ld   b,a
    ld   a,(0x401d)
    ld   c,a
    ld   a,b
    sub  c
    ld   (0x4025),a                   ; pointer grab x
    call _gb_my
    ld   b,a
    ld   a,(0x401e)
    ld   c,a
    ld   a,b
    sub  c
    ld   (0x4026),a                   ; pointer grab y
    xor  a
    ld   (0x4027),a                   ; moved

mp_loop:
    call _gb_poll
    ld   (0x4028),a                   ; flags

    call _gb_mx
    ld   c,a
    ld   a,(0x4025)
    ld   b,a
    ld   a,c
    sub  b
    jr   nc,mp_x_nonneg
    xor  a
mp_x_nonneg:
    ld   b,a                          ; candidate x
    ld   a,#GB_COLS
    ld   c,a
    ld   a,(0x4023)
    ld   e,a
    ld   a,c
    sub  e                            ; maximum x
    cp   b
    jr   nc,mp_x_ok
    ld   b,a
mp_x_ok:
    ld   a,b
    ld   (0x4021),a                   ; next x

    call _gb_my
    ld   c,a
    ld   a,(0x4026)
    ld   b,a
    ld   a,c
    sub  b
    jr   nc,mp_y_nonneg
    ld   a,#8
mp_y_nonneg:
    cp   #8
    jr   nc,mp_y_top
    ld   a,#8
mp_y_top:
    ld   b,a                          ; candidate y
    ld   a,#GB_LINES
    ld   c,a
    ld   a,(0x4024)
    ld   e,a
    ld   a,c
    sub  e                            ; maximum y
    cp   b
    jr   nc,mp_y_ok
    ld   b,a
mp_y_ok:
    ld   a,b
    ld   (0x4022),a                   ; next y

    ld   a,(0x401f)
    ld   b,a
    ld   a,(0x4021)
    cp   b
    jr   nz,mp_changed
    ld   a,(0x4020)
    ld   b,a
    ld   a,(0x4022)
    cp   b
    jr   z,mp_next
mp_changed:
    ld   a,(0x4027)
    or   a
    jr   z,mp_draw
    ld   a,(0x401f)
    ld   (0x4029),a
    ld   a,(0x4020)
    ld   (0x402a),a
    ld   a,(0x4021)
    ld   (0x402b),a
    ld   a,(0x4022)
    ld   (0x402c),a
    ld   a,#TITLE_H
    ld   (0x402d),a
    call mp_damage
    call _gb_restore_parent

mp_draw:
    call _gb_curhide
    ld   a,(0x4021)
    ld   b,a
    ld   a,(0x4022)
    ld   c,a
    ld   a,(0x4023)
    ld   d,a
    ld   e,#TITLE_H
    ld   a,#3
    call 0x8021                       ; GB_FRAME
    call _gb_curshow
    ld   a,(0x4021)
    ld   (0x401f),a
    ld   a,(0x4022)
    ld   (0x4020),a
    ld   a,#1
    ld   (0x4027),a

mp_next:
    ld   a,(0x4028)
    and  #GB_FIRE
    jp   nz,mp_loop
    ld   a,(0x4027)
    or   a
    jr   z,mp_return

    ld   hl,(0x4019)
    ld   a,(0x401f)
    ld   (hl),a
    ld   hl,(0x401b)
    ld   a,(0x4020)
    ld   (hl),a
    ld   a,(0x401d)
    ld   (0x4029),a
    ld   a,(0x401e)
    ld   (0x402a),a
    ld   a,(0x401f)
    ld   (0x402b),a
    ld   a,(0x4020)
    ld   (0x402c),a
    ld   a,(0x4024)
    ld   (0x402d),a
    call mp_damage
    call _gb_restore_parent

mp_return:
    pop  hl                           ; discard packed w/h after return address
    pop  af
    jp   (hl)

    ; Damage = bounding rectangle of (ax,ay) and (bx,by), using the pane width
    ; and caller-selected height. k_wm_damage leaves the next compositor repaint
    ; clipped to this rectangle.
mp_damage:
    ld   a,(0x4029)                   ; ax
    ld   c,a
    ld   a,(0x402b)                   ; bx
    cp   c
    jr   nc,mp_dx_ordered
    ld   c,a                          ; left=bx, right starts at ax
    ld   a,(0x4029)
mp_dx_ordered:
    ld   hl,#0x4023                   ; pane width
    add  a,(hl)
    sub  c
    ld   e,a
    ld   a,(0x402a)                   ; ay
    ld   b,a
    ld   a,(0x402c)                   ; by
    cp   b
    jr   nc,mp_dy_ordered
    ld   b,a                          ; top=by, bottom starts at ay
    ld   a,(0x402a)
mp_dy_ordered:
    ld   hl,#0x402d                   ; damage height
    add  a,(hl)
    sub  b
    ld   d,a
    call 0x80b4                       ; GB_WMDAMAGE: C=x B=y E=w D=h
    ret
__endasm;
}

static void choose_pen(unsigned char pen)
{
#ifdef GB_MSX2
    if (pen > 3 && loaded && pic_mode != 7) {
        gb_alert("Four-color picture", "Pens 0-3 only");
        return;
    }
#endif
    if (current_pen == pen) return;
    gb_curhide();
    /* The selection frame extends one scanline beyond the swatch at each end. */
    gb_fill(tc_x + 1, tc_y + TITLE_H + TOOL_ROWS * TOOL_STEP_Y, 24,
#ifdef GB_MSX2
            19,
#else
            11,
#endif
            0);
    current_pen = pen;
    draw_palette();
    gb_curshow();
}

static void tool_action(unsigned char index)
{
    if (index == TOOL_UNDO) do_undo();
    else if (index == TOOL_CUT) cut_selection();
    else if (index == TOOL_COPY) copy_selection();
    else if (index == TOOL_PASTE) paste_selection();
    else {
        if (index == current_tool) return;
        gb_curhide();
        gb_frame(tool_x(current_tool), tool_y(current_tool), TOOL_WB, TOOL_H, 2);
        current_tool = index;
        gb_frame(tool_x(index), tool_y(index), TOOL_WB, TOOL_H, 3);
        gb_curshow();
    }
}

static void toolchest_click(unsigned char mx, unsigned char my)
{
    unsigned char i;
    unsigned char py = (unsigned char)(tc_y + TITLE_H +
                                       TOOL_ROWS * TOOL_STEP_Y + 1);
    if (title_hit(tc_x, tc_y, TC_W, mx, my)) {
        if (close_hit(tc_x, tc_y, mx, my)) close_app();
        else move_pane(&tc_x, &tc_y, TC_W, TC_H);
        return;
    }
    for (i = 0; i < N_TOOLS; i++) {
        if (inside(tool_x(i), tool_y(i), TOOL_WB, TOOL_H, mx, my)) {
            tool_action(i);
            return;
        }
    }
#ifdef GB_MSX2
    if (my >= py && my < (unsigned char)(py + 18) &&
        mx >= (unsigned char)(tc_x + 1) &&
        mx < (unsigned char)(tc_x + 25)) {
        i = (unsigned char)((my - py >= 9 ? 8 : 0) +
                            (unsigned char)(mx - tc_x - 1) / 3U);
        if (i < 16) choose_pen(i);
    }
#else
    if (my >= py && my < (unsigned char)(py + 9) &&
        mx >= (unsigned char)(tc_x + 1) &&
        mx < (unsigned char)(tc_x + 25)) {
        i = (unsigned char)((mx - tc_x - 1) / 6);
        if (i < 4) choose_pen(i);
    }
#endif
}

static void preview_click(unsigned char mx, unsigned char my)
{
    focus_pane(PANE_PREVIEW);
    if (title_hit(pv_x, pv_y, PV_W, mx, my)) {
        if (close_hit(pv_x, pv_y, mx, my)) {
            unsigned char action = confirm_discard(AFTER_CLOSE_DOC);
            if (action == CONFIRM_NOW) {
                close_document();
                gb_restore_parent();
            }
        } else move_pane(&pv_x, &pv_y, PV_W, PV_H);
        return;
    }
    layout_preview();
    if (inside((unsigned char)(pv_x + 1), pv_view_y, SB_W, pv_view_h,
               mx, my)) {
        drag_scroll(0);
        return;
    }
    if (inside(pv_hbar_x, pv_hbar_y, pv_hbar_w, HSB_H, mx, my)) {
        drag_scroll(1);
        return;
    }
    if (my >= pv_image_y && my < (unsigned char)(pv_image_y + pv_view_h) &&
        gb_mxp() >= (unsigned int)pv_image_x * 4U &&
        gb_mxp() < (unsigned int)(pv_image_x + pv_view_w) * 4U)
        drag_selector();
}

static void work_click(unsigned char mx, unsigned char my)
{
    focus_pane(PANE_WORK);
    if (title_hit(wk_x, wk_y, WK_W, mx, my)) {
        if (close_hit(wk_x, wk_y, mx, my)) {
            work_visible = 0;
            gb_restore_parent();
        } else move_pane(&wk_x, &wk_y, WK_W, (unsigned char)WK_H);
        return;
    }
    start_work_action();
}

static void handle_click(void)
{
    unsigned char mx = gb_mx(), my = gb_my();
    if (inside(tc_x, tc_y, TC_W, TC_H, mx, my)) {
        toolchest_click(mx, my);
        return;
    }
    if (!loaded) return;
    if (work_visible && front_pane == PANE_WORK &&
        inside(wk_x, wk_y, WK_W, (unsigned char)WK_H, mx, my)) {
        work_click(mx, my);
        return;
    }
    if (inside(pv_x, pv_y, PV_W, PV_H, mx, my)) {
        preview_click(mx, my);
        return;
    }
    if (work_visible &&
        inside(wk_x, wk_y, WK_W, (unsigned char)WK_H, mx, my))
        work_click(mx, my);
}

static void close_app(void)
{
    unsigned char action = confirm_discard(AFTER_CLOSE_APP);
    if (action == CONFIRM_CANCEL) {
        gb_restore_parent();
        return;
    }
    if (action == CONFIRM_WAIT) return;
    release_document_page();
    FS_XFLAGS_K = 0;
    gb_wm_close();
}

static void finish_open(unsigned char fresh)
{
    if (!fresh && io_off < doc_len) {
        io_error();
        return;
    }
    if (!fresh) doc_len = io_off;
    io_job = IO_IDLE;
    gb_drop_release();
    loaded = 1;
    dirty = fresh;
    if (fresh) {
        named = 0;
        clear_name();
    }
    scroll_x = scroll_y = tile_x = tile_y = 0;
    work_visible = work_sel_on = undo_valid = 0;
    front_pane = PANE_PREVIEW;
    select_document();
    if (fresh) {
        if (!load_tile()) {
            close_document();
            gb_alert("New picture failed", "Could not read canvas");
        } else {
            work_visible = 1;
            front_pane = PANE_WORK;
        }
    }
    repaint_all();                  /* Old panes were removed before starting this job. */
}

static void finish_save(void)
{
    unsigned char after = io_after;
    io_job = IO_IDLE;
    io_after = AFTER_NONE;
    FS_XFLAGS_K = 0;
    gb_drop_release();
    dirty = 0;
    undo_valid = 0;
    if (after == AFTER_NEW) start_new(pending_width, pending_height);
    else if (after == AFTER_LOAD) do_load();
    else if (after == AFTER_CLOSE_DOC) {
        close_document();
        gb_restore_parent();
    } else if (after == AFTER_CLOSE_APP) {
        release_document_page();
        gb_wm_close();
    } else gb_restore_parent();
}

static void step_job(void)
{
    unsigned int take, got, i;
    unsigned char blank;
    if (io_job == IO_LOAD) {
        if (io_off == DOC_MAX) {
            if (read_chunk(io_off, 1))
                io_error();
            else finish_open(0);
            return;
        }
        take = DOC_MAX - io_off;
        if (take > PAINT_IO_MAX) take = PAINT_IO_MAX;
        got = read_chunk(io_off, take);
        if (io_first) {
            if (!parse_header((unsigned char *)gb_copybuf, got)) {
                io_error();
                return;
            }
            io_first = 0;
        }
        if (got && !document_write(io_off, gb_copybuf, got)) {
            io_error();
            return;
        }
        io_off += got;
        if (got < take) finish_open(0);
        return;
    }
    if (io_job == IO_NEW) {
        take = doc_len - io_off;
        if (take > PAINT_IO_MAX) take = PAINT_IO_MAX;
        blank = pic_mode == 7 ? 0x11 : 0xF0;
        for (i = 0; i < take; i++)
            ((unsigned char *)gb_copybuf)[i] = blank;
        if (!document_write(io_off, gb_copybuf, take)) {
            io_error();
            return;
        }
        io_off += take;
        if (io_off == doc_len) finish_open(1);
        return;
    }
    take = doc_len - io_off;
    if (take > PAINT_IO_MAX) take = PAINT_IO_MAX;
    if (!document_read(io_off, gb_copybuf, take)) {
        io_error();
        return;
    }
    gb_set_name(cur_name);
    FS_XFLAGS_K = io_first ? 0x04 : 0x06;
    if (!gb_fs_save(gb_copybuf, take)) {
        io_first = 0;
        io_error();
        return;
    }
    io_first = 0;
    FS_XFLAGS_K = 0;
    UI_MODAL_K = 0;
    io_off += take;
    if (io_off == doc_len) finish_save();
}

static void paint_frame(void)
{
    unsigned char flags;
    if (io_job != IO_IDLE) {
        flags = gb_flags();
        if ((flags & GB_QUIT) || gb_getkey() == 27) {
            stop_job(1);
            gb_restore_parent();
        } else step_job();
        return;
    }
    if (want_menu) {
        run_menu();
        return;
    }
    flags = gb_flags();
    if (flags & GB_QUIT) {
        close_app();
        return;
    }
    if (stroke_active) {
        continue_stroke();
        return;
    }
    if (flags & GB_CLICK) handle_click();
}

static void paint_event(void)
{
    unsigned char col;
    if (gb_msg.type != GB_MSG_MENU) return;
    col = gb_msg.p0;
    if (menu_title_hit(col, MENU_FILE_X)) want_menu = 1;
    else if (menu_title_hit(col, MENU_EDIT_X)) want_menu = 2;
}

static const gb_win_t paint_window = {
    0, 8, GB_COLS, GB_LINES - 8,
    paint_frame, repaint_all, paint_event, menu_def
};

static void initial_layout(void)
{
    tc_x = (unsigned char)(GB_COLS - TC_W - 1);
    tc_y = 12;
    pv_x = 1;
    pv_y = 12;
#ifdef GB_MSX2
    wk_x = 39;
    wk_y = 32;
#else
#ifdef GB_PCW
    wk_x = 34;
    wk_y = 64;
#else
    wk_x = 25;
    wk_y = 24;
#endif
#endif
}

void main(void)
{
    unsigned char i;
    io_job = IO_IDLE;
    gb_wm_add(&paint_window);
#ifdef GB_MSX2
    if (MSX_SCRMOD != 7) {
        gb_alert("PAINT needs Mode 7", "Select 16 colors");
        gb_wm_close();
        return;
    }
#endif
    reset_editor_state();
    current_tool = TOOL_PENCIL;
    current_pen = 2;
    front_pane = PANE_PREVIEW;
    initial_layout();
    gb_get_name(launch_name);
    load_tools();
#if !defined(GB_MSX2) && !defined(GB_PCW)
    load_picedit_helper();
#endif
    if (is_pic_name(launch_name)) start_load(launch_name);
    for (i = 64; i; i--) if (!gb_getkey()) break;
    gb_restore_parent();
}
