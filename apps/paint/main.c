/*
 * PAINT.APP - a banked, three-pane GEOBENCH picture editor.
 *
 * Paint owns one invisible full-screen WM workspace and draws three ordinary
 * GEOBENCH windows inside it. This avoids registering several windows against
 * one application page (closing any such window would release the shared page):
 *
 *   Toolchest    always visible and always painted on top
 *   Area selector 1:1 scrollable picture preview with a fixed 10x10 navigator
 *   Canvas       the selected 10x10 pixels enlarged to a 100x100 work area
 *
 * The document itself lives in one borrowed 16 KiB application-pool page.
 * Only the selected 10x10 pixels, undo snapshot, and clipboard are retained in
 * this application page. Mode-1 pictures are portable across CPC/MSX/PCW;
 * mode-7 pictures and sixteen-colour editing are accepted only by MSX Paint
 * running under Screen 7.
 */
#include "gb.h"

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

#define TILE_SIDE 10
#define TILE_PIXELS 100
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
#define WK_W 27
#define WK_H (TITLE_H + 101)
#define SB_W 3
#define HSB_H 6

#define PANE_PREVIEW 0
#define PANE_WORK    1

#define IST_MAX 1800
#define TOOL_BITS_PER ((TOOL_WB * 4 * TOOL_H) / 8)
#define TOOL_BITS_LEN (N_TOOLS * TOOL_BITS_PER)
#define NATIVE_STAGE ((unsigned char *)gb_copybuf + 6000)

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
static unsigned char tile[TILE_PIXELS];
static unsigned char undo_tile[TILE_PIXELS];
static unsigned char undo_valid;
static unsigned char work_sel_on;
static unsigned char work_sel_x0, work_sel_y0, work_sel_x1, work_sel_y1;

static unsigned char current_tool;
static unsigned char current_pen;
static unsigned char stroke_active;
static unsigned char stroke_x, stroke_y;
static unsigned char random_state = 0x5D;

static unsigned char tool_bits[TOOL_BITS_LEN];
static unsigned char ist_ok;
static unsigned char pv_view_x, pv_view_y, pv_view_w, pv_view_h;
static unsigned char pv_image_x, pv_image_y;
static unsigned char pv_hbar_x, pv_hbar_y, pv_hbar_w;
static unsigned char want_menu;

static void repaint_all(void);
static void draw_toolchest(void);
static void draw_preview(void);
static void draw_work(void);
static void close_app(void);
static unsigned char save_document(void);
static unsigned char commit_tile(void);
static unsigned char unpack_mode1(unsigned char value, unsigned char pixel);

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
    unsigned char *dest = tool_bits;
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
    if (named) gb_set_name(cur_name);
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
    unsigned char row, col, first, last, count, span, position;
    unsigned int remain;
    unsigned char *buf = (unsigned char *)gb_copybuf;
    unsigned char *pixel;

    if (!write)
        for (row = 0; row < TILE_PIXELS; row++) tile[row] = 1;
    if (!loaded) return 0;
    if (tile_x >= pic_width || tile_y >= pic_height) return 0;
    remain = pic_width - tile_x;
    count = (unsigned char)(remain > TILE_SIDE ? TILE_SIDE : remain);
    if (pic_mode == 7) {
        first = (unsigned char)(tile_x >> 1);
        last = (unsigned char)((tile_x + count - 1) >> 1);
        position = (unsigned char)tile_x & 1;
    } else {
        first = (unsigned char)(tile_x >> 2);
        last = (unsigned char)((tile_x + count - 1) >> 2);
        position = (unsigned char)tile_x & 3;
    }
    span = (unsigned char)(last - first + 1);
    /* SDCC's high-allocation pass folds the three-term row offset through a
       stack address on Z80. Keep the live offset in fixed scratch words. */
    fx1 = tile_y;
    fx0 = fx1 * pic_stride;
    fx0 += pic_off;
    fx0 += first;
    for (row = 0; row < TILE_SIDE && fx1 < pic_height; row++) {
        if (!document_read(fx0, buf, span))
            return 0;
        pixel = &tile[(unsigned char)(row * TILE_SIDE)];
        for (col = 0; col < count; col++) {
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
    unsigned char *source = &tool_bits[(unsigned int)index * TOOL_BITS_PER];
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

static void draw_toolchest(void)
{
    unsigned char i;
    unsigned char py = (unsigned char)(tc_y + TITLE_H +
                                       TOOL_ROWS * TOOL_STEP_Y + 1);
    gb_window(tc_x, tc_y, TC_W, TC_H, tool_title());
    if (ist_ok) for (i = 0; i < N_TOOLS; i++) draw_tool_icon(i);
    for (i = 0; i < N_TOOLS; i++)
        gb_frame(tool_x(i), tool_y(i), TOOL_WB, TOOL_H,
                 (i == current_tool) ? 3 : 2);
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
    fx0 = scroll_x;
    fx0 <<= 2;
    if (tile_x < fx0 || tile_y < scroll_y) return;
    fx1 = tile_x;
    fx1 -= fx0;
    fy0 = tile_y;
    fy0 -= scroll_y;
    if (fx1 + TILE_SIDE > (unsigned int)pv_view_w * 4U ||
        fy0 + TILE_SIDE > pv_view_h) return;
    fx0 = pv_image_x;
    fx0 <<= 2;
    fx0 += fx1;
    fx1 = fx0 + TILE_SIDE - 1;
    fy0 += pv_image_y;
    fy1 = fy0 + TILE_SIDE - 1;
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

static unsigned char mode1_solid(unsigned char pen)
{
    return (unsigned char)(((pen & 1) ? 0xF0 : 0) |
                           ((pen & 2) ? 0x0F : 0));
}

static unsigned char mode1_pair(unsigned char left, unsigned char right)
{
    return (unsigned char)(((left & 1) ? 0xC0 : 0) |
                           ((left & 2) ? 0x0C : 0) |
                           ((right & 1) ? 0x30 : 0) |
                           ((right & 2) ? 0x03 : 0));
}

static void draw_work_bitmap(void)
{
    unsigned char sy, repeat, sx, p0, p1, value;
#ifdef GB_MSX2
    unsigned char n;
#endif
    unsigned char *out = (unsigned char *)gb_copybuf;
    unsigned char *source;
#ifdef GB_MSX2
    if (pic_mode == 7) {
        for (sy = 0; sy < TILE_SIDE; sy++) {
            for (repeat = 0; repeat < TILE_SIDE; repeat++) {
                source = &tile[(unsigned char)(sy * TILE_SIDE)];
                for (sx = 0; sx < TILE_SIDE; sx++) {
                    p0 = *source++;
                    value = (unsigned char)((p0 << 4) | p0);
                    for (n = 0; n < 5; n++) *out++ = value;
                }
            }
        }
        native_blit((unsigned char)(wk_x + 1),
                    (unsigned char)(wk_y + TITLE_H), 25, 100,
                    (unsigned char *)gb_copybuf);
        return;
    }
#endif
    for (sy = 0; sy < TILE_SIDE; sy++) {
        for (repeat = 0; repeat < TILE_SIDE; repeat++) {
            source = &tile[(unsigned char)(sy * TILE_SIDE)];
            for (sx = 0; sx < TILE_SIDE; sx += 2) {
                p0 = *source++;
                p1 = *source++;
                value = mode1_solid(p0);
                *out++ = value;
                *out++ = value;
                *out++ = mode1_pair(p0, p1);
                value = mode1_solid(p1);
                *out++ = value;
                *out++ = value;
            }
        }
    }
    blit_mode1((unsigned char)(wk_x + 1),
               (unsigned char)(wk_y + TITLE_H), 25, 100,
               (unsigned char *)gb_copybuf, 25);
}

static void draw_work_grid(void)
{
    unsigned char i;
    int left = (int)(wk_x + 1) * 4;
    int top = wk_y + TITLE_H;
    for (i = 1; i < TILE_SIDE; i++) {
        line(left + i * 10, top, left + i * 10, top + 99, 2);
        line(left, top + i * 10, left + 99, top + i * 10, 2);
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

static void draw_work_selection(void)
{
    if (!work_sel_on) return;
    fx0 = (unsigned int)(wk_x + 1);
    fx0 <<= 2;
    fx1 = work_sel_x0;
    fx1 *= 10;
    fx0 += fx1;
    fy0 = (unsigned int)(wk_y + TITLE_H);
    fy1 = work_sel_y0;
    fy1 *= 10;
    fy0 += fy1;
    fx1 = (unsigned int)(work_sel_x1 - work_sel_x0 + 1);
    fx1 *= 10;
    fx1 += fx0;
    fx1--;
    fy1 = (unsigned int)(work_sel_y1 - work_sel_y0 + 1);
    fy1 *= 10;
    fy1 += fy0;
    fy1--;
    fpen = 3;
    fw_frame();
}

static void draw_work(void)
{
    if (!work_visible) return;
    gb_window(wk_x, wk_y, WK_W, WK_H, "Canvas 10x");
    draw_work_bitmap();
    draw_work_grid();
    draw_work_selection();
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

static void draw_work_cell(unsigned char x, unsigned char y)
{
    unsigned char row;
    int left = (int)(wk_x + 1) * 4 + x * 10;
    int top = wk_y + TITLE_H + y * 10;
    unsigned char pen = tile[(unsigned char)(y * 10 + x)];
    for (row = 0; row < 10; row++)
        line(left, top + row, left + 9, top + row, pen);
    line(left, top, left + 9, top, 2);
    line(left, top, left, top + 9, 2);
}

/* ---- work tools ---------------------------------------------------------- */

static void save_undo(void)
{
    unsigned char i;
    for (i = 0; i < TILE_PIXELS; i++) undo_tile[i] = tile[i];
    undo_valid = 1;
}

static void restore_undo_base(void)
{
    unsigned char i;
    for (i = 0; i < TILE_PIXELS; i++) tile[i] = undo_tile[i];
}

static void set_tile_pixel(signed char x, signed char y)
{
    if (x >= 0 && x < TILE_SIDE && y >= 0 && y < TILE_SIDE)
        tile[(unsigned char)(y * TILE_SIDE + x)] = current_pen;
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
        set_tile_pixel(x0, y0);
        if (live) draw_work_cell((unsigned char)x0, (unsigned char)y0);
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

static unsigned char small_sqrt(unsigned char value)
{
    unsigned char root = 0;
    while ((unsigned char)((root + 1) * (root + 1)) <= value) root++;
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

static void flood_fill(unsigned char x, unsigned char y)
{
    unsigned char target = tile[(unsigned char)(y * 10 + x)];
    unsigned char *stack = (unsigned char *)gb_copybuf;
    unsigned char sp = 0;
    if (target == current_pen) return;
    stack[sp++] = (unsigned char)(y * 10 + x);
    while (sp) {
        unsigned char value = stack[--sp];
        unsigned char px = (unsigned char)(value % 10);
        unsigned char py = (unsigned char)(value / 10);
        if (tile[value] != target) continue;
        tile[value] = current_pen;
        if (px && sp < 96) stack[sp++] = (unsigned char)(value - 1);
        if (px < 9 && sp < 96) stack[sp++] = (unsigned char)(value + 1);
        if (py && sp < 96) stack[sp++] = (unsigned char)(value - 10);
        if (py < 9 && sp < 96) stack[sp++] = (unsigned char)(value + 10);
    }
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
        if (px >= 0 && px < 10 && py >= 0 && py < 10) {
            set_tile_pixel(px, py);
            draw_work_cell((unsigned char)px, (unsigned char)py);
        }
    }
}

static void finish_change(void)
{
    if (!commit_tile()) gb_alert("Paint error", "Could not write tile");
    gb_curhide();
    repaint_picture_panes();
    gb_curshow();
}

static void do_undo(void)
{
    unsigned char i, value;
    if (!work_visible || !undo_valid) return;
    for (i = 0; i < TILE_PIXELS; i++) {
        value = tile[i];
        tile[i] = undo_tile[i];
        undo_tile[i] = value;
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
        work_sel_x1 = work_sel_y1 = 9;
    }
    normalize_work_selection();
    clip_w = (unsigned char)(work_sel_x1 - work_sel_x0 + 1);
    clip_h = (unsigned char)(work_sel_y1 - work_sel_y0 + 1);
    clip[0] = PAINT_CLIP_MAGIC;
    clip[1] = clip_w;
    clip[2] = clip_h;
    for (y = 0; y < clip_h; y++)
        for (x = 0; x < clip_w; x++)
            clip[(unsigned char)(3 + y * 10 + x)] =
                tile[(unsigned char)((work_sel_y0 + y) * 10 +
                                     work_sel_x0 + x)];
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
            tile[(unsigned char)(y * 10 + x)] = 1;
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
    if (!clip_w || clip_w > 10 || !clip_h || clip_h > 10) return;
    if (work_sel_on) { ox = work_sel_x0; oy = work_sel_y0; }
    if (clip_w > (unsigned char)(10 - ox))
        clip_w = (unsigned char)(10 - ox);
    if (clip_h > (unsigned char)(10 - oy))
        clip_h = (unsigned char)(10 - oy);
    save_undo();
    for (y = 0; y < clip_h; y++)
        for (x = 0; x < clip_w; x++)
            tile[(unsigned char)((oy + y) * 10 + ox + x)] =
                clip[(unsigned char)(3 + y * 10 + x)];
    finish_change();
}

static unsigned char work_point(unsigned char *x, unsigned char *y,
                                unsigned char clamp)
{
    int px = (int)gb_mxp() - (int)(wk_x + 1) * 4;
    int py = (int)gb_my() - (int)(wk_y + TITLE_H);
    if (!clamp && (px < 0 || py < 0 || px >= 100 || py >= 100)) return 0;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > 99) px = 99;
    if (py > 99) py = 99;
    *x = (unsigned char)(px / 10);
    *y = (unsigned char)(py / 10);
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
            small_sqrt((unsigned char)(dx * dx + dy * dy)),
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
        restore_undo_base();
        apply_shape(sx, sy, ex, ey);
        gb_curhide();
        draw_work();
        gb_curshow();
    }
    restore_undo_base();
    apply_shape(sx, sy, ex, ey);
    finish_change();
}

static void drag_work_selection(unsigned char sx, unsigned char sy)
{
    unsigned char nx = sx, ny = sy, flags;
    work_sel_on = 1;
    work_sel_x0 = work_sel_x1 = sx;
    work_sel_y0 = work_sel_y1 = sy;
    for (;;) {
        flags = gb_poll();
        if (!(flags & GB_FIRE)) break;
        work_point(&nx, &ny, 1);
        if (nx == work_sel_x1 && ny == work_sel_y1) continue;
        work_sel_x1 = nx;
        work_sel_y1 = ny;
        gb_curhide();
        draw_work();
        gb_curshow();
    }
    work_sel_x1 = nx;
    work_sel_y1 = ny;
    normalize_work_selection();
    gb_restore_parent();
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
            set_tile_pixel(x, y);
            draw_work_cell(x, y);
        } else spray_at(x, y);
        gb_curshow();
    }
}

static void continue_stroke(void)
{
    unsigned char x, y;
    if (!(gb_flags() & GB_FIRE)) {
        stroke_active = 0;
        finish_change();
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

static unsigned char selector_screen(int *left, int *top)
{
    int relx, rely;
    layout_preview();
    relx = (int)tile_x - (int)(scroll_x * 4U);
    rely = (int)tile_y - (int)scroll_y;
    *left = (int)pv_image_x * 4 + relx;
    *top = pv_image_y + rely;
    return (unsigned char)(relx >= 0 && rely >= 0 &&
        relx + 10 <= (int)pv_view_w * 4 &&
        rely + 10 <= pv_view_h);
}

static void clamp_tile_origin(void)
{
    unsigned int maxx = pic_width > 10 ? pic_width - 10 : 0;
    unsigned int maxy = pic_height > 10 ? pic_height - 10 : 0;
    if (tile_x > maxx) tile_x = maxx;
    if (tile_y > maxy) tile_y = maxy;
}

static void drag_selector(void)
{
    int left, top, px, py, nx, ny;
    int grabx = 5, graby = 5;
    unsigned char flags;
    unsigned int source_x, source_y;

    layout_preview();
    px = (int)gb_mxp();
    py = gb_my();
    if (selector_screen(&left, &top) &&
        px >= left && px < left + 10 && py >= top && py < top + 10) {
        grabx = px - left;
        graby = py - top;
    } else {
        source_x = scroll_x * 4U +
                   (unsigned int)(px - (int)pv_image_x * 4);
        source_y = scroll_y + (unsigned int)(py - pv_image_y);
        tile_x = source_x > 5 ? source_x - 5 : 0;
        tile_y = source_y > 5 ? source_y - 5 : 0;
        clamp_tile_origin();
    }
    for (;;) {
        flags = gb_poll();
        if (!(flags & GB_FIRE)) break;
        px = (int)gb_mxp() - (int)pv_image_x * 4 - grabx;
        py = (int)gb_my() - pv_image_y - graby;
        nx = (int)(scroll_x * 4U) + px;
        ny = (int)scroll_y + py;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if ((unsigned int)nx == tile_x && (unsigned int)ny == tile_y) continue;
        tile_x = (unsigned int)nx;
        tile_y = (unsigned int)ny;
        clamp_tile_origin();
        gb_curhide();
        draw_preview();
        draw_toolchest();
        gb_curshow();
    }
    if (!load_tile()) {
        gb_alert("Paint error", "Could not read tile");
        return;
    }
    work_visible = 1;
    front_pane = PANE_WORK;
    gb_restore_parent();
}

static void drag_vscroll(void)
{
    unsigned int value;
    unsigned char flags;
    do {
        value = scroll_value(pv_view_y, pv_view_h,
                             pic_height, pv_view_h, gb_my());
        if (value != scroll_y) {
            tile_y += value - scroll_y;
            scroll_y = value;
            clamp_tile_origin();
            gb_curhide(); draw_preview(); draw_toolchest(); gb_curshow();
        }
        flags = gb_poll();
    } while (flags & GB_FIRE);
    load_tile();
    gb_restore_parent();
}

static void drag_hscroll(void)
{
    unsigned int value;
    unsigned char flags;
    do {
        value = scroll_value(pv_hbar_x, pv_hbar_w,
                             pic_wb, pv_view_w, gb_mx());
        if (value != scroll_x) {
            tile_x += (value - scroll_x) << 2;
            scroll_x = value;
            clamp_tile_origin();
            gb_curhide(); draw_preview(); draw_toolchest(); gb_curshow();
        }
        flags = gb_poll();
    } while (flags & GB_FIRE);
    load_tile();
    gb_restore_parent();
}

/* ---- files --------------------------------------------------------------- */

static const char *const pic_exts[] = { "PIC", 0 };
static char prompt_name[16];

static unsigned char inspect_header(const unsigned char *header)
{
    if (header[0] != 'G' || header[1] != 'B' ||
        header[2] != 'P' || header[3] != 'C') return 0;
    if (header[4] == 2) {
        if (header[5] != 1 && header[5] != 6 && header[5] != 7)
            return 0;
#ifndef GB_MSX2
        if (header[5] == 7) return 2;
#endif
    } else if (!header[4] || !header[5]) return 0;
    return 1;
}

static unsigned char open_named_picture(const char *name)
{
    unsigned int source_width;
    unsigned char page2;
    unsigned char *header = (unsigned char *)gb_copybuf;
#ifdef GB_MSX2
    unsigned char page3, page4;
#endif
    gb_set_name(name);
    doc_page = gb_pic_open();
    UI_MODAL_K = 0;
    if (!doc_page) {
#ifndef GB_MSX2
        gb_alert("Picture not loaded", "Four-color GBPC required");
#else
        gb_alert("Picture not loaded", "Invalid or no free bank");
#endif
        if (named) gb_set_name(cur_name);
        return 0;
    }
    page2 = PIC_PAGE2_K;
#ifdef GB_MSX2
    page3 = PIC_PAGE3_K;
    page4 = PIC_PAGE4_K;
#endif
    doc_len = PIC_SIZE_K;
    if (page2 || doc_len > DOC_MAX
#ifdef GB_MSX2
        || page3 || page4
#endif
        ) {
        PIC_PAGE_K = doc_page;
        PIC_PAGE2_K = page2;
#ifdef GB_MSX2
        PIC_PAGE3_K = page3;
        PIC_PAGE4_K = page4;
#endif
        gb_pic_close();
        doc_page = 0;
        gb_alert("Picture too large", "16K maximum");
        if (named) gb_set_name(cur_name);
        return 0;
    }
    pic_wb = PIC_WB_K;
    pic_height = PIC_H_K;
    pic_off = PIC_OFF_K;
#ifdef GB_MSX2
    pic_mode = PIC_MODE_K;
    pic_stride = PIC_STRIDE_K;
#else
    pic_mode = 1;
    pic_stride = pic_wb;
#endif
    select_document();
    if (!document_read(0, header, PIC_HDR) ||
        inspect_header(header) != 1) {
        release_document_page();
        gb_alert("Invalid picture", "GBPC header required");
        if (named) gb_set_name(cur_name);
        return 0;
    }
    source_width = header[4] == 2 ?
        ((unsigned int)header[6] | ((unsigned int)header[7] << 8)) : 0;
    pic_width = source_width ? source_width : (unsigned int)pic_wb * 4U;
    if (!pic_width || !pic_height || !pic_stride || doc_len < pic_off ||
        pic_height > (unsigned int)((doc_len - pic_off) / pic_stride)) {
        release_document_page();
        gb_alert("Incomplete picture", "Bitmap data missing");
        if (named) gb_set_name(cur_name);
        return 0;
    }
    copy11(cur_name, name);
    gb_set_name(cur_name);
    named = 1;
    loaded = 1;
    dirty = 0;
    scroll_x = scroll_y = 0;
    tile_x = tile_y = 0;
    work_visible = 0;
    work_sel_on = 0;
    undo_valid = 0;
    front_pane = PANE_PREVIEW;
    select_document();
    return 1;
}

static unsigned char create_picture(unsigned int width, unsigned int height)
{
    unsigned int remaining, off, take, i;
    unsigned char blank;
    unsigned char *header = (unsigned char *)gb_copybuf;

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
        release_document_page();
        gb_alert("New picture failed", "Header write error");
        return 0;
    }
    blank = pic_mode == 7 ? 0x11 : 0xF0;
    for (i = 0; i < GB_COPYMAX; i++) ((unsigned char *)gb_copybuf)[i] = blank;
    remaining = doc_len - PIC_HDR;
    off = PIC_HDR;
    while (remaining) {
        take = remaining > GB_COPYMAX ? GB_COPYMAX : remaining;
        if (!document_write(off, gb_copybuf, take)) {
            release_document_page();
            gb_alert("New picture failed", "Bitmap write error");
            return 0;
        }
        off += take;
        remaining -= take;
    }
    loaded = 1;
    named = 0;
    dirty = 1;
    clear_name();
    scroll_x = scroll_y = 0;
    tile_x = tile_y = 0;
    work_visible = 0;
    work_sel_on = 0;
    undo_valid = 0;
    select_document();
    if (!load_tile()) {
        release_document_page();
        reset_editor_state();
        gb_alert("New picture failed", "Could not read canvas");
        return 0;
    }
    work_visible = 1;
    front_pane = PANE_WORK;
    return 1;
}

static unsigned char save_to_current_name(void)
{
    unsigned int off = 0, take;
    unsigned char first = 1;
    if (!loaded || !named) return 0;
    gb_set_name(cur_name);
    while (off < doc_len) {
        take = doc_len - off;
        if (take > GB_COPYMAX) take = GB_COPYMAX;
        if (!document_read(off, gb_copybuf, take)) break;
        FS_XFLAGS_K = first ? 0x04 : 0x06;
        if (!gb_fs_save(gb_copybuf, take)) break;
        first = 0;
        off += take;
    }
    FS_XFLAGS_K = 0;
    UI_MODAL_K = 0;
    if (off != doc_len) {
        gb_alert("Save failed", "Check destination");
        return 0;
    }
    dirty = 0;
    undo_valid = 0;
    return 1;
}

static unsigned char save_as(void)
{
    char raw[11];
    if (!loaded) return 0;
    if (!gb_pickdir(pic_exts)) return 0;
    if (!gb_prompt("Save picture as:", prompt_name, 12)) return 0;
    to_83(prompt_name, raw);
    copy11(cur_name, raw);
    named = 1;
    gb_set_name(cur_name);
    return save_to_current_name();
}

static unsigned char save_document(void)
{
    if (!loaded) return 0;
    if (!named) return save_as();
    return save_to_current_name();
}

static const char *const confirm_items[] = {
    "Save", "Don't Save", "Cancel"
};

static unsigned char confirm_discard(void)
{
    unsigned char choice;
    if (!dirty) return 1;
    choice = gb_popup(28, 80, confirm_items, 3);
    if (choice == 0) return save_document();
    if (choice == 1) return 1;
    return 0;
}

static void do_new(void)
{
    unsigned int width = 100, height = 100, stride;
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
    if (!confirm_discard()) return;
    close_document();
    if (create_picture(width, height)) gb_restore_parent();
}

static void do_load(void)
{
    char name[11];
    if (!confirm_discard()) return;
    if (!gb_pickfile(name, pic_exts)) return;
    close_document();
    if (open_named_picture(name)) gb_restore_parent();
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
        else if (choice == 2) save_document();
        else if (choice == 3) save_as();
        else if (choice == 4) { close_app(); return; }
    } else if (menu == 2) {
        choice = gb_popup(MENU_EDIT_X, 8, edit_items, 4);
        if (choice == 0) do_undo();
        else if (choice == 1) cut_selection();
        else if (choice == 2) copy_selection();
        else if (choice == 3) paste_selection();
    }
    gb_restore_parent();
}

static void move_pane(unsigned char *x, unsigned char *y,
                      unsigned char w, unsigned char h)
{
    unsigned char ox = *x, oy = *y;
    unsigned char grab_x = (unsigned char)(gb_mx() - ox);
    unsigned char grab_y = (unsigned char)(gb_my() - oy);
    unsigned char nx, ny, mx, my, flags;

    do {
        flags = gb_poll();
    } while (flags & GB_FIRE);
    mx = gb_mx();
    my = gb_my();
    nx = mx >= grab_x ? (unsigned char)(mx - grab_x) : 0;
    ny = my >= grab_y ? (unsigned char)(my - grab_y) : 8;
    if (nx > (unsigned char)(GB_COLS - w))
        nx = (unsigned char)(GB_COLS - w);
    if (ny < 8) ny = 8;
    if (ny > (unsigned char)(GB_LINES - h))
        ny = (unsigned char)(GB_LINES - h);
    if (nx != ox || ny != oy) {
        *x = nx;
        *y = ny;
        gb_restore_parent();
    }
}

static void choose_pen(unsigned char pen)
{
#ifdef GB_MSX2
    if (pen > 3 && loaded && pic_mode != 7) {
        gb_alert("Four-color picture", "Pens 0-3 only");
        return;
    }
#endif
    current_pen = pen;
    gb_curhide(); draw_toolchest(); gb_curshow();
}

static void tool_action(unsigned char index)
{
    if (index == TOOL_UNDO) do_undo();
    else if (index == TOOL_CUT) cut_selection();
    else if (index == TOOL_COPY) copy_selection();
    else if (index == TOOL_PASTE) paste_selection();
    else {
        current_tool = index;
        gb_curhide(); draw_toolchest(); gb_curshow();
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
    front_pane = PANE_PREVIEW;
    if (title_hit(pv_x, pv_y, PV_W, mx, my)) {
        if (close_hit(pv_x, pv_y, mx, my)) {
            if (confirm_discard()) {
                close_document();
                gb_restore_parent();
            }
        } else move_pane(&pv_x, &pv_y, PV_W, PV_H);
        return;
    }
    layout_preview();
    if (inside((unsigned char)(pv_x + 1), pv_view_y, SB_W, pv_view_h,
               mx, my)) {
        drag_vscroll();
        return;
    }
    if (inside(pv_hbar_x, pv_hbar_y, pv_hbar_w, HSB_H, mx, my)) {
        drag_hscroll();
        return;
    }
    if (my >= pv_image_y && my < (unsigned char)(pv_image_y + pv_view_h) &&
        gb_mxp() >= (unsigned int)pv_image_x * 4U &&
        gb_mxp() < (unsigned int)(pv_image_x + pv_view_w) * 4U)
        drag_selector();
}

static void work_click(unsigned char mx, unsigned char my)
{
    front_pane = PANE_WORK;
    if (title_hit(wk_x, wk_y, WK_W, mx, my)) {
        if (close_hit(wk_x, wk_y, mx, my)) {
            work_visible = 0;
            gb_restore_parent();
        } else move_pane(&wk_x, &wk_y, WK_W, WK_H);
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
        inside(wk_x, wk_y, WK_W, WK_H, mx, my)) {
        work_click(mx, my);
        return;
    }
    if (inside(pv_x, pv_y, PV_W, PV_H, mx, my)) {
        preview_click(mx, my);
        return;
    }
    if (work_visible && inside(wk_x, wk_y, WK_W, WK_H, mx, my))
        work_click(mx, my);
}

static void close_app(void)
{
    if (!confirm_discard()) {
        gb_restore_parent();
        return;
    }
    release_document_page();
    FS_XFLAGS_K = 0;
    gb_wm_close();
}

static void paint_frame(void)
{
    unsigned char flags;
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
    wk_y = 88;
#else
    wk_x = 25;
    wk_y = 78;
#endif
#endif
}

void main(void)
{
    unsigned char i;
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
    if (is_pic_name(launch_name)) open_named_picture(launch_name);
    for (i = 64; i; i--) if (!gb_getkey()) break;
    gb_restore_parent();
}
