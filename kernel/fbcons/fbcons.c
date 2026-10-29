#include <vex/console.h>
#include <vex/fb.h>
#include <vex/fbcons.h>
#include <vex/hart.h>
#include <vex/kmalloc.h>
#include <vex/kprintf.h>
#include <vex/panic.h>
#include <vex/string.h>

#include "generated.fbcons_font.h"

#define CHR_BS  0x08
#define CHR_HT  0x09
#define CHR_LF  0x0A
#define CHR_VT  0x0B
#define CHR_FF  0x0C
#define CHR_CR  0x0D
#define CHR_WS  0x20
#define CHR_DEL 0x7F

enum {
    CURSOR_NONE,
    CURSOR_INVERT,
    CURSOR_OVERLAP,
    CURSOR_FRAME,
};

#if defined(CONFIG_FBCONS_CURSOR_INVERT)
#define CURSOR_STYLE CURSOR_INVERT
#elif defined(CONFIG_FBCONS_CURSOR_OVERLAP)
#define CURSOR_STYLE CURSOR_OVERLAP
#elif defined(CONFIG_FBCONS_CURSOR_FRAME)
#define CURSOR_STYLE CURSOR_FRAME
#else
#define CURSOR_STYLE CURSOR_NONE
#endif

// The cursor sits in the bottom left corner of its cell.
// Its size is a share of the cell, rounded up to whole pixels
#define CURSOR_HEIGHT ((FBCONS_FONT_HEIGHT * CONFIG_FBCONS_CURSOR_HEIGHT + 99) / 100)
#define CURSOR_WIDTH  ((FBCONS_FONT_WIDTH * CONFIG_FBCONS_CURSOR_WIDTH + 99) / 100)

static_assert(CURSOR_HEIGHT <= FBCONS_FONT_HEIGHT && CURSOR_WIDTH <= FBCONS_FONT_WIDTH);

typedef struct vt_cell_s vt_cell_t;

struct vt_cell_s {
    char value;
    bool dirty;
};

static size_t fb_width = 0U;
static size_t fb_height = 0U;
static size_t fb_depth = 0U;
static size_t fb_stride = 0U;
static uint8_t* fb_8 = nullptr;
static uint16_t* fb_16 = nullptr;
static uint32_t* fb_32 = nullptr;

static size_t vt_width = 0U;
static size_t vt_height = 0U;
static size_t vt_length = 0U;
static fbcons_restore_t restore_mode = nullptr;
static void* restore_ctxdata = nullptr;
static vt_cell_t* vt_cells = nullptr;

static size_t cursor_xpos_prev = 0U;
static size_t cursor_ypos_prev = 0U;
static size_t cursor_xpos = 0U;
static size_t cursor_ypos = 0U;

// The redraw only walks the cells between these, so that
// a character costs one cell rather than a pass over the screen
static size_t dirty_first = SIZE_MAX;
static size_t dirty_last = 0U;

static __always_inline void put_pixel(size_t offset, bool white)
{
    switch(fb_depth) {
        case 8:
            fb_8[offset] = white ? UINT8_C(0xFF) : UINT8_C(0x00);
            break;

        case 16:
            fb_16[offset] = white ? UINT16_C(0xFFFF) : UINT16_C(0x0000);
            break;

        case 32:
            fb_32[offset] = white ? UINT32_C(0xFFFFFFFF) : UINT32_C(0x00000000);
            break;

        default:
            panic("fbcons: unsupported framebuffer depth");
    }
}

static const unsigned char* glyph_of(int value)
{
    if(value < 0 || value >= FBCONS_FONT_GLYPHS)
        value = 0;
    return &fbcons_font[value * FBCONS_FONT_GLYPH_BYTES];
}

static void render_cell(size_t sx, size_t sy, int value)
{
    size_t pixel = 0U;

    size_t i = sy * FBCONS_FONT_HEIGHT;
    if(i > fb_height)
        return;
    pixel += i * fb_stride;

    i = sx * FBCONS_FONT_WIDTH;
    if(i > fb_width)
        return;
    pixel += i;

    const unsigned char* gptr = glyph_of(value);

    for(size_t gy = 0U; gy < FBCONS_FONT_HEIGHT; gy += 1U) {
        for(size_t gx = 0U; gx < FBCONS_FONT_WIDTH; gx += 1U)
            put_pixel(pixel + gx, (gptr[gx >> 3] & (0x80 >> (gx & 0x07))));
        gptr += FBCONS_FONT_ROW_BYTES;
        pixel += fb_stride;
    }
}

static bool cursor_pixel(size_t gx, size_t gy, bool glyph)
{
    switch(CURSOR_STYLE) {
        case CURSOR_INVERT:
            return !glyph;

        case CURSOR_FRAME:
            if(gx == 0U || gx == CURSOR_WIDTH - 1U)
                return true;
            if(gy == 0U || gy == CURSOR_HEIGHT - 1U)
                return true;
            return glyph;

        default:
            return true;
    }
}

static void render_cursor(size_t sx, size_t sy, int value)
{
    size_t pixel = 0U;

    size_t i = sy * FBCONS_FONT_HEIGHT;
    if(i > fb_height)
        return;
    pixel += i * fb_stride;

    i = sx * FBCONS_FONT_WIDTH;
    if(i > fb_width)
        return;
    pixel += i;

    size_t skipped = FBCONS_FONT_HEIGHT - CURSOR_HEIGHT;
    pixel += skipped * fb_stride;
    const unsigned char* gptr = glyph_of(value) + skipped * FBCONS_FONT_ROW_BYTES;

    for(size_t gy = 0U; gy < CURSOR_HEIGHT; gy += 1U) {
        for(size_t gx = 0U; gx < CURSOR_WIDTH; gx += 1U) {
            bool glyph = gptr[gx >> 3] & (0x80 >> (gx & 0x07));
            put_pixel(pixel + gx, cursor_pixel(gx, gy, glyph));
        }
        gptr += FBCONS_FONT_ROW_BYTES;
        pixel += fb_stride;
    }
}

static void set_cell(size_t index, char value)
{
    vt_cell_t* cell = &vt_cells[index];

    if(cell->value == value) {
        return;
    }

    cell->value = value;
    cell->dirty = true;

    if(index < dirty_first) {
        dirty_first = index;
    }

    if(index > dirty_last) {
        dirty_last = index;
    }
}

// Only cells whose character changes are redrawn,
// so scrolling past blank space costs next to nothing
static void do_newline(void)
{
    cursor_ypos += 1U;

    if(cursor_ypos >= vt_height) {
        cursor_ypos = vt_height - 1U;
        size_t subscreen = vt_length - vt_width;

        for(size_t i = 0U; i < subscreen; ++i) {
            set_cell(i, vt_cells[i + vt_width].value);
        }

        for(size_t i = subscreen; i < vt_length; ++i) {
            set_cell(i, CHR_WS);
        }
    }
}

static void do_redraw(void)
{
    for(size_t i = dirty_first; i <= dirty_last && i < vt_length; ++i) {
        vt_cell_t* cell = &vt_cells[i];

        if(cell->dirty) {
            render_cell(i % vt_width, i / vt_width, cell->value);

            cell->dirty = false;
        }
    }

    dirty_first = SIZE_MAX;
    dirty_last = 0U;

    if(CURSOR_STYLE == CURSOR_NONE)
        return;

    vt_cell_t* cell = &vt_cells[cursor_ypos_prev * vt_width + cursor_xpos_prev];
    render_cell(cursor_xpos_prev, cursor_ypos_prev, cell->value);

    cell = &vt_cells[cursor_ypos * vt_width + cursor_xpos];
    render_cursor(cursor_xpos, cursor_ypos, cell->value);

    cursor_xpos_prev = cursor_xpos;
    cursor_ypos_prev = cursor_ypos;
}

static void do_clear(void)
{
    for(size_t y = 0U; y < fb_height; ++y) {
        for(size_t x = 0U; x < fb_width; ++x) {
            put_pixel(y * fb_stride + x, false);
        }
    }

    memset(vt_cells, 0, vt_length * sizeof(vt_cell_t));

    dirty_first = SIZE_MAX;
    dirty_last = 0U;
}

static void do_reset(void)
{
    do_clear();

    cursor_xpos_prev = 0U;
    cursor_ypos_prev = 0U;
    cursor_xpos = 0U;
    cursor_ypos = 0U;
}

static void fbcons_putchar(int character)
{
    switch(character) {
        case CHR_BS:
        case CHR_DEL:
            if(cursor_xpos > 0U)
                cursor_xpos -= 1U;
            return;

        case CHR_HT:
            cursor_xpos += CONFIG_FBCONS_TAB_WIDTH - (cursor_xpos % CONFIG_FBCONS_TAB_WIDTH);
            if(cursor_xpos >= vt_width)
                cursor_xpos = vt_width - 1U;
            return;

        case CHR_LF:
            cursor_xpos = 0U;
            do_newline();
            return;

        case CHR_VT:
            do_newline();
            return;

        case CHR_FF:
            do_reset();
            return;

        case CHR_CR:
            cursor_xpos = 0U;
            return;
    }

    set_cell(cursor_ypos * vt_width + cursor_xpos, (char)(character));

    cursor_xpos += 1U;

    if(cursor_xpos >= vt_width) {
        cursor_xpos = 0U;
        do_newline();
    }
}

static void fbcons_write(console_t* restrict cons, const void* restrict data, size_t size)
{
    const char* psz = data;

    for(size_t i = 0U; i < size; fbcons_putchar(psz[i++])) {
        // empty
    }

    do_redraw();
}

// Whatever the device's owner drew, the console core's recall fills a blank screen
// next; nothing here locks or allocates, as vt_cells is the one from the attach
static bool fbcons_takeover(console_t* restrict cons)
{
    if(restore_mode) {
        restore_mode(restore_ctxdata);
    }

    do_reset();

    return true;
}

static const console_ops_t fbcons_ops = {
    .write = &fbcons_write,
    .takeover = &fbcons_takeover,
};

static console_t fbcons_driver = {
    .name = "fbcons",
    .consflags = CONSF_RECALL,
    .ops = &fbcons_ops,
};

console_t* fbcons_attach(const vexfb_info_t* restrict info, void* restrict virt, fbcons_restore_t restore, void* restrict ctxdata)
{
    bool depth_supported = info->bpp == 8 || info->bpp == 16 || info->bpp == 32;

    if(!depth_supported || (info->pitch % (info->bpp / 8))) {
        return nullptr;
    }

    fb_width = info->width;
    fb_height = info->height;
    fb_depth = info->bpp;
    fb_stride = info->pitch / (info->bpp / 8);

    restore_mode = restore;
    restore_ctxdata = ctxdata;

    fb_8 = virt;
    fb_16 = virt;
    fb_32 = virt;

    vt_width = fb_width / FBCONS_FONT_WIDTH;
    vt_height = fb_height / FBCONS_FONT_HEIGHT;
    vt_length = vt_width * vt_height;

    vt_cells = kmalloc(vt_length * sizeof(vt_cell_t));

    if(vt_cells == nullptr) {
        panic("fbcons: out of memory: vt_cells");
    }

    do_reset();

    console_add(&fbcons_driver);

    return &fbcons_driver;
}

void fbcons_detach(void)
{
    console_remove(&fbcons_driver);

    kfree(vt_cells);
    vt_cells = nullptr;

    restore_mode = nullptr;
    restore_ctxdata = nullptr;
}
