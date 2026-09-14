/*
 * n9browser engine process.
 *
 * Hosts WPE WebKit and renders into an X window. When the Qt shell launches it,
 * that window is a CHILD of the shell's (see backend/n9-wpe-backend.c), so the
 * shell keeps ownership of orientation, the status bar and the chrome -- all of
 * which Qt Components already handles. Run without a parent it goes fullscreen,
 * which is how it is tested standalone.
 *
 * Control protocol: one command per line on STDIN, one event per line on
 * STDOUT. Diagnostics go to STDERR only -- anything printed to stdout would be
 * parsed as an event by the shell.
 *
 *   in:  load <url> | back | forward | reload | stop | geometry <x> <y> <w> <h>
 *        touch <down|motion|up> <slot> <x> <y> | zoom <level>
 *        set <javascript|images> <0|1> | scale <magnification>
 *        renderscale <pixels per css pixel> | useragent <string>
 *        axis <x> <y> <dx> <dy> | layoutwidth <css px>
 *        text <utf8> | key <keysym> <0|1> | scrollby <dx> <dy>
 *   out: ... | ime <0|1> | imerect <x> <y> <w> <h>
 *        clearcache | clearcookies | quit
 *   out: ... | surface <w> <h>
 *   out: url <s> | title <s> | progress <0..1> | loading <0|1> | scroll <y>
 *        nav <canBack> <canForward> | failed <msg> | ready
 *        window <xid> | frame
 *
 * Usage: n9engine [url]
 */

/* RTLD_DEFAULT is a GNU extension and needs this before dlfcn.h. */
#define _GNU_SOURCE

#include <wpe/webkit.h>
#include <wpe/wpe.h>
#include <glib.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/*
 * Memory budget. The device reports ~985 MB of RAM but only ~350 MB is actually
 * free, and WebKit's default memory limit is "system RAM" -- left alone it would
 * grow past what exists and be OOM-killed. Swap is ramzswap (compressed RAM),
 * so spilling costs CPU on the single core rather than buying capacity.
 */
/*
 * Lay out as a 345 CSS px phone, not as the 480 px tablet the panel would
 * otherwise imply. This is done with the DEVICE SCALE FACTOR, not page zoom:
 * dispatch_set_size takes the LOGICAL size and the scale factor turns it into
 * device pixels, so the page lays out at 320x450 while still rasterising at the
 * panel's full 480x674 -- crisp text at a phone layout. Page zoom would instead
 * reflow the document, which changes the page rather than presenting it.
 */
#define N9_LAYOUT_WIDTH          345

/*
 * How many device pixels to rasterise per CSS pixel. 1.0 renders the 320px
 * layout at 320x449 and lets the shell upscale it to the panel: soft, but
 * 2.25x fewer pixels to rasterise than matching the panel 1:1. On a 1 GHz A8
 * that trade is worth making -- a heavy page is CPU-bound in rasterisation,
 * not in anything we can otherwise influence. Override with N9_RENDER_SCALE.
 */
#define N9_RENDER_SCALE          1.0f

/*
 * Pinch magnification is applied by rendering MORE device pixels for the SAME
 * CSS viewport: device size and scale factor go up together, so
 * css = device / scale stays at 320x450 and WebKit never re-lays-out. Showing
 * the panel's 480x674 device pixels out of that larger surface is then a 1:1
 * mapping, which is what keeps magnified text sharp.
 *
 * The ceiling is the GPU: GL_MAX_TEXTURE_SIZE is 2048 on SGX530 (measured),
 * and 674 * 3 = 2022 fits where 674 * 4 would not.
 */
#define N9_MAX_SURFACE_PX        2048

#define N9_MEMORY_LIMIT_MB       200
#define N9_CONSERVATIVE_FRACTION 0.33
#define N9_STRICT_FRACTION       0.50
/* Killing the web process loses the page outright; on hardware this slow the
 * user would rather wait through swapping than lose the session. */
#define N9_KILL_FRACTION         0.90

static GMainLoop *loop = NULL;
static WebKitWebView *view = NULL;
static WebKitNetworkSession *session = NULL;

/* The panel-sized viewport, and the magnification currently rendered into it. */
static unsigned base_w = 0, base_h = 0;
static float surface_mag = 1.0f;


/* Implemented in the backend module, which libwpe dlopens -- so it is already in
 * the process and RTLD_DEFAULT finds it. Resolving it this way avoids linking
 * the app against a module that is not meant to be a link-time dependency. */
typedef void (*move_resize_fn)(int, int, unsigned, unsigned);
static move_resize_fn backend_move_resize = NULL;

static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);          /* the shell reads line by line */
}

/* Defined with the input-method context below; used from the load and crash
   handlers above it. */
static void set_ime_focus(gboolean focused);

static void send_nav(void)
{
    if (!view) return;
    emit("nav %d %d",
         webkit_web_view_can_go_back(view) ? 1 : 0,
         webkit_web_view_can_go_forward(view) ? 1 : 0);
}

static void on_load_changed(WebKitWebView *v, WebKitLoadEvent event, gpointer d)
{
    (void) d;
    switch (event) {
    case WEBKIT_LOAD_STARTED:
        emit("loading 1");
        emit("url %s", webkit_web_view_get_uri(v) ? webkit_web_view_get_uri(v) : "");
        break;
    case WEBKIT_LOAD_COMMITTED:
        /* The old document is gone from here, so anything it had focused is
           gone with it. COMMITTED rather than STARTED: a load that is started
           can still be cancelled, and reporting focus-out for a field that is
           in fact still focused is worse than reporting it late -- WebKit does
           not emit focus-in again for a field that never lost focus, so the
           keyboard would not come back at all. */
        set_ime_focus(FALSE);
        emit("url %s", webkit_web_view_get_uri(v) ? webkit_web_view_get_uri(v) : "");
        send_nav();
        break;
    case WEBKIT_LOAD_FINISHED:
        emit("loading 0");
        send_nav();
        break;
    default:
        break;
    }
}

/*
 * "unacceptable TLS certificate" on its own says nothing about WHY. This names
 * the actual failed check, which is the difference between a site problem, a
 * missing root in our bundle, a wrong clock, and something on the network
 * substituting its own certificate.
 */
static gboolean on_tls_error(WebKitWebView *v, const char *uri,
                             GTlsCertificate *cert, GTlsCertificateFlags flags,
                             gpointer d)
{
    (void) v; (void) cert; (void) d;
    GString *why = g_string_new(NULL);
    if (flags & G_TLS_CERTIFICATE_UNKNOWN_CA)    g_string_append(why, "unknown-CA ");
    if (flags & G_TLS_CERTIFICATE_BAD_IDENTITY)  g_string_append(why, "wrong-hostname ");
    if (flags & G_TLS_CERTIFICATE_NOT_ACTIVATED) g_string_append(why, "not-yet-valid ");
    if (flags & G_TLS_CERTIFICATE_EXPIRED)       g_string_append(why, "expired ");
    if (flags & G_TLS_CERTIFICATE_REVOKED)       g_string_append(why, "revoked ");
    if (flags & G_TLS_CERTIFICATE_INSECURE)      g_string_append(why, "insecure-algorithm ");
    if (flags & G_TLS_CERTIFICATE_GENERIC_ERROR) g_string_append(why, "generic-error ");
    if (!why->len) g_string_append(why, "none-set");

    fprintf(stderr, "[n9engine] TLS rejected %s: 0x%x %s\n", uri, flags, why->str);
    emit("failed TLS %s (%s)", why->str, uri);
    g_string_free(why, TRUE);
    return FALSE;      /* let the load fail as usual */
}

static gboolean on_load_failed(WebKitWebView *v, WebKitLoadEvent e,
                               const char *uri, GError *error, gpointer d)
{
    (void) v; (void) e; (void) uri; (void) d;
    emit("failed %s", error ? error->message : "unknown error");
    emit("loading 0");
    return FALSE;
}

static void on_progress(GObject *o, GParamSpec *p, gpointer d)
{
    (void) p; (void) d;
    emit("progress %.3f", webkit_web_view_get_estimated_load_progress(WEBKIT_WEB_VIEW(o)));
}

/* Title arrives on its own notify, NOT at load-finished: reading it there gives
 * "(no title)" because the document has not set it yet. */
static void on_title(GObject *o, GParamSpec *p, gpointer d)
{
    (void) p; (void) d;
    const char *t = webkit_web_view_get_title(WEBKIT_WEB_VIEW(o));
    emit("title %s", t ? t : "");
}

/* Settable at runtime. Wider layout = more CSS pixels in BOTH dimensions, so
   it is also the knob for "the page does not fit vertically": at 480 the
   viewport is 480x674 css instead of 320x449, half again as much height. */
static unsigned layout_width_override;

static unsigned layout_width(void)
{
    if (layout_width_override)
        return layout_width_override;
    const char *e = getenv("N9_LAYOUT_WIDTH");
    unsigned v = e ? (unsigned) atoi(e) : (unsigned) N9_LAYOUT_WIDTH;
    return v ? v : (unsigned) N9_LAYOUT_WIDTH;
}

/* Presence is NOT the test. The shell passes these on every launch, so it must
   be able to say "on" as well as "off" -- and an inherited N9_NO_JS=0, which
   reads to everyone as "do not disable", would otherwise disable. */
static gboolean env_flag(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

/* Settable at runtime, so the sharp/fast trade can be judged on the device
   instead of guessed at build time. */
static float render_scale_override = 0.0f;

static float render_scale(void)
{
    if (render_scale_override > 0.0f)
        return render_scale_override;
    const char *e = getenv("N9_RENDER_SCALE");
    float v = e ? (float) g_ascii_strtod(e, NULL) : N9_RENDER_SCALE;
    return (v >= 0.25f && v <= 4.0f) ? v : N9_RENDER_SCALE;
}

static void apply_surface(void)
{
    if (!view || !base_w || !base_h)
        return;
    struct wpe_view_backend *b =
        webkit_web_view_backend_get_wpe_backend(webkit_web_view_get_backend(view));
    if (!b)
        return;

    float mag = surface_mag;
    /* Clamp to what the GPU can actually hold as a texture. */
    while (mag > 1.0f &&
           (base_w * render_scale() * mag > N9_MAX_SURFACE_PX
            || base_h * render_scale() * mag > N9_MAX_SURFACE_PX))
        mag -= 0.05f;
    surface_mag = mag;

    /* Logical size is fixed by the layout width and does NOT change with
       magnification -- that is precisely what keeps the layout identical while
       the rasterisation gets denser. Magnification goes entirely into the
       device scale factor, so device pixels = logical * scale. */
    /* Layout width is independent of render scale: the page always lays out as
       a 345px phone, and the scale only decides how finely it is rasterised. */
    const float layout_scale = (float) base_w / (float) layout_width();
    unsigned lw = layout_width();
    unsigned lh = (unsigned) (base_h / layout_scale + 0.5f);
    float scale = render_scale() * mag;
    if (scale < 0.25f)
        scale = 0.25f;

    /* Scale before size: set_size is interpreted against the current scale. */
    wpe_view_backend_dispatch_set_device_scale_factor(b, scale);
    wpe_view_backend_dispatch_set_size(b, lw, lh);

    /* The SIZE reported to the shell comes from the backend, which is told by
       WebKit what it is really rendering. Computing it here was wrong by a
       pixel after rounding, and the shell then rejected every pixmap. */
    fprintf(stderr, "[n9engine] request %ux%u css at scale %.2f (mag %.2f) "
                    "-> %ux%u device\n", lw, lh, scale, mag,
            (unsigned) (lw * scale + 0.5f), (unsigned) (lh * scale + 0.5f));
}

/*
 * Touch. WPE turns these into clicks, scrolls and context menus itself
 * (TouchGestureController), so raw touchpoints are all the shell has to send --
 * no gesture recognition on our side.
 *
 * WPE wants the FULL set of live touchpoints on every event, with `id` naming
 * the one that changed, so the state is kept here between commands. Four slots
 * is one more than any gesture the platform reports and costs nothing.
 */
#define N9_MAX_TOUCH 4
static struct wpe_input_touch_event_raw touchpoints[N9_MAX_TOUCH];
static uint32_t touch_time = 0;

static void dispatch_touch(enum wpe_input_touch_event_type type, int slot, int x, int y)
{
    if (slot < 0 || slot >= N9_MAX_TOUCH) return;
    struct wpe_view_backend *b =
        webkit_web_view_backend_get_wpe_backend(webkit_web_view_get_backend(view));
    if (!b) return;

    /* Monotonic and in milliseconds is all the gesture controller asks of it;
       it compares timestamps to tell a tap from a drag. */
    touch_time += 16;

    touchpoints[slot].type = type;
    touchpoints[slot].time = touch_time;
    touchpoints[slot].id = slot;
    touchpoints[slot].x = x;
    touchpoints[slot].y = y;

    struct wpe_input_touch_event event = {
        touchpoints, N9_MAX_TOUCH, type, slot, touch_time, 0
    };
    wpe_view_backend_dispatch_touch_event(b, &event);

    /* A released point must not linger in the set, or the next gesture starts
       with a phantom finger still down. */
    if (type == wpe_input_touch_event_type_up) {
        touchpoints[slot].type = wpe_input_touch_event_type_null;
        touchpoints[slot].id = -1;
    }
}

/* Without this the web process can die and the shell just sits there with a
   frozen page and no idea why -- which is exactly what "it hangs" looks like. */
static void on_web_process_terminated(WebKitWebView *v,
                                      WebKitWebProcessTerminationReason reason,
                                      gpointer d)
{
    (void) v; (void) d;
    const char *why = reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed"
                    : reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT
                        ? "exceeded memory limit" : "terminated by API";
    fprintf(stderr, "[n9engine] web process %s\n", why);
    emit("failed web process %s", why);
    emit("loading 0");
    /* Same reason: the field that was focused died with the process. */
    set_ime_focus(FALSE);
}

/*
 * Scroll reporting, for the URL bar's hide-on-scroll.
 *
 * A script message handler rather than polling window.scrollY: this is
 * event-driven, costs nothing while the page is still, and does not wake the
 * web process on a timer. Throttled in the page, because a scroll listener
 * fires far more often than the shell can use.
 */
static void on_scroll_message(WebKitUserContentManager *m, JSCValue *value, gpointer d)
{
    (void) m; (void) d;
    if (jsc_value_is_number(value))
        emit("scroll %d", (int) jsc_value_to_double(value));
}

static const char *kScrollScript =
    "(function(){"
    "  var last = -1000;"
    "  function post() {"
    "    var y = window.scrollY || document.documentElement.scrollTop || 0;"
    "    if (Math.abs(y - last) < 8) return;"   /* throttle: 8 css px */
    "    last = y;"
    "    try { window.webkit.messageHandlers.n9scroll.postMessage(y); } catch (e) {}"
    "  }"
    "  window.addEventListener('scroll', post, {passive:true, capture:true});"
    "  window.addEventListener('load', post);"
    "})();";

/*
 * Input method: how the shell learns that the page focused a text field.
 *
 * WebKit drives a WebKitInputMethodContext for the focused editable. Nothing
 * else reports focus, so without this the on-screen keyboard has no reason to
 * open -- tapping a field in a page did nothing at all.
 *
 * Typed text goes back in through the context's "committed" signal rather than
 * synthesised key events: WebKit inserts the string directly, which handles
 * multi-byte characters and predictive input that a keysym stream cannot.
 * Editing keys (backspace, enter) still need real key events.
 */
#define N9_TYPE_IM_CONTEXT (n9_im_context_get_type())
G_DECLARE_FINAL_TYPE(N9ImContext, n9_im_context, N9, IM_CONTEXT, WebKitInputMethodContext)
struct _N9ImContext { WebKitInputMethodContext parent; };
G_DEFINE_TYPE(N9ImContext, n9_im_context, WEBKIT_TYPE_INPUT_METHOD_CONTEXT)

static N9ImContext *im_context = NULL;

/*
 * "ime" is a STATE, not an edge, and the engine is the only thing that can keep
 * it honest. WebKit reports focus-in for a field and focus-out when the page
 * blurs it -- but NOT when the document itself goes away. Submitting a form is
 * exactly that case: the field is focused, Return navigates, the document is
 * replaced, and no focus-out is ever emitted. The shell, which closes the
 * keyboard only on "ime 0", then leaves it up forever.
 *
 * So every path that destroys the focused field reports it here, and the state
 * is tracked so repeats cost nothing.
 */
static gboolean ime_focused = FALSE;

static void set_ime_focus(gboolean focused)
{
    if (ime_focused == focused)
        return;
    ime_focused = focused;
    emit("ime %d", focused ? 1 : 0);
}

static void n9_im_focus_in(WebKitInputMethodContext *c)
{
    (void) c;
    set_ime_focus(TRUE);
}

static void n9_im_focus_out(WebKitInputMethodContext *c)
{
    (void) c;
    set_ime_focus(FALSE);
}

/* Where the caret is, so the shell can keep the keyboard off it. */
static void n9_im_cursor_area(WebKitInputMethodContext *c, int x, int y, int w, int h)
{
    (void) c;
    emit("imerect %d %d %d %d", x, y, w, h);
}

static void n9_im_context_class_init(N9ImContextClass *klass)
{
    WebKitInputMethodContextClass *ic = WEBKIT_INPUT_METHOD_CONTEXT_CLASS(klass);
    ic->notify_focus_in = n9_im_focus_in;
    ic->notify_focus_out = n9_im_focus_out;
    ic->notify_cursor_area = n9_im_cursor_area;
}

static void n9_im_context_init(N9ImContext *self) { (void) self; }

static void handle_command(const char *line)
{
    if (!view || !line) return;
    /* Touch is per-motion-event and would drown the log; everything else is
       rare enough to trace. */
    if (strncmp(line, "touch ", 6))
        fprintf(stderr, "[n9engine] cmd: %s\n", line);

    if (!strncmp(line, "load ", 5)) {
        webkit_web_view_load_uri(view, line + 5);
    } else if (!strcmp(line, "back")) {
        webkit_web_view_go_back(view);
    } else if (!strcmp(line, "forward")) {
        webkit_web_view_go_forward(view);
    } else if (!strcmp(line, "reload")) {
        webkit_web_view_reload(view);
    } else if (!strcmp(line, "stop")) {
        webkit_web_view_stop_loading(view);
    } else if (!strncmp(line, "geometry ", 9)) {
        int x, y; unsigned w, h;
        if (sscanf(line + 9, "%d %d %u %u", &x, &y, &w, &h) == 4 && w && h) {
            (void) x; (void) y;
            base_w = w;
            base_h = h;
            apply_surface();
        }
    } else if (!strncmp(line, "touch ", 6)) {
        char kind[16];
        int slot, x, y;
        if (sscanf(line + 6, "%15s %d %d %d", kind, &slot, &x, &y) == 4) {
            enum wpe_input_touch_event_type type =
                !strcmp(kind, "down")   ? wpe_input_touch_event_type_down :
                !strcmp(kind, "motion") ? wpe_input_touch_event_type_motion :
                !strcmp(kind, "up")     ? wpe_input_touch_event_type_up :
                                          wpe_input_touch_event_type_null;
            if (type != wpe_input_touch_event_type_null)
                dispatch_touch(type, slot, x, y);
        }
    } else if (!strncmp(line, "zoom ", 5)) {
        /* WPE's TouchGestureController recognises taps, long-presses and
           scrolls but NOT pinch, so the shell does the recognising and this is
           just the result. Page zoom relayouts rather than scaling a bitmap --
           slower on this hardware, but text stays sharp at every level. */
        double z = g_ascii_strtod(line + 5, NULL);
        if (z >= 0.25 && z <= 5.0)
            webkit_web_view_set_zoom_level(view, z);
    } else if (!strncmp(line, "layoutwidth ", 12)) {
        int v = atoi(line + 12);
        if (v >= 240 && v <= 1024) {
            layout_width_override = (unsigned) v;
            apply_surface();
        }
    } else if (!strncmp(line, "renderscale ", 12)) {
        double v = g_ascii_strtod(line + 12, NULL);
        if (v >= 0.25 && v <= 4.0) {
            render_scale_override = (float) v;
            apply_surface();
        }
    } else if (!strncmp(line, "scale ", 6)) {
        double m = g_ascii_strtod(line + 6, NULL);
        if (m >= 1.0 && m <= 8.0) {
            surface_mag = (float) m;
            apply_surface();
        }
    } else if (!strncmp(line, "scrollby ", 9)) {
        /* Used to lift the caret clear of the on-screen keyboard. WebKit knows
           nothing about the keyboard, so the shell works out the overlap and
           asks for the scroll. scrollBy rather than scrollIntoView: it also
           works when the caret sits inside a nested scroller, which
           scrollIntoView would re-centre unnecessarily. */
        int dx, dy;
        if (sscanf(line + 9, "%d %d", &dx, &dy) == 2 && (dx || dy)) {
            char js[128];
            g_snprintf(js, sizeof js,
                       "window.scrollBy(%d,%d)", dx, dy);
            webkit_web_view_evaluate_javascript(view, js, -1, NULL, NULL,
                                                NULL, NULL, NULL);
        }
    } else if (!strncmp(line, "text ", 5)) {
        /* Straight into the focused field, as a committed string. */
        if (im_context)
            g_signal_emit_by_name(im_context, "committed", line + 5);
    } else if (!strncmp(line, "key ", 4)) {
        unsigned keysym; int pressed;
        if (sscanf(line + 4, "%u %d", &keysym, &pressed) == 2) {
            struct wpe_view_backend *b =
                webkit_web_view_backend_get_wpe_backend(webkit_web_view_get_backend(view));
            if (b) {
                struct wpe_input_keyboard_event e;
                memset(&e, 0, sizeof e);
                e.time = touch_time += 16;
                e.key_code = keysym;      /* xkb keysym: Backspace 0xFF08 etc. */
                e.pressed = pressed ? true : false;
                wpe_view_backend_dispatch_keyboard_event(b, &e);
            }
        }
    } else if (!strncmp(line, "axis ", 5)) {
        /*
         * A wheel event, in surface device pixels.
         *
         * Touch alone is not enough: WPE only synthesises scrolling from touch
         * when the PAGE did not consume the touch event
         * (WPEWebView.cpp, handle_touch_event -> doneWithTouchEvent). A modal
         * that attaches touchmove handlers and preventDefaults -- every cookie
         * banner does -- therefore cannot be scrolled at all. Wheel events do
         * not go through that path and scroll the innermost scrollable element
         * under the pointer.
         */
        int x, y, dx, dy;
        if (sscanf(line + 5, "%d %d %d %d", &x, &y, &dx, &dy) == 4) {
            struct wpe_view_backend *b =
                webkit_web_view_backend_get_wpe_backend(webkit_web_view_get_backend(view));
            if (b) {
                struct wpe_input_axis_2d_event e;
                memset(&e, 0, sizeof e);
                e.base.type = (enum wpe_input_axis_event_type)
                    (wpe_input_axis_event_type_motion_smooth
                     | wpe_input_axis_event_type_mask_2d);
                e.base.time = touch_time += 16;
                e.base.x = x;
                e.base.y = y;
                e.x_axis = dx;
                e.y_axis = dy;
                wpe_view_backend_dispatch_axis_event(b, &e.base);
            }
        }
    } else if (!strncmp(line, "useragent ", 10)) {
        /* Rest of the line, spaces and all -- a UA string is full of them. */
        webkit_settings_set_user_agent(webkit_web_view_get_settings(view),
                                       line + 10);
    } else if (!strncmp(line, "set ", 4)) {
        char key[32];
        int val = 0;
        if (sscanf(line + 4, "%31s %d", key, &val) == 2) {
            WebKitSettings *st = webkit_web_view_get_settings(view);
            if (!strcmp(key, "javascript"))
                webkit_settings_set_enable_javascript(st, val ? TRUE : FALSE);
            else if (!strcmp(key, "images"))
                webkit_settings_set_auto_load_images(st, val ? TRUE : FALSE);
        }
    } else if (!strcmp(line, "clearcache") || !strcmp(line, "clearcookies")) {
        /* WebKit owns these files and holds them open, so the shell must not
           delete them behind its back -- ask the owner instead. */
        if (session) {
            WebKitWebsiteDataTypes types = !strcmp(line, "clearcache")
                ? (WEBKIT_WEBSITE_DATA_DISK_CACHE | WEBKIT_WEBSITE_DATA_MEMORY_CACHE)
                : WEBKIT_WEBSITE_DATA_COOKIES;
            webkit_website_data_manager_clear(
                webkit_network_session_get_website_data_manager(session),
                types, 0, NULL, NULL, NULL);
        }
    } else if (!strcmp(line, "quit")) {
        g_main_loop_quit(loop);
    }
}

static gboolean on_stdin(GIOChannel *ch, GIOCondition cond, gpointer d)
{
    (void) d;
    if (cond & (G_IO_HUP | G_IO_ERR)) {      /* shell went away */
        g_main_loop_quit(loop);
        return FALSE;
    }
    char *line = NULL;
    gsize len = 0;
    while (g_io_channel_read_line(ch, &line, &len, NULL, NULL) == G_IO_STATUS_NORMAL && line) {
        g_strchomp(line);
        handle_command(line);
        g_free(line);
        line = NULL;
    }
    return TRUE;
}

int main(int argc, char *argv[])
{
    const char *url = argc > 1 ? argv[1] : "about:blank";

    /*
     * Serialise JIT compilation. This device has ONE CPU core, so JSC's
     * concurrent DFG compiler runs on a thread that competes with the very
     * JavaScript it is trying to speed up, and the optimised code lands later
     * than it otherwise would. Measured with probe/jsbench.html:
     *
     *   default                 total 3948 ms   (fib27 189, props 281, sort 1165)
     *   useConcurrentJIT=false  total 3140 ms   (fib27  41, props 132, sort  719)
     *
     * ~20% overall and 4.5x on call-heavy code. Concurrent GC is NOT worth
     * disabling as well -- measured 9% slower -- and lowering the tier-up
     * threshold made no difference.
     *
     * JSC reads its options from the environment, and the web process inherits
     * ours, so this reaches the process that actually runs the JavaScript.
     * overwrite = 0, so an external setting still wins for experiments.
     */
    setenv("JSC_useConcurrentJIT", "false", 0);

    /* NETWORK process only -- despite the generic-sounding name, the docs are
       explicit: "The periodic check for used memory is disabled by default on
       network processes." WPE 2.44 exposes no equivalent for the web process,
       which therefore runs with WebKit's default limit of system RAM. */
    WebKitMemoryPressureSettings *pressure = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(pressure, N9_MEMORY_LIMIT_MB);
    webkit_memory_pressure_settings_set_conservative_threshold(pressure, N9_CONSERVATIVE_FRACTION);
    webkit_memory_pressure_settings_set_strict_threshold(pressure, N9_STRICT_FRACTION);
    webkit_memory_pressure_settings_set_kill_threshold(pressure, N9_KILL_FRACTION);
    webkit_network_session_set_memory_pressure_settings(pressure);
    webkit_memory_pressure_settings_free(pressure);

    /* Without an explicit session the view falls back to
       webkit_network_session_get_default(), which is persistent but uses
       WebKit's own directories -- so the cache worked while the shell's cache
       page reported an empty one it had invented. Same directories, one owner. */
    const char *data_dir = getenv("N9_DATA_DIR");
    const char *cache_dir = getenv("N9_CACHE_DIR");
    session = webkit_network_session_new(data_dir, cache_dir);
    fprintf(stderr, "[n9engine] cache dir %s\n",
            cache_dir ? cache_dir : "(webkit default)");

    /* A persistent SESSION is not a persistent COOKIE JAR. WebKit's own docs on
       webkit_cookie_manager_set_persistent_storage are explicit: "By default,
       cookie_manager doesn't store the cookies persistently, so you need to
       call this method to keep cookies saved across sessions." Nothing reports
       the omission -- cookies work perfectly all session and are simply gone at
       the next start, which reads as a login problem rather than a storage one.

       Ask the data manager where the data actually went instead of reusing
       data_dir: it is NULL when the shell did not pass one, and WebKit then
       picks its own. Deriving the path a second time is what had the shell
       reporting on a cache directory nothing ever wrote to.

       SQLite, not TEXT: the text jar rewrites the whole file on every change,
       and cookies change constantly. Both jars are present in our libsoup. */
    {
        WebKitWebsiteDataManager *data_manager =
            webkit_network_session_get_website_data_manager(session);
        const char *base = webkit_website_data_manager_get_base_data_directory(data_manager);
        if (base) {
            /* soup opens the jar in the network process and will not create the
               directory for it; a missing one fails silently, like everything
               else about this path. */
            g_mkdir_with_parents(base, 0700);
            char *jar = g_build_filename(base, "cookies.db", NULL);
            webkit_cookie_manager_set_persistent_storage(
                webkit_network_session_get_cookie_manager(session), jar,
                WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
            fprintf(stderr, "[n9engine] cookie jar %s\n", jar);
            g_free(jar);
        } else {
            fprintf(stderr, "[n9engine] no data directory -- cookies will NOT persist\n");
        }
    }

    WebKitWebContext *context = webkit_web_context_new();
    /* DOCUMENT_BROWSER, not WEB_BROWSER: the latter caches "a very large number
       of resources and previously viewed content", the wrong trade on 350 MB. */
    webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);

    struct wpe_view_backend *wpe_backend = wpe_view_backend_create();
    if (!wpe_backend) {
        fprintf(stderr, "wpe_view_backend_create() failed -- no backend\n");
        return 1;
    }

    /* RTLD_DEFAULT does NOT find it: libwpe dlopens the backend with RTLD_LOCAL,
       so its symbols stay private to that handle. Re-open the same object with
       RTLD_NOLOAD -- the module is already mapped, so this returns the existing
       handle rather than loading a second copy -- and look the symbol up there. */
    const char *backend_path = getenv("WPE_BACKEND");
    if (backend_path && *backend_path) {
        void *h = dlopen(backend_path, RTLD_LAZY | RTLD_NOLOAD);
        if (h)
            backend_move_resize = (move_resize_fn) dlsym(h, "n9_backend_move_resize");
    }
    if (!backend_move_resize)
        backend_move_resize = (move_resize_fn) dlsym(RTLD_DEFAULT, "n9_backend_move_resize");
    if (!backend_move_resize)
        fprintf(stderr, "[n9engine] backend has no move_resize; window geometry is fixed\n");
    else
        fprintf(stderr, "[n9engine] backend move_resize resolved\n");

    WebKitUserContentManager *content = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(content, "n9scroll", NULL);
    g_signal_connect(content, "script-message-received::n9scroll",
                     G_CALLBACK(on_scroll_message), NULL);
    /* capture:true so it still fires for scrolling inside a nested scroller. */
    webkit_user_content_manager_add_script(content,
        webkit_user_script_new(kScrollScript,
                               WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                               WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                               NULL, NULL));

    WebKitWebViewBackend *view_backend = webkit_web_view_backend_new(wpe_backend, NULL, NULL);
    view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "backend", view_backend, "web-context", context,
        "network-session", session,
        "user-content-manager", content, NULL));

    im_context = g_object_new(N9_TYPE_IM_CONTEXT, NULL);
    webkit_web_view_set_input_method_context(view, WEBKIT_INPUT_METHOD_CONTEXT(im_context));

    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(on_progress), NULL);
    g_signal_connect(view, "notify::title", G_CALLBACK(on_title), NULL);
    g_signal_connect(view, "load-failed-with-tls-errors", G_CALLBACK(on_tls_error), NULL);
    g_signal_connect(view, "web-process-terminated",
                     G_CALLBACK(on_web_process_terminated), NULL);

    for (int i = 0; i < N9_MAX_TOUCH; ++i) {
        touchpoints[i].type = wpe_input_touch_event_type_null;
        touchpoints[i].id = -1;
    }

    /* Initial settings must be applied BEFORE the first load. The shell cannot
       send them in time -- it only learns the engine exists when "ready" is
       emitted, which is immediately before load_uri -- so anything that has to
       be true for the very first page comes in through the environment. */
    {
        WebKitSettings *st = webkit_web_view_get_settings(view);
        if (env_flag("N9_NO_JS")) {
            webkit_settings_set_enable_javascript(st, FALSE);
            fprintf(stderr, "[n9engine] javascript disabled\n");
        }
        /* Must be in place before the first load: a site decides which layout
           to serve from the very first request, and settings sent in reply to
           "ready" arrive after load_uri has already gone out. */
        const char *ua = getenv("N9_USER_AGENT");
        if (ua && *ua) {
            webkit_settings_set_user_agent(st, ua);
            fprintf(stderr, "[n9engine] user agent: %s\n", ua);
        }
        if (env_flag("N9_NO_IMAGES")) {
            webkit_settings_set_auto_load_images(st, FALSE);
            fprintf(stderr, "[n9engine] images disabled\n");
        }
    }

    GIOChannel *in = g_io_channel_unix_new(0);
    g_io_channel_set_flags(in, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(in, G_IO_IN | G_IO_HUP | G_IO_ERR, on_stdin, NULL);

    /* glib-networking is a runtime GIO module, so a missing or unloadable TLS
       backend produces no build error and no startup error -- just every
       https:// URL failing later with "TLS support is not available". Say so
       once, at startup, where it is cheap to notice. */
    {
        GTlsBackend *tls = g_tls_backend_get_default();
        fprintf(stderr, "[n9engine] TLS backend %s, https %s\n",
                tls ? G_OBJECT_TYPE_NAME(tls) : "(none)",
                (tls && g_tls_backend_supports_tls(tls)) ? "available" : "UNAVAILABLE");
    }

    fprintf(stderr, "[n9engine] loading %s (memory limit %d MB)\n", url, N9_MEMORY_LIMIT_MB);
    emit("ready");
    webkit_web_view_load_uri(view, url);

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
