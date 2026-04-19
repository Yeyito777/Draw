/*
 * Draw — Screenshot annotation tool
 *
 * Takes a screenshot, presents it fullscreen, and lets the user
 * draw / erase over it.
 *
 * Controls:
 *   Left  mouse button      — draw (current colour)
 *   1–9 keys                — switch colour
 *   Ctrl+1..9               — save drawing slot
 *   Ctrl+Shift+1..9         — load drawing slot
 *   Hold right button       — switch to eraser cursor
 *   Left click + right      — erase (restore original screenshot)
 *   Scroll wheel            — change brush size
 *   Side button (up)        — undo last stroke
 *   Side button (back)      — redo
 *   ESC / Ctrl+C            — quit
 *
 * The cursor is a separate 32-bit ARGB overlay window rendered
 * with per-pixel anti-aliasing.  The compositor alpha-blends it
 * over the desktop.
 *
 * While idle / drawing the cursor is a solid red circle.
 * While erasing it becomes a transparent circle with a white
 * border so you can see the lines being removed underneath.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/shape.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── Tunables ─────────────────────────────────────────────────── */
#define BRUSH_INITIAL      5
#define BRUSH_MIN          1
#define BRUSH_MAX          50
#define ERASE_BORDER       2      /* white ring width when erasing  */
#define MAX_UNDO           30     /* max undo / redo levels         */
#define SLOT_FILE_VERSION  1
#define CURSOR_FEEDBACK_MS 1000
#define CURSOR_FEEDBACK_PAD 4

/* ── Globals ──────────────────────────────────────────────────── */
static Display *dpy;
static int      screen_num;
static Window   root;

static Window   win;                /* fullscreen drawing surface    */
static Pixmap   canvas;             /* back-buffer: screenshot + ink */
static Pixmap   original;           /* pristine screenshot for erase */
static GC       gc_draw;            /* red ink on canvas             */
static GC       gc_copy;            /* blit canvas → window          */
static GC       gc_erase;           /* copy original → canvas (clip) */
static int      scr_w, scr_h;

static int      erase_mode;             /* right button held            */
static int      stroking;               /* left button held             */
static int      prev_x = -1, prev_y = -1;
static int      cur_x, cur_y;
static int      brush_radius = BRUSH_INITIAL;

/* Colour palette — index 0–8 correspond to keys 1–9 */
#define NUM_COLORS 9
static struct {
    unsigned short r, g, b;       /* X colour (0–0xFFFF)            */
    unsigned char  ar, ag, ab;    /* 0–255 for ARGB cursor          */
} color_table[NUM_COLORS] = {
    /* 1 */ { 0x0000, 0x0000, 0x0000,   0,   0,   0 },   /* black   */
    /* 2 */ { 0xFFFF, 0x0000, 0x0000, 255,   0,   0 },   /* red     */
    /* 3 */ { 0x0000, 0xC000, 0x0000,   0, 192,   0 },   /* green   */
    /* 4 */ { 0x0000, 0x5555, 0xFFFF,   0,  85, 255 },   /* blue    */
    /* 5 */ { 0xFFFF, 0xA500, 0x0000, 255, 165,   0 },   /* orange  */
    /* 6 */ { 0xFFFF, 0xFFFF, 0x0000, 255, 255,   0 },   /* yellow  */
    /* 7 */ { 0x8000, 0x0000, 0x8000, 128,   0, 128 },   /* purple  */
    /* 8 */ { 0x0000, 0xFFFF, 0xFFFF,   0, 255, 255 },   /* cyan    */
    /* 9 */ { 0xFFFF, 0xFFFF, 0xFFFF, 255, 255, 255 },   /* white   */
};
static int           cur_color = 1;   /* default = red (index 1)    */
static unsigned long px_colors[NUM_COLORS];

/* Persistent annotation layer: 0 = empty, 1–NUM_COLORS = palette entry + 1 */
static uint8_t *overlay_map;

typedef struct {
    Pixmap   pixmap;
    uint8_t *overlay;
} CanvasSnapshot;

/* Undo / redo stacks (canvas snapshots + overlay state) */
static CanvasSnapshot undo_stack[MAX_UNDO];
static int            undo_count;
static CanvasSnapshot redo_stack[MAX_UNDO];
static int            redo_count;

/* Cached circular brush mask shared by draw / erase / save-load */
static Pixmap   erase_clip;
static int      erase_clip_r = -1;  /* radius the clip was built for */
static uint8_t *brush_mask;
static int      brush_mask_r = -1;
static int      brush_mask_d;

/* ARGB cursor overlay */
static Visual  *argb_visual;
static Colormap argb_cmap;
static Window   cursor_win;
static GC       gc_cursor;
static int      has_cursor;

/* Cursor-centred save / load feedback */
static XFontStruct *cursor_feedback_font;
static int         cursor_feedback_slot;
static unsigned long cursor_feedback_pixel;
static long long   cursor_feedback_until_ms;

static volatile sig_atomic_t running = 1;
static Window saved_focus = None;
static int    saved_focus_revert = RevertToPointerRoot;

#define FOCUS_TIMEOUT_US 1000000
#define FOCUS_RETRY_US      5000
#define FOCUS_SETTLE_US    50000

/* ── Signal handler ───────────────────────────────────────────── */
static void on_sigint(int sig) { (void)sig; running = 0; }

static void resize_cursor(void);

static int
ignore_xerror(Display *unused_dpy, XErrorEvent *unused_err)
{
    (void)unused_dpy;
    (void)unused_err;
    return 0;
}

static int
focus_window(Window target, int revert_to)
{
    Window focused;
    int revert;
    useconds_t waited = 0;

    while (waited <= FOCUS_TIMEOUT_US) {
        XSetInputFocus(dpy, target, revert_to, CurrentTime);
        XSync(dpy, False);
        XGetInputFocus(dpy, &focused, &revert);
        if (focused == target)
            return 1;
        usleep(FOCUS_RETRY_US);
        waited += FOCUS_RETRY_US;
    }

    return 0;
}

static int
focus_should_return_to_draw(void)
{
    Window focused;
    int revert;
    XWindowAttributes attr;
    int (*old_handler)(Display *, XErrorEvent *);
    Status ok;

    XGetInputFocus(dpy, &focused, &revert);
    if (focused == win)
        return 0;
    if (focused == None || focused == PointerRoot)
        return 1;
    if (has_cursor && focused == cursor_win)
        return 0;

    old_handler = XSetErrorHandler(ignore_xerror);
    ok = XGetWindowAttributes(dpy, focused, &attr);
    XSync(dpy, False);
    XSetErrorHandler(old_handler);
    if (!ok)
        return 1;

    /*
     * If another override-redirect or input-only helper currently owns focus
     * (for example another overlay tool), do not steal it back.
     */
    if (attr.override_redirect || attr.class == InputOnly)
        return 0;

    return 1;
}

static void
reclaim_focus_if_needed(void)
{
    usleep(FOCUS_SETTLE_US);
    if (focus_should_return_to_draw())
        focus_window(win, RevertToPointerRoot);
}

/*
 * Draw explicitly focuses its fullscreen override-redirect window so ESC and
 * the 1–9 palette shortcuts work without a keyboard grab. Remember the window
 * that was focused before Draw started and restore it once Draw exits so the
 * previously active client does not get stranded at PointerRoot.
 */
static void
restore_saved_focus(void)
{
    int (*old_handler)(Display *, XErrorEvent *);

    if (!saved_focus || saved_focus == None || saved_focus == PointerRoot ||
        saved_focus == win || (has_cursor && saved_focus == cursor_win))
        return;

    old_handler = XSetErrorHandler(ignore_xerror);
    focus_window(saved_focus, saved_focus_revert);
    XSetErrorHandler(old_handler);
}

/* ── Colour helpers ───────────────────────────────────────────── */
static void alloc_colors(void)
{
    Colormap cmap = DefaultColormap(dpy, screen_num);
    for (int i = 0; i < NUM_COLORS; i++) {
        XColor c;
        c.red   = color_table[i].r;
        c.green = color_table[i].g;
        c.blue  = color_table[i].b;
        c.flags = DoRed | DoGreen | DoBlue;
        XAllocColor(dpy, cmap, &c);
        px_colors[i] = c.pixel;
    }
}

static void set_color(int idx)
{
    cur_color = idx;
    XSetForeground(dpy, gc_draw, px_colors[idx]);
    if (has_cursor && !erase_mode)
        resize_cursor();          /* repaint cursor in new colour */
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static int cursor_feedback_active(void)
{
    return cursor_feedback_slot != 0
        && now_ms() < cursor_feedback_until_ms;
}

static unsigned long argb_pixel(unsigned char r, unsigned char g, unsigned char b)
{
    return ((unsigned long)0xFF << 24)
         | ((unsigned long)r << 16)
         | ((unsigned long)g << 8)
         |  (unsigned long)b;
}

static void show_cursor_feedback(int slot, unsigned long pixel)
{
    if (!has_cursor || !cursor_feedback_font)
        return;

    cursor_feedback_slot = slot;
    cursor_feedback_pixel = pixel;
    cursor_feedback_until_ms = now_ms() + CURSOR_FEEDBACK_MS;
    resize_cursor();
}

static void clear_cursor_feedback_if_needed(void)
{
    if (cursor_feedback_slot == 0 || cursor_feedback_active())
        return;

    cursor_feedback_slot = 0;
    resize_cursor();
}

static size_t overlay_size(void)
{
    return (size_t)scr_w * (size_t)scr_h;
}

static int snapshot_valid(const CanvasSnapshot *snap)
{
    return snap->pixmap != None && snap->overlay != NULL;
}

static void free_snapshot(CanvasSnapshot *snap)
{
    if (snap->pixmap != None)
        XFreePixmap(dpy, snap->pixmap);
    free(snap->overlay);
    snap->pixmap = None;
    snap->overlay = NULL;
}

static CanvasSnapshot snapshot_canvas(void)
{
    CanvasSnapshot snap = {0};
    size_t bytes = overlay_size();
    int depth = DefaultDepth(dpy, screen_num);

    snap.pixmap = XCreatePixmap(dpy, win, scr_w, scr_h, depth);
    if (snap.pixmap != None)
        XCopyArea(dpy, canvas, snap.pixmap, gc_copy, 0, 0, scr_w, scr_h, 0, 0);

    snap.overlay = malloc(bytes);
    if (!snap.overlay) {
        free_snapshot(&snap);
        return (CanvasSnapshot){0};
    }

    memcpy(snap.overlay, overlay_map, bytes);
    return snap;
}

static void push_stack(CanvasSnapshot *stack, int *count, CanvasSnapshot *snap)
{
    if (!snapshot_valid(snap)) {
        free_snapshot(snap);
        return;
    }

    if (*count >= MAX_UNDO) {
        free_snapshot(&stack[0]);
        memmove(&stack[0], &stack[1],
                (size_t)(MAX_UNDO - 1) * sizeof(*stack));
        (*count)--;
    }

    stack[(*count)++] = *snap;
    *snap = (CanvasSnapshot){0};
}

static void clear_stack(CanvasSnapshot *stack, int *count)
{
    for (int i = 0; i < *count; i++)
        free_snapshot(&stack[i]);
    *count = 0;
}

static void restore_snapshot(const CanvasSnapshot *snap)
{
    if (!snapshot_valid(snap))
        return;

    XCopyArea(dpy, snap->pixmap, canvas, gc_copy,
              0, 0, scr_w, scr_h, 0, 0);
    memcpy(overlay_map, snap->overlay, overlay_size());
}

static int slot_from_keysym(KeySym ks)
{
    switch (ks) {
    case XK_1:
    case XK_KP_1:
        return 1;
    case XK_2:
    case XK_KP_2:
        return 2;
    case XK_3:
    case XK_KP_3:
        return 3;
    case XK_4:
    case XK_KP_4:
        return 4;
    case XK_5:
    case XK_KP_5:
        return 5;
    case XK_6:
    case XK_KP_6:
        return 6;
    case XK_7:
    case XK_KP_7:
        return 7;
    case XK_8:
    case XK_KP_8:
        return 8;
    case XK_9:
    case XK_KP_9:
        return 9;
    default:
        return 0;
    }
}

static char *build_data_path(const char *suffix)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    int needed;
    char *path;

    if (xdg && xdg[0] != '\0')
        needed = snprintf(NULL, 0, "%s/%s", xdg, suffix);
    else if (home && home[0] != '\0')
        needed = snprintf(NULL, 0, "%s/.local/share/%s", home, suffix);
    else
        return NULL;

    if (needed < 0)
        return NULL;

    path = malloc((size_t)needed + 1);
    if (!path)
        return NULL;

    if (xdg && xdg[0] != '\0')
        snprintf(path, (size_t)needed + 1, "%s/%s", xdg, suffix);
    else
        snprintf(path, (size_t)needed + 1, "%s/.local/share/%s", home, suffix);

    return path;
}

static int ensure_dir(const char *path)
{
    struct stat st;

    if (mkdir(path, 0700) == 0)
        return 1;
    if (errno != EEXIST) {
        fprintf(stderr, "draw: failed to create %s: %s\n",
                path, strerror(errno));
        return 0;
    }
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;

    fprintf(stderr, "draw: %s exists but is not a directory\n", path);
    return 0;
}

static int ensure_slot_dir(void)
{
    char *draw_dir = build_data_path("draw");
    char *slots_dir = build_data_path("draw/slots");
    int ok = 0;

    if (!draw_dir || !slots_dir) {
        fprintf(stderr, "draw: failed to resolve data directory\n");
        goto out;
    }

    if (!ensure_dir(draw_dir) || !ensure_dir(slots_dir))
        goto out;

    ok = 1;
out:
    free(draw_dir);
    free(slots_dir);
    return ok;
}

static char *slot_file_path(int slot)
{
    char rel[64];
    snprintf(rel, sizeof rel, "draw/slots/slot-%d.drawslot", slot);
    return build_data_path(rel);
}

static int write_u32_le(FILE *fp, uint32_t value)
{
    unsigned char buf[4] = {
        (unsigned char)(value & 0xFFu),
        (unsigned char)((value >> 8) & 0xFFu),
        (unsigned char)((value >> 16) & 0xFFu),
        (unsigned char)((value >> 24) & 0xFFu),
    };
    return fwrite(buf, 1, sizeof buf, fp) == sizeof buf;
}

static int read_u32_le(FILE *fp, uint32_t *value)
{
    unsigned char buf[4];

    if (fread(buf, 1, sizeof buf, fp) != sizeof buf)
        return 0;

    *value = (uint32_t)buf[0]
           | ((uint32_t)buf[1] << 8)
           | ((uint32_t)buf[2] << 16)
           | ((uint32_t)buf[3] << 24);
    return 1;
}

static int rebuild_canvas_from_overlay(void)
{
    XImage *img = XGetImage(dpy, original, 0, 0, scr_w, scr_h,
                            AllPlanes, ZPixmap);
    if (!img) {
        fprintf(stderr, "draw: failed to rebuild canvas from overlay\n");
        return 0;
    }

    for (int y = 0; y < scr_h; y++) {
        size_t row = (size_t)y * (size_t)scr_w;
        for (int x = 0; x < scr_w; x++) {
            uint8_t px = overlay_map[row + (size_t)x];
            if (px != 0)
                XPutPixel(img, x, y, px_colors[px - 1]);
        }
    }

    XPutImage(dpy, canvas, gc_copy, img, 0, 0, 0, 0, scr_w, scr_h);
    XDestroyImage(img);
    return 1;
}

static int save_slot(int slot)
{
    static const unsigned char magic[8] = { 'D', 'R', 'W', 'S', 'L', 'T', '0', '1' };
    char *path = NULL;
    FILE *fp = NULL;
    size_t bytes = overlay_size();
    int ok = 0;

    if (!ensure_slot_dir())
        goto out;

    path = slot_file_path(slot);
    if (!path) {
        fprintf(stderr, "draw: failed to build slot path\n");
        goto out;
    }

    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "draw: failed to open %s for writing: %s\n",
                path, strerror(errno));
        goto out;
    }

    if (fwrite(magic, 1, sizeof magic, fp) != sizeof magic
        || !write_u32_le(fp, SLOT_FILE_VERSION)
        || !write_u32_le(fp, (uint32_t)scr_w)
        || !write_u32_le(fp, (uint32_t)scr_h)
        || fwrite(overlay_map, 1, bytes, fp) != bytes) {
        fprintf(stderr, "draw: failed to write %s\n", path);
        goto out;
    }

    if (fclose(fp) != 0) {
        fp = NULL;
        fprintf(stderr, "draw: failed to close %s: %s\n",
                path, strerror(errno));
        goto out;
    }
    fp = NULL;

    fprintf(stderr, "draw: saved slot %d to %s\n", slot, path);
    ok = 1;

out:
    if (fp)
        fclose(fp);
    if (!ok && path)
        unlink(path);
    free(path);
    show_cursor_feedback(slot, ok ? argb_pixel(0, 255, 0)
                                  : argb_pixel(255, 64, 64));
    return ok;
}

static int load_slot(int slot)
{
    static const unsigned char magic[8] = { 'D', 'R', 'W', 'S', 'L', 'T', '0', '1' };
    char *path = NULL;
    FILE *fp = NULL;
    unsigned char file_magic[sizeof magic];
    uint32_t version, width, height;
    size_t dst_bytes = overlay_size();
    size_t src_bytes;
    size_t copy_w, copy_h;
    uint8_t *loaded = NULL;
    uint8_t *src = NULL;
    uint8_t *old_overlay = NULL;
    CanvasSnapshot before = {0};
    int ok = 0;

    path = slot_file_path(slot);
    if (!path) {
        fprintf(stderr, "draw: failed to build slot path\n");
        goto out;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "draw: failed to open %s: %s\n",
                path, strerror(errno));
        goto out;
    }

    if (fread(file_magic, 1, sizeof file_magic, fp) != sizeof file_magic
        || memcmp(file_magic, magic, sizeof magic) != 0
        || !read_u32_le(fp, &version)
        || !read_u32_le(fp, &width)
        || !read_u32_le(fp, &height)) {
        fprintf(stderr, "draw: invalid slot file %s\n", path);
        goto out;
    }

    if (version != SLOT_FILE_VERSION || width == 0 || height == 0) {
        fprintf(stderr, "draw: unsupported slot file %s\n", path);
        goto out;
    }
    if ((size_t)width > SIZE_MAX / (size_t)height) {
        fprintf(stderr, "draw: slot file %s is too large\n", path);
        goto out;
    }

    src_bytes = (size_t)width * (size_t)height;
    src = malloc(src_bytes);
    loaded = calloc(dst_bytes, 1);
    old_overlay = malloc(dst_bytes);
    if (!src || !loaded || !old_overlay) {
        fprintf(stderr, "draw: out of memory loading slot %d\n", slot);
        goto out;
    }

    if (fread(src, 1, src_bytes, fp) != src_bytes) {
        fprintf(stderr, "draw: failed to read %s\n", path);
        goto out;
    }

    copy_w = (size_t)width < (size_t)scr_w ? (size_t)width : (size_t)scr_w;
    copy_h = (size_t)height < (size_t)scr_h ? (size_t)height : (size_t)scr_h;
    for (size_t y = 0; y < copy_h; y++)
        memcpy(&loaded[y * (size_t)scr_w], &src[y * (size_t)width], copy_w);

    memcpy(old_overlay, overlay_map, dst_bytes);
    before = snapshot_canvas();
    memcpy(overlay_map, loaded, dst_bytes);
    if (!rebuild_canvas_from_overlay()) {
        memcpy(overlay_map, old_overlay, dst_bytes);
        goto out;
    }

    push_stack(undo_stack, &undo_count, &before);
    clear_stack(redo_stack, &redo_count);

    XCopyArea(dpy, canvas, win, gc_copy, 0, 0, scr_w, scr_h, 0, 0);
    XFlush(dpy);
    fprintf(stderr, "draw: loaded slot %d from %s\n", slot, path);
    ok = 1;

out:
    if (fp)
        fclose(fp);
    free_snapshot(&before);
    free(old_overlay);
    free(src);
    free(loaded);
    free(path);
    show_cursor_feedback(slot, ok ? argb_pixel(0, 255, 255)
                               : argb_pixel(255, 64, 64));
    return ok;
}

/* ── Find a 32-bit ARGB TrueColor visual ─────────────────────── */
static Visual *find_argb_visual(void)
{
    XVisualInfo tpl;
    memset(&tpl, 0, sizeof tpl);
    tpl.screen = screen_num;
    tpl.depth  = 32;
    tpl.class  = TrueColor;

    int count = 0;
    XVisualInfo *info = XGetVisualInfo(
        dpy, VisualScreenMask | VisualDepthMask | VisualClassMask,
        &tpl, &count);
    if (!info || count == 0) return NULL;
    Visual *v = info[0].visual;
    XFree(info);
    return v;
}

/* ─────────────────────────────────────────────────────────────── *
 *  Cursor rendering                                               *
 *                                                                  *
 *  Two modes, both anti-aliased with premultiplied ARGB:          *
 *                                                                  *
 *    draw  — solid red filled circle                              *
 *    erase — transparent interior + white ring border             *
 * ─────────────────────────────────────────────────────────────── */

static int feedback_cursor_size(void)
{
    char text[2] = { (char)('0' + cursor_feedback_slot), '\0' };
    int text_w;
    int text_h;
    int d;

    if (!cursor_feedback_active() || !cursor_feedback_font)
        return 0;

    text_w = XTextWidth(cursor_feedback_font, text, 1);
    text_h = cursor_feedback_font->ascent + cursor_feedback_font->descent;
    d = text_w > text_h ? text_w : text_h;
    return d + CURSOR_FEEDBACK_PAD * 2;
}

static int cursor_diameter(void)
{
    int feedback_d = feedback_cursor_size();

    if (feedback_d > 0)
        return feedback_d;

    {
        int border = erase_mode ? ERASE_BORDER : 0;
        return (brush_radius + border) * 2 + 2;        /* +2 AA fringe */
    }
}

static void render_cursor(void)
{
    int d = cursor_diameter();

    if (cursor_feedback_active() && cursor_feedback_font) {
        char text[2] = { (char)('0' + cursor_feedback_slot), '\0' };
        int text_w = XTextWidth(cursor_feedback_font, text, 1);
        int text_h = cursor_feedback_font->ascent + cursor_feedback_font->descent;
        int x = (d - text_w) / 2;
        int y = (d - text_h) / 2 + cursor_feedback_font->ascent;

        XSetForeground(dpy, gc_cursor, 0);
        XFillRectangle(dpy, cursor_win, gc_cursor, 0, 0,
                       (unsigned)d, (unsigned)d);
        XSetFont(dpy, gc_cursor, cursor_feedback_font->fid);
        XSetForeground(dpy, gc_cursor, argb_pixel(0, 0, 0));
        XDrawString(dpy, cursor_win, gc_cursor, x - 1, y, text, 1);
        XDrawString(dpy, cursor_win, gc_cursor, x + 1, y, text, 1);
        XDrawString(dpy, cursor_win, gc_cursor, x, y - 1, text, 1);
        XDrawString(dpy, cursor_win, gc_cursor, x, y + 1, text, 1);
        XSetForeground(dpy, gc_cursor, cursor_feedback_pixel);
        XDrawString(dpy, cursor_win, gc_cursor, x, y, text, 1);
        return;
    }

    {
        float center = d * 0.5f;
        XImage *img = XCreateImage(dpy, argb_visual, 32, ZPixmap,
                                   0, NULL, d, d, 32, 0);
        img->data = calloc((size_t)img->bytes_per_line * d, 1);

        if (erase_mode) {
            /* transparent inside, anti-aliased white ring outside */
            float r_inner = (float)brush_radius;
            float r_outer = (float)(brush_radius + ERASE_BORDER);

            for (int y = 0; y < d; y++) {
                float dy  = y + 0.5f - center;
                float dy2 = dy * dy;
                for (int x = 0; x < d; x++) {
                    float dx   = x + 0.5f - center;
                    float dist = sqrtf(dx * dx + dy2);

                    float oc = r_outer - dist + 0.5f;
                    if (oc <= 0.0f) continue;
                    if (oc > 1.0f)  oc = 1.0f;

                    float ic = r_inner - dist + 0.5f;
                    if (ic < 0.0f) ic = 0.0f;
                    if (ic > 1.0f) ic = 1.0f;

                    float ring = oc - ic;
                    if (ring <= 0.0f) continue;

                    /* premultiplied white: A=R=G=B */
                    unsigned a = (unsigned)(ring * 255.0f + 0.5f);
                    XPutPixel(img, x, y,
                              ((unsigned long)a << 24) |
                              ((unsigned long)a << 16) |
                              ((unsigned long)a << 8)  |
                               (unsigned long)a);
                }
            }
        } else {
            /* solid filled circle in the current colour */
            float r = (float)brush_radius;
            unsigned char cr = color_table[cur_color].ar;
            unsigned char cg = color_table[cur_color].ag;
            unsigned char cb = color_table[cur_color].ab;

            for (int y = 0; y < d; y++) {
                float dy  = y + 0.5f - center;
                float dy2 = dy * dy;
                for (int x = 0; x < d; x++) {
                    float dx   = x + 0.5f - center;
                    float dist = sqrtf(dx * dx + dy2);

                    float cov = r - dist + 0.5f;
                    if (cov <= 0.0f) continue;
                    if (cov > 1.0f)  cov = 1.0f;

                    /* premultiplied ARGB */
                    unsigned a  = (unsigned)(cov * 255.0f + 0.5f);
                    unsigned rr = (unsigned)(cov * cr + 0.5f);
                    unsigned gg = (unsigned)(cov * cg + 0.5f);
                    unsigned bb = (unsigned)(cov * cb + 0.5f);
                    XPutPixel(img, x, y,
                              ((unsigned long)a  << 24) |
                              ((unsigned long)rr << 16) |
                              ((unsigned long)gg << 8)  |
                               (unsigned long)bb);
                }
            }
        }

        XPutImage(dpy, cursor_win, gc_cursor, img, 0, 0, 0, 0, d, d);
        XDestroyImage(img);
    }
}

static void resize_cursor(void)
{
    int d    = cursor_diameter();
    int half = d / 2;
    XMoveResizeWindow(dpy, cursor_win,
                      cur_x - half, cur_y - half, d, d);
    render_cursor();
    XRaiseWindow(dpy, cursor_win);
    XFlush(dpy);
}

static void move_cursor(int x, int y)
{
    int half = cursor_diameter() / 2;
    XMoveWindow(dpy, cursor_win, x - half, y - half);
}

/* ── Blank (invisible) X cursor ───────────────────────────────── */
static Cursor make_blank_cursor(void)
{
    static char data = 0;
    Pixmap p = XCreateBitmapFromData(dpy, win, &data, 1, 1);
    XColor black = {0};
    Cursor c = XCreatePixmapCursor(dpy, p, p, &black, &black, 0, 0);
    XFreePixmap(dpy, p);
    return c;
}

/* ── Drawing helpers ──────────────────────────────────────────── */
static int update_brush_cache(void)
{
    XImage *mask_img;
    GC mgc;
    int d;

    if (erase_clip_r == brush_radius && brush_mask_r == brush_radius)
        return 1;

    if (erase_clip_r >= 0)
        XFreePixmap(dpy, erase_clip);
    erase_clip_r = -1;

    free(brush_mask);
    brush_mask = NULL;
    brush_mask_r = -1;
    brush_mask_d = 0;

    d = brush_radius * 2;
    erase_clip = XCreatePixmap(dpy, canvas, d, d, 1);
    mgc = XCreateGC(dpy, erase_clip, 0, NULL);
    XSetForeground(dpy, mgc, 0);
    XFillRectangle(dpy, erase_clip, mgc, 0, 0, d, d);
    XSetForeground(dpy, mgc, 1);
    XFillArc(dpy, erase_clip, mgc, 0, 0, d, d, 0, 360 * 64);
    XFreeGC(dpy, mgc);

    XSetClipMask(dpy, gc_erase, erase_clip);
    erase_clip_r = brush_radius;

    mask_img = XGetImage(dpy, erase_clip, 0, 0, d, d, AllPlanes, ZPixmap);
    if (!mask_img) {
        fprintf(stderr, "draw: failed to build brush mask\n");
        return 0;
    }

    brush_mask = malloc((size_t)d * (size_t)d);
    if (!brush_mask) {
        fprintf(stderr, "draw: out of memory building brush mask\n");
        XDestroyImage(mask_img);
        return 0;
    }

    for (int my = 0; my < d; my++) {
        size_t row = (size_t)my * (size_t)d;
        for (int mx = 0; mx < d; mx++)
            brush_mask[row + (size_t)mx] = XGetPixel(mask_img, mx, my) ? 1u : 0u;
    }

    XDestroyImage(mask_img);
    brush_mask_r = brush_radius;
    brush_mask_d = d;
    return 1;
}

static void overlay_paint_circle(int x, int y, uint8_t value)
{
    if (!update_brush_cache()) {
        int r = brush_radius;
        int min_x = x - r;
        int max_x = x + r;
        int min_y = y - r;
        int max_y = y + r;
        int rr = r * r;

        if (min_x < 0) min_x = 0;
        if (min_y < 0) min_y = 0;
        if (max_x >= scr_w) max_x = scr_w - 1;
        if (max_y >= scr_h) max_y = scr_h - 1;

        for (int py = min_y; py <= max_y; py++) {
            int dy = py - y;
            size_t row = (size_t)py * (size_t)scr_w;
            for (int px = min_x; px <= max_x; px++) {
                int dx = px - x;
                if (dx * dx + dy * dy <= rr)
                    overlay_map[row + (size_t)px] = value;
            }
        }
        return;
    }

    {
        int ox = x - brush_radius;
        int oy = y - brush_radius;
        int d = brush_mask_d;

        for (int my = 0; my < d; my++) {
            int py = oy + my;
            size_t src_row = (size_t)my * (size_t)d;
            size_t dst_row;

            if (py < 0 || py >= scr_h)
                continue;
            dst_row = (size_t)py * (size_t)scr_w;

            for (int mx = 0; mx < d; mx++) {
                int px = ox + mx;
                if (px < 0 || px >= scr_w)
                    continue;
                if (brush_mask[src_row + (size_t)mx] != 0)
                    overlay_map[dst_row + (size_t)px] = value;
            }
        }
    }
}

static void brush_stamp(int x, int y)
{
    overlay_paint_circle(x, y, (uint8_t)(cur_color + 1));
    XFillArc(dpy, canvas, gc_draw,
             x - brush_radius, y - brush_radius,
             brush_radius * 2, brush_radius * 2,
             0, 360 * 64);
}

static void brush_stroke(int x0, int y0, int x1, int y1)
{
    float dx   = (float)(x1 - x0);
    float dy   = (float)(y1 - y0);
    float dist = sqrtf(dx * dx + dy * dy);
    int   steps = (int)dist + 1;
    for (int i = 0; i <= steps; i++) {
        float t = steps > 0 ? (float)i / steps : 0.0f;
        brush_stamp(x0 + (int)(dx * t), y0 + (int)(dy * t));
    }
}

/* ── Eraser helpers ───────────────────────────────────────────── *
 *  Copies from the pristine `original` pixmap back onto `canvas`  *
 *  through a circular clip mask so the erased area is round.      */

static void update_erase_clip(void)
{
    (void)update_brush_cache();
}

static void erase_stamp(int x, int y)
{
    overlay_paint_circle(x, y, 0);
    update_erase_clip();
    int d = brush_radius * 2;
    int ox = x - brush_radius;
    int oy = y - brush_radius;
    XSetClipOrigin(dpy, gc_erase, ox, oy);
    XCopyArea(dpy, original, canvas, gc_erase,
              ox, oy, d, d, ox, oy);
}

static void erase_stroke(int x0, int y0, int x1, int y1)
{
    float dx   = (float)(x1 - x0);
    float dy   = (float)(y1 - y0);
    float dist = sqrtf(dx * dx + dy * dy);
    int   steps = (int)dist + 1;
    for (int i = 0; i <= steps; i++) {
        float t = steps > 0 ? (float)i / steps : 0.0f;
        erase_stamp(x0 + (int)(dx * t), y0 + (int)(dy * t));
    }
}

/* ── Undo / redo ──────────────────────────────────────────────── */

static void do_undo(void)
{
    CanvasSnapshot current;

    if (undo_count == 0) return;
    current = snapshot_canvas();
    push_stack(redo_stack, &redo_count, &current);
    undo_count--;
    restore_snapshot(&undo_stack[undo_count]);
    free_snapshot(&undo_stack[undo_count]);
    XCopyArea(dpy, canvas, win, gc_copy, 0, 0, scr_w, scr_h, 0, 0);
    XFlush(dpy);
}

static void do_redo(void)
{
    CanvasSnapshot current;

    if (redo_count == 0) return;
    current = snapshot_canvas();
    push_stack(undo_stack, &undo_count, &current);
    redo_count--;
    restore_snapshot(&redo_stack[redo_count]);
    free_snapshot(&redo_stack[redo_count]);
    XCopyArea(dpy, canvas, win, gc_copy, 0, 0, scr_w, scr_h, 0, 0);
    XFlush(dpy);
}

/* ── Blit a clamped rectangle from canvas to the window ───────── */
static void blit_rect(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (w > 0 && h > 0)
        XCopyArea(dpy, canvas, win, gc_copy, x, y, w, h, x, y);
}

/* ── Main ─────────────────────────────────────────────────────── */
int main(void)
{
    struct sigaction sa = { .sa_handler = on_sigint };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "error: cannot open X display\n");
        return 1;
    }

    screen_num = DefaultScreen(dpy);
    root       = RootWindow(dpy, screen_num);
    scr_w      = DisplayWidth(dpy, screen_num);
    scr_h      = DisplayHeight(dpy, screen_num);
    alloc_colors();

    overlay_map = calloc(overlay_size(), 1);
    if (!overlay_map) {
        fprintf(stderr, "error: failed to allocate drawing layer\n");
        XCloseDisplay(dpy);
        return 1;
    }

    /* ── Capture screenshot ─────────────────────────────────── */
    XImage *shot = XGetImage(dpy, root, 0, 0, scr_w, scr_h,
                             AllPlanes, ZPixmap);
    if (!shot) {
        fprintf(stderr, "error: failed to capture screenshot\n");
        XCloseDisplay(dpy);
        return 1;
    }

    /* ── ARGB cursor overlay (off-screen) ───────────────────── */
    argb_visual = find_argb_visual();
    if (argb_visual) {
        argb_cmap = XCreateColormap(dpy, root, argb_visual, AllocNone);
        int d = cursor_diameter();
        XSetWindowAttributes cwa = {
            .override_redirect = True,
            .colormap          = argb_cmap,
            .background_pixel  = 0,
            .border_pixel      = 0,
        };
        cursor_win = XCreateWindow(
            dpy, root, -10000, -10000, d, d, 0,
            32, InputOutput, argb_visual,
            CWOverrideRedirect | CWColormap |
            CWBackPixel        | CWBorderPixel,
            &cwa);
        /* Make the visual cursor overlay click-through. */
        XShapeCombineRectangles(dpy, cursor_win, ShapeInput,
                                0, 0, NULL, 0, ShapeSet, Unsorted);
        gc_cursor = XCreateGC(dpy, cursor_win, 0, NULL);
        cursor_feedback_font = XLoadQueryFont(dpy, "10x20");
        if (!cursor_feedback_font)
            cursor_feedback_font = XLoadQueryFont(dpy, "fixed");
        XClassHint cch = { .res_name = "draw", .res_class = "draw" };
        XSetClassHint(dpy, cursor_win, &cch);
        has_cursor = 1;
    } else {
        fprintf(stderr,
                "warning: no 32-bit ARGB visual — "
                "cursor indicator disabled\n");
    }

    /* ── Fullscreen window ──────────────────────────────────── */
    XSetWindowAttributes wa = {
        .override_redirect = True,
        .event_mask = ExposureMask      | KeyPressMask       |
                      ButtonPressMask   | ButtonReleaseMask  |
                      PointerMotionMask | StructureNotifyMask |
                      FocusChangeMask,
    };
    win = XCreateWindow(dpy, root, 0, 0, scr_w, scr_h, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWEventMask, &wa);
    XClassHint ch = { .res_name = "draw", .res_class = "draw" };
    XSetClassHint(dpy, win, &ch);

    /* ── Canvas + pristine original ─────────────────────────── */
    int depth = DefaultDepth(dpy, screen_num);
    canvas    = XCreatePixmap(dpy, win, scr_w, scr_h, depth);
    original  = XCreatePixmap(dpy, win, scr_w, scr_h, depth);
    gc_copy   = XCreateGC(dpy, canvas, 0, NULL);
    XPutImage(dpy, canvas,   gc_copy, shot, 0, 0, 0, 0, scr_w, scr_h);
    XPutImage(dpy, original, gc_copy, shot, 0, 0, 0, 0, scr_w, scr_h);
    XDestroyImage(shot);

    /* ── GCs for drawing and erasing ────────────────────────── */
    gc_draw  = XCreateGC(dpy, canvas, 0, NULL);
    XSetForeground(dpy, gc_draw, px_colors[cur_color]);
    gc_erase = XCreateGC(dpy, canvas, 0, NULL);

    /* ── Hide the real cursor ───────────────────────────────── */
    Cursor blank = make_blank_cursor();
    XDefineCursor(dpy, win, blank);

    /* ── Map windows ────────────────────────────────────────── */
    if (has_cursor) {
        XMapWindow(dpy, cursor_win);
        render_cursor();
    }
    XMapRaised(dpy, win);
    if (has_cursor)
        XRaiseWindow(dpy, cursor_win);

    /* ── Take focus, but avoid active grabs ─────────────────── */
    /*
     * Draw already sits in a fullscreen override-redirect window, so it can
     * receive pointer events without grabbing the pointer. Likewise, setting
     * input focus is enough for its plain key controls (Esc, 1–9) while still
     * letting dwm's passive global hotkeys fire. This keeps Draw compatible
     * with other tools such as dmenu and maim/slop.
     *
     * Save the previously focused window first so we can restore it on exit.
     */
    XGetInputFocus(dpy, &saved_focus, &saved_focus_revert);
    focus_window(win, RevertToPointerRoot);

    /* ── Initial cursor placement ───────────────────────────── */
    {
        Window wr, wc;
        int rx, ry, wx, wy;
        unsigned int mask;
        if (XQueryPointer(dpy, win, &wr, &wc, &rx, &ry,
                          &wx, &wy, &mask)) {
            cur_x = wx;
            cur_y = wy;
            if (has_cursor) move_cursor(cur_x, cur_y);
        }
    }
    XFlush(dpy);

    /* ── Event loop ─────────────────────────────────────────── */
    int xfd = ConnectionNumber(dpy);

    while (running) {
        clear_cursor_feedback_if_needed();

        while (XPending(dpy) > 0 && running) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            switch (ev.type) {

            case Expose:
                XCopyArea(dpy, canvas, win, gc_copy,
                          0, 0, scr_w, scr_h, 0, 0);
                break;

            case KeyPress: {
                unsigned int mods = ev.xkey.state;
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                int slot = slot_from_keysym(ks);

                if (ks == XK_Escape || ((mods & ControlMask) && ks == XK_c)) {
                    running = 0;
                } else if ((mods & ControlMask) && slot != 0) {
                    if (mods & ShiftMask)
                        load_slot(slot);
                    else
                        save_slot(slot);
                } else if (!(mods & ControlMask) && slot != 0) {
                    set_color(slot - 1);
                }
                break;
            }

            case ButtonPress:
                switch (ev.xbutton.button) {
                case 1: /* left = draw or erase depending on mode */
                    stroking = 1;
                    prev_x   = ev.xbutton.x;
                    prev_y   = ev.xbutton.y;
                    {
                        CanvasSnapshot before = snapshot_canvas();
                        push_stack(undo_stack, &undo_count, &before);
                    }
                    clear_stack(redo_stack, &redo_count);
                    if (erase_mode) {
                        erase_stamp(prev_x, prev_y);
                    } else {
                        brush_stamp(prev_x, prev_y);
                    }
                    {
                        int r = brush_radius + 1;
                        blit_rect(prev_x - r, prev_y - r,
                                  r * 2 + 1, r * 2 + 1);
                    }
                    break;
                case 3: /* right = enter erase mode */
                    erase_mode = 1;
                    prev_x = prev_y = -1;  /* reset mid-stroke */
                    if (has_cursor) resize_cursor();
                    break;
                case 4: /* scroll up */
                    if (brush_radius < BRUSH_MAX) {
                        brush_radius += 2;
                        if (brush_radius > BRUSH_MAX)
                            brush_radius = BRUSH_MAX;
                        if (has_cursor) resize_cursor();
                    }
                    break;
                case 5: /* scroll down */
                    if (brush_radius > BRUSH_MIN) {
                        brush_radius -= 2;
                        if (brush_radius < BRUSH_MIN)
                            brush_radius = BRUSH_MIN;
                        if (has_cursor) resize_cursor();
                    }
                    break;
                case 8: /* side button (back) = redo */
                    if (!stroking) do_redo();
                    break;
                case 9: /* side button (up) = undo */
                    if (!stroking) do_undo();
                    break;
                }
                break;

            case ButtonRelease:
                if (ev.xbutton.button == 1) {
                    stroking = 0;
                    prev_x   = -1;
                    prev_y   = -1;
                } else if (ev.xbutton.button == 3) {
                    erase_mode = 0;
                    prev_x = prev_y = -1;
                    if (has_cursor) resize_cursor();
                }
                break;

            case FocusOut:
                reclaim_focus_if_needed();
                break;

            case MotionNotify: {
                while (XCheckTypedEvent(dpy, MotionNotify, &ev))
                    ;
                int mx = ev.xmotion.x;
                int my = ev.xmotion.y;
                cur_x  = mx;
                cur_y  = my;

                if (stroking) {
                    if (erase_mode) {
                        if (prev_x >= 0)
                            erase_stroke(prev_x, prev_y, mx, my);
                        else
                            erase_stamp(mx, my);
                    } else {
                        if (prev_x >= 0)
                            brush_stroke(prev_x, prev_y, mx, my);
                        else
                            brush_stamp(mx, my);
                    }

                    int x0 = (prev_x >= 0 ? prev_x : mx);
                    int y0 = (prev_y >= 0 ? prev_y : my);
                    int r  = brush_radius + 1;
                    int lx = (x0 < mx ? x0 : mx) - r;
                    int ly = (y0 < my ? y0 : my) - r;
                    int hx = (x0 > mx ? x0 : mx) + r;
                    int hy = (y0 > my ? y0 : my) + r;
                    blit_rect(lx, ly, hx - lx, hy - ly);

                    prev_x = mx;
                    prev_y = my;
                }

                if (has_cursor)
                    move_cursor(mx, my);
                XFlush(dpy);
                break;
            }

            } /* switch */
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
        select(xfd + 1, &fds, NULL, NULL, &tv);
    }

    /* ── Cleanup ────────────────────────────────────────────── */
    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);
    XFreeCursor(dpy, blank);
    XFreeGC(dpy, gc_draw);
    XFreeGC(dpy, gc_copy);
    XFreeGC(dpy, gc_erase);
    free(brush_mask);
    if (erase_clip_r >= 0)
        XFreePixmap(dpy, erase_clip);
    if (has_cursor) {
        if (cursor_feedback_font)
            XFreeFont(dpy, cursor_feedback_font);
        XFreeGC(dpy, gc_cursor);
        XDestroyWindow(dpy, cursor_win);
        XFreeColormap(dpy, argb_cmap);
    }
    free(overlay_map);
    XFreePixmap(dpy, original);
    XFreePixmap(dpy, canvas);
    XDestroyWindow(dpy, win);
    XSync(dpy, False);
    restore_saved_focus();
    XCloseDisplay(dpy);

    return 0;
}
