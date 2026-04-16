/*
 * Draw — Screenshot annotation tool
 *
 * Takes a screenshot, presents it fullscreen, and lets the user
 * draw / erase over it.
 *
 * Controls:
 *   Left  mouse button  — draw (current colour)
 *   1–9 keys            — switch colour
 *   Hold right button   — switch to eraser cursor
 *   Left click + right  — erase (restore original screenshot)
 *   Scroll wheel        — change brush size
 *   Side button (up)    — undo last stroke
 *   Side button (back)  — redo
 *   ESC / Ctrl+C        — quit
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
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

/* ── Tunables ─────────────────────────────────────────────────── */
#define BRUSH_INITIAL   5
#define BRUSH_MIN       1
#define BRUSH_MAX       50
#define ERASE_BORDER    2         /* white ring width when erasing  */
#define MAX_UNDO        30        /* max undo / redo levels         */

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

/* Undo / redo stacks (full canvas snapshots) */
static Pixmap   undo_stack[MAX_UNDO];
static int      undo_count;
static Pixmap   redo_stack[MAX_UNDO];
static int      redo_count;

/* Cached circular clip mask for the eraser */
static Pixmap   erase_clip;
static int      erase_clip_r = -1;  /* radius the clip was built for */

/* ARGB cursor overlay */
static Visual  *argb_visual;
static Colormap argb_cmap;
static Window   cursor_win;
static GC       gc_cursor;
static int      has_cursor;

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

static int cursor_diameter(void)
{
    int border = erase_mode ? ERASE_BORDER : 0;
    return (brush_radius + border) * 2 + 2;        /* +2 AA fringe */
}

static void render_cursor(void)
{
    int   d      = cursor_diameter();
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
static void brush_stamp(int x, int y)
{
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
    if (erase_clip_r == brush_radius) return;

    if (erase_clip_r >= 0)
        XFreePixmap(dpy, erase_clip);

    int d = brush_radius * 2;
    erase_clip = XCreatePixmap(dpy, canvas, d, d, 1);
    GC mgc = XCreateGC(dpy, erase_clip, 0, NULL);
    XSetForeground(dpy, mgc, 0);
    XFillRectangle(dpy, erase_clip, mgc, 0, 0, d, d);
    XSetForeground(dpy, mgc, 1);
    XFillArc(dpy, erase_clip, mgc, 0, 0, d, d, 0, 360 * 64);
    XFreeGC(dpy, mgc);

    XSetClipMask(dpy, gc_erase, erase_clip);
    erase_clip_r = brush_radius;
}

static void erase_stamp(int x, int y)
{
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

static Pixmap snapshot_canvas(void)
{
    int depth = DefaultDepth(dpy, screen_num);
    Pixmap snap = XCreatePixmap(dpy, win, scr_w, scr_h, depth);
    XCopyArea(dpy, canvas, snap, gc_copy, 0, 0, scr_w, scr_h, 0, 0);
    return snap;
}

static void push_stack(Pixmap *stack, int *count, Pixmap snap)
{
    if (*count >= MAX_UNDO) {
        XFreePixmap(dpy, stack[0]);
        memmove(&stack[0], &stack[1],
                (size_t)(MAX_UNDO - 1) * sizeof(Pixmap));
        (*count)--;
    }
    stack[(*count)++] = snap;
}

static void clear_stack(Pixmap *stack, int *count)
{
    for (int i = 0; i < *count; i++)
        XFreePixmap(dpy, stack[i]);
    *count = 0;
}

static void do_undo(void)
{
    if (undo_count == 0) return;
    push_stack(redo_stack, &redo_count, snapshot_canvas());
    undo_count--;
    XCopyArea(dpy, undo_stack[undo_count], canvas, gc_copy,
              0, 0, scr_w, scr_h, 0, 0);
    XFreePixmap(dpy, undo_stack[undo_count]);
    XCopyArea(dpy, canvas, win, gc_copy, 0, 0, scr_w, scr_h, 0, 0);
    XFlush(dpy);
}

static void do_redo(void)
{
    if (redo_count == 0) return;
    push_stack(undo_stack, &undo_count, snapshot_canvas());
    redo_count--;
    XCopyArea(dpy, redo_stack[redo_count], canvas, gc_copy,
              0, 0, scr_w, scr_h, 0, 0);
    XFreePixmap(dpy, redo_stack[redo_count]);
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
        while (XPending(dpy) > 0 && running) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            switch (ev.type) {

            case Expose:
                XCopyArea(dpy, canvas, win, gc_copy,
                          0, 0, scr_w, scr_h, 0, 0);
                break;

            case KeyPress: {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                if (ks == XK_Escape)
                    running = 0;
                else if (ks >= XK_1 && ks <= XK_9)
                    set_color((int)(ks - XK_1));
                break;
            }

            case ButtonPress:
                switch (ev.xbutton.button) {
                case 1: /* left = draw or erase depending on mode */
                    stroking = 1;
                    prev_x   = ev.xbutton.x;
                    prev_y   = ev.xbutton.y;
                    push_stack(undo_stack, &undo_count,
                               snapshot_canvas());
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
    if (erase_clip_r >= 0)
        XFreePixmap(dpy, erase_clip);
    if (has_cursor) {
        XFreeGC(dpy, gc_cursor);
        XDestroyWindow(dpy, cursor_win);
        XFreeColormap(dpy, argb_cmap);
    }
    XFreePixmap(dpy, original);
    XFreePixmap(dpy, canvas);
    XDestroyWindow(dpy, win);
    XSync(dpy, False);
    restore_saved_focus();
    XCloseDisplay(dpy);

    return 0;
}
