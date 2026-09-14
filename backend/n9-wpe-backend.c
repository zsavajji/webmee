/*
 * n9browser WPE backend -- X11 + EGL for the Nokia N9 (PowerVR SGX530).
 *
 * WPE's normal backend, wpebackend-fdo, is Wayland-only; Harmattan is X11. This
 * implements the six interfaces libwpe asks for by name, on top of Xlib and the
 * device's EGL 1.4 / GLES2 driver.
 *
 * Architecture, which mirrors what the stock N9 browser (QtWebKit 2.2) does:
 * WebKit's WebProcess rasterises tiles with Cairo on the CPU, and the compositor
 * uploads them as GLES2 textures and composites through EGL. We supply the
 * native display and window; WebKit does the rest.
 *
 * Device constraints, all measured rather than assumed (see project notes):
 *   - EGL 1.4, GLES2 only, max texture 2048, 8 texture units
 *   - EGL_KHR_fence_sync and GL_EXT_texture_format_BGRA8888 present (the latter
 *     matters: Cairo ARGB32 tiles upload with no swizzle)
 *   - EGL_KHR_surfaceless_context ABSENT, so the offscreen target uses a real
 *     pbuffer rather than a surfaceless context
 *   - eglCreateWindowSurface returns EGL_BAD_ALLOC unless the X window is built
 *     with the visual named by the config's EGL_NATIVE_VISUAL_ID
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <execinfo.h>
#include <ucontext.h>
#include <fcntl.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <EGL/egl.h>

#include <wpe/wpe.h>
#include <wpe/wpe-egl.h>

/* --------------------------------------------------------------- embedding -- */
/*
 * The engine is the Qt shell's renderer, not an application. When the shell
 * launches it, it passes its own X window id and the rectangle of the QML item
 * the page should occupy; we then create the EGL window as a CHILD of that,
 * which makes the shell responsible for orientation, the status bar and the
 * chrome -- all of which it already handles through Qt Components.
 *
 * Without N9_PARENT_WINDOW the engine still runs standalone and fullscreen,
 * which is how it is tested without the shell.
 */
/* CSS pixels the page should lay out to. 345 is a phone; the panel's 480 would
   make sites pick a tablet layout. */
static unsigned n9_layout_width(void)
{
    const char *e = getenv("N9_LAYOUT_WIDTH");
    unsigned v = e ? (unsigned) atoi(e) : 345u;
    return v ? v : 345u;
}

static Window n9_parent_window(void)
{
    const char *e = getenv("N9_PARENT_WINDOW");
    if (!e || !*e)
        return 0;
    return (Window) strtoul(e, NULL, 0);
}

static int n9_view_rect(int *x, int *y, unsigned *w, unsigned *h)
{
    const char *e = getenv("N9_VIEW_RECT");
    if (!e || !*e)
        return 0;
    int rx, ry; unsigned rw, rh;
    if (sscanf(e, "%d,%d,%u,%u", &rx, &ry, &rw, &rh) != 4)
        return 0;
    *x = rx; *y = ry; *w = rw; *h = rh;
    return 1;
}

/* The live on-screen target, so the app can move/resize it when the shell's
   layout changes. Exported and resolved with dlsym(RTLD_DEFAULT) rather than
   linked, because this module is dlopened by libwpe, not linked by the app. */
/*
 * Crash reporting.
 *
 * This module is dlopen()ed by libwpe in BOTH processes -- the engine and
 * WPEWebProcess -- which makes it the only piece of our own code running inside
 * the web process. The device has no gdb and core_pattern is /dev/null, so this
 * handler is the whole post-mortem: signal, faulting address, program counter,
 * a backtrace, and the memory map needed to turn those addresses into source
 * lines offline (tools/symbolise.sh).
 */
static void n9_write(const char *s, size_t n) { ssize_t r = write(2, s, n); (void) r; }

static void n9_crash_handler(int sig, siginfo_t *info, void *ctx)
{
    char buf[256];
    unsigned long pc = 0;
#ifdef __arm__
    const ucontext_t *uc = (const ucontext_t *) ctx;
    pc = uc->uc_mcontext.arm_pc;
#else
    (void) ctx;
#endif
    int n = snprintf(buf, sizeof buf,
                     "\n[n9-crash] pid=%d signal=%d addr=%p pc=0x%08lx\n",
                     (int) getpid(), sig, info ? info->si_addr : NULL, pc);
    n9_write(buf, (size_t) n);

    void *frames[32];
    int nf = backtrace(frames, 32);
    n9_write("[n9-crash] backtrace:\n", 22);
    backtrace_symbols_fd(frames, nf, 2);

    /* The maps are what make the addresses above meaningful: every interesting
       one is inside a shared object loaded at a runtime base. */
    n9_write("[n9-crash] maps:\n", 17);
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd >= 0) {
        char m[1024];
        ssize_t got;
        while ((got = read(fd, m, sizeof m)) > 0)
            n9_write(m, (size_t) got);
        close(fd);
    }
    n9_write("[n9-crash] end\n", 15);

    /* Let it die normally, so the parent still sees a real crash. */
    signal(sig, SIG_DFL);
    raise(sig);
}

/*
 * Volunteer the web process as the OOM killer's victim.
 *
 * On 2.6.32 badness() folds a share of each child's score into the parent, so
 * the ENGINE ends up with a far higher oom_score than the web process it
 * spawned (measured: 308804 against 36803, despite using less memory). Killing
 * the parent takes every child with it, turning a recoverable "page died,
 * reload" into the whole browser disappearing.
 *
 * Raising oom_adj is allowed unprivileged; lowering is not. So the web process
 * raises its own, and nothing needs root.
 */
static void n9_volunteer_for_oom(void)
{
    char comm[64] = {0};
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, comm, sizeof comm - 1);
    close(fd);
    if (n <= 0)
        return;
    if (!strstr(comm, "WPEWebProcess"))
        return;

    fd = open("/proc/self/oom_adj", O_WRONLY);
    if (fd < 0)
        return;
    ssize_t w = write(fd, "8\n", 2);
    close(fd);
    fprintf(stderr, "[n9-wpe] web process oom_adj=8 (%s)\n",
            w > 0 ? "set" : "failed");
}

__attribute__((constructor))
static void n9_install_crash_handler(void)
{
    n9_volunteer_for_oom();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = n9_crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    fprintf(stderr, "[n9-wpe] crash handler installed (pid %d)\n", (int) getpid());
}

static Display *g_target_dpy = NULL;
static Window g_target_window = 0;

__attribute__((visibility("default")))
void n9_backend_move_resize(int x, int y, unsigned w, unsigned h)
{
    if (!g_target_dpy || !g_target_window || !w || !h)
        return;
    XMoveResizeWindow(g_target_dpy, g_target_window, x, y, w, h);
    XSync(g_target_dpy, False);
}

/* ---------------------------------------------------------------- shared -- */

/* One X connection for the whole process. WebKit asks for the native display
   from more than one place, and handing out different Displays would put the
   EGL surfaces on connections the compositor does not own. */
static Display *n9_display(void)
{
    static Display *dpy = NULL;
    if (!dpy) {
        dpy = XOpenDisplay(NULL);
        if (!dpy)
            fprintf(stderr, "[n9-wpe] XOpenDisplay failed (DISPLAY=%s)\n",
                    getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
    }
    return dpy;
}

/* ------------------------------------------------------- renderer host ---- */
/*
 * The host lives in the UI process. Its only real job is handing out one end of
 * a socketpair per view; WebKit uses it for the compositor IPC and the view
 * backend returns the other end from get_renderer_host_fd().
 */

struct n9_host {
    int pending_fd;      /* the peer fd handed to the view backend */
};

static void *host_create(void)
{
    struct n9_host *h = calloc(1, sizeof(*h));
    if (h) h->pending_fd = -1;
    return h;
}

static void host_destroy(void *data)
{
    struct n9_host *h = data;
    if (!h) return;
    if (h->pending_fd != -1) close(h->pending_fd);
    free(h);
}

static int host_create_client(void *data)
{
    struct n9_host *h = data;
    int fds[2];
    /* SEQPACKET: WebKit's IPC is message-oriented, and a stream socket would
       let messages coalesce. CLOEXEC so the fd does not leak into the auxiliary
       processes WebKit spawns. */
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) < 0) {
        fprintf(stderr, "[n9-wpe] socketpair failed\n");
        return -1;
    }
    if (h) {
        if (h->pending_fd != -1) close(h->pending_fd);
        h->pending_fd = fds[1];
    } else {
        close(fds[1]);
    }
    return fds[0];
}

static struct wpe_renderer_host_interface n9_renderer_host = {
    host_create, host_destroy, host_create_client,
    NULL, NULL, NULL, NULL
};

/* --------------------------------------------------------- view backend --- */

struct n9_view {
    struct wpe_view_backend *backend;
    int host_fd;
    uint32_t width, height;
    uint32_t device_width, device_height;
    float scale;
};

static void *view_create(void *params, struct wpe_view_backend *backend)
{
    (void) params;
    struct n9_view *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->backend = backend;
    v->host_fd = wpe_renderer_host_create_client();

    /* Ask X rather than hardcoding: the panel is 854x480 but the window manager
       may hand us a rotated or inset area, and a viewport that disagrees with
       the window leaves part of the page unpainted. */
    int rx, ry; unsigned rw, rh;
    if (n9_view_rect(&rx, &ry, &rw, &rh)) {
        v->width = rw;
        v->height = rh;          /* embedded: the shell dictates the viewport */
    } else {
        Display *dpy = n9_display();
        if (dpy) {
            int scr = DefaultScreen(dpy);
            v->width = (uint32_t) DisplayWidth(dpy, scr);
            v->height = (uint32_t) DisplayHeight(dpy, scr);
        }
    }
    if (!v->width || !v->height) {
        v->width = 854;
        v->height = 480;
    }

    /* The size WebKit is told is the LOGICAL one: it lays out in CSS pixels and
       multiplies by the device scale factor to get the surface. Reporting the
       panel's device size here is what made the page lay out as a 480px tablet
       and then render 720x1011 -- the engine's later dispatch_set_size lost a
       race with this one. Deriving it here removes the race entirely; the X
       window is still created at the device size, below. */
    v->device_width = v->width;
    v->device_height = v->height;
    {
        unsigned layout = n9_layout_width();
        if (layout && v->width > layout) {
            v->scale = (float) v->width / (float) layout;
            v->width = (uint32_t) (v->device_width / v->scale + 0.5f);
            v->height = (uint32_t) (v->device_height / v->scale + 0.5f);
        } else {
            v->scale = 1.0f;
        }
    }
    fprintf(stderr, "[n9-wpe] view %ux%u css, %ux%u device, scale %.2f\n",
            v->width, v->height, v->device_width, v->device_height, v->scale);
    return v;
}

static void view_destroy(void *data)
{
    struct n9_view *v = data;
    if (!v) return;
    if (v->host_fd != -1) close(v->host_fd);
    free(v);
}

static void view_initialize(void *data)
{
    struct n9_view *v = data;
    if (!v) return;
    /* WebKit lays out nothing until it is told a size. */
    fprintf(stderr, "[n9-wpe] dispatch_set_size %ux%u\n", v->width, v->height);
    wpe_view_backend_dispatch_set_size(v->backend, v->width, v->height);
}

static int view_get_renderer_host_fd(void *data)
{
    struct n9_view *v = data;
    return v ? v->host_fd : -1;
}

static struct wpe_view_backend_interface n9_view_backend = {
    view_create, view_destroy, view_initialize, view_get_renderer_host_fd,
    NULL, NULL, NULL, NULL
};

/* ------------------------------------------------ renderer backend egl ---- */

struct n9_egl_backend {
    Display *dpy;
};

static void *egl_backend_create(int host_fd)
{
    (void) host_fd;
    struct n9_egl_backend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->dpy = n9_display();
    return b;
}

static void egl_backend_destroy(void *data)
{
    free(data);
}

static EGLNativeDisplayType egl_backend_get_native_display(void *data)
{
    struct n9_egl_backend *b = data;
    /* EGLNativeDisplayType is void* here: the PowerVR headers' X11 block is
       gated on SUPPORT_X11, and without it they fall back to "int". We ship a
       shadowing eglplatform.h that corrects just that typedef. */
    return (EGLNativeDisplayType) (b ? b->dpy : n9_display());
}

static uint32_t egl_backend_get_platform(void *data)
{
    (void) data;
    return 0;   /* no EGL_PLATFORM_* enum; plain eglGetDisplay semantics */
}

static struct wpe_renderer_backend_egl_interface n9_egl_backend_iface = {
    egl_backend_create, egl_backend_destroy,
    egl_backend_get_native_display, egl_backend_get_platform,
    NULL, NULL, NULL
};

/* ----------------------------------------- renderer backend egl target ---- */

struct n9_egl_target {
    struct wpe_renderer_backend_egl_target *target;
    Display *dpy;
    Window window;
    uint32_t width, height;
};

static void *egl_target_create(struct wpe_renderer_backend_egl_target *target, int host_fd)
{
    (void) host_fd;
    struct n9_egl_target *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->target = target;
    t->dpy = n9_display();
    return t;
}

static void egl_target_destroy(void *data)
{
    struct n9_egl_target *t = data;
    if (!t) return;
    if (t->window && t->dpy)
        XDestroyWindow(t->dpy, t->window);
    free(t);
}

/* Pick the X visual the EGL config names, or eglCreateWindowSurface will fail
   with EGL_BAD_ALLOC (0x3005) -- confirmed on device. */
static Window n9_create_window(Display *dpy, uint32_t width, uint32_t height)
{
    EGLDisplay edpy = eglGetDisplay((EGLNativeDisplayType) dpy);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(edpy, &major, &minor)) {
        fprintf(stderr, "[n9-wpe] eglInitialize failed 0x%x\n", eglGetError());
        return 0;
    }

    static const EGLint cfg_attrs[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(edpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "[n9-wpe] no GLES2 window config 0x%x\n", eglGetError());
        return 0;
    }

    EGLint vid = 0;
    eglGetConfigAttrib(edpy, cfg, EGL_NATIVE_VISUAL_ID, &vid);

    XVisualInfo tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.visualid = (VisualID) vid;
    int nvis = 0;
    XVisualInfo *vi = XGetVisualInfo(dpy, VisualIDMask, &tmpl, &nvis);
    if (!vi || nvis < 1) {
        fprintf(stderr, "[n9-wpe] no X visual for EGL visual id %d\n", vid);
        return 0;
    }

    Window parent = n9_parent_window();
    int wx = 0, wy = 0;
    unsigned rw, rh;
    int rx, ry;
    if (parent && n9_view_rect(&rx, &ry, &rw, &rh)) {
        wx = rx; wy = ry;
    }
    if (!parent)
        parent = RootWindow(dpy, vi->screen);

    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask | ExposureMask;

    Window w = XCreateWindow(dpy, parent, wx, wy, width, height, 0,
                             vi->depth, InputOutput, vi->visual,
                             CWBorderPixel | CWColormap | CWEventMask, &swa);
    XFree(vi);
    if (!w) {
        fprintf(stderr, "[n9-wpe] XCreateWindow failed\n");
        return 0;
    }
    /* Go fullscreen BEFORE mapping. Left alone, MCompositor treats this as an
       ordinary application window and reserves space: the window came back as
       854x376 at (0,36) -- 36px for the status bar, 68px at the bottom -- while
       WebKit had been told the viewport was 854x480. The page then lays out
       taller than the window and the bottom is simply never shown.
       Setting the state before the map avoids a visible resize afterwards. */
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    /* Only meaningful for a top-level window. A child inherits the shell's
       geometry and must not ask the WM for anything. */
    if (!n9_parent_window() && wm_state != None && fullscreen != None) {
        XChangeProperty(dpy, w, wm_state, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *) &fullscreen, 1);
    }

    /* Tell the platform how this window is oriented. The N9's framebuffer is
       854x480 LANDSCAPE even though the device is held in portrait -- the panel
       is mounted rotated -- so Harmattan apps declare an angle and draw to
       match. Qt Components does this via screen.currentOrientation; a raw X
       client has to set the property itself.
       Whether MCompositor rotates for us or merely aligns the status bar and
       decorator to the declared angle is what this is testing. */
    Atom angle_atom = XInternAtom(dpy, "_MEEGOTOUCH_ORIENTATION_ANGLE", False);
    if (!n9_parent_window() && angle_atom != None) {
        unsigned long angle = 270;
        XChangeProperty(dpy, w, angle_atom, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *) &angle, 1);
    }
    /* NOT declaring _MEEGOTOUCH_NET_WM_WINDOW_TYPE_MAPPLICATION here, though it
       looks like the obvious thing to do: it makes MCompositor treat the engine
       as a task-switcher application, and an un-activated one stays behind the
       launcher -- tested, and the home screen covered the page.
       The engine is not an application in its own right. It is the renderer for
       the Qt shell, and will become a CHILD window of the shell's. Orientation,
       the status bar and the chrome are all the shell's business; declaring
       ourselves an application fights it for all three. */

    /* Redirect BEFORE mapping, and only when embedded.
       The shell samples this window as a texture and never wants X to composite
       it into the parent. It cannot do the redirect itself early enough: it
       does not learn the XID until after this function returns, and the gap
       between XMapWindow and the shell's first paint is long enough to flash
       the page across the screen unrotated. The window's own client is the only
       one that can redirect it this early.
       Standalone, redirecting would mean nothing is ever displayed. */
    if (n9_parent_window()) {
        int evb, erb;
        if (XCompositeQueryExtension(dpy, &evb, &erb)) {
            XCompositeRedirectWindow(dpy, w, CompositeRedirectManual);
            XSync(dpy, False);
            fprintf(stderr, "[n9-wpe] window redirected before map\n");
        } else {
            fprintf(stderr, "[n9-wpe] no XComposite: page will not reach the shell\n");
        }
    }

    XMapWindow(dpy, w);
    XSync(dpy, False);

    /* Report what we actually got, so a mismatch is visible in the log rather
       than showing up as a mysteriously cropped page. */
    XWindowAttributes wa;
    if (XGetWindowAttributes(dpy, w, &wa)) {
        fprintf(stderr, "[n9-wpe] window mapped %dx%d at (%d,%d)\n",
                wa.width, wa.height, wa.x, wa.y);
    }
    return w;
}

static void egl_target_initialize(void *data, void *backend_data, uint32_t width, uint32_t height)
{
    (void) backend_data;
    struct n9_egl_target *t = data;
    if (!t || !t->dpy) return;
    fprintf(stderr, "[n9-wpe] target initialize %ux%u\n", width, height);
    /* Authoritative surface size: whatever WebKit actually renders, reported by
       the one piece of code that knows. The engine used to compute this and got
       it wrong by a pixel after rounding, which made the shell reject every
       pixmap and show a black page. */
    printf("surface %u %u\n", width, height);
    fflush(stdout);
    t->width = width;
    t->height = height;
    t->window = n9_create_window(t->dpy, width, height);
    g_target_dpy = t->dpy;
    g_target_window = t->window;
    /* The shell redirects this window with XComposite and samples its backing
       pixmap as a texture -- that is how the page ends up inside the QML scene
       and follows Qt's rotation. stdout is the engine's event channel; both
       writers flush whole lines, so they cannot interleave mid-line. */
    printf("window %lu\n", (unsigned long) t->window);
    fflush(stdout);
}

static EGLNativeWindowType egl_target_get_native_window(void *data)
{
    struct n9_egl_target *t = data;
    /* A Window is an XID, not a pointer; EGLNativeWindowType is void* in these
       headers, and both are 32-bit on armv7, so the driver sees the same value. */
    return t ? (EGLNativeWindowType) (uintptr_t) t->window : (EGLNativeWindowType) 0;
}

static void egl_target_resize(void *data, uint32_t width, uint32_t height)
{
    struct n9_egl_target *t = data;
    if (!t || !t->window) return;
    fprintf(stderr, "[n9-wpe] target resize %ux%u\n", width, height);
    printf("surface %u %u\n", width, height);
    fflush(stdout);
    t->width = width;
    t->height = height;
    XResizeWindow(t->dpy, t->window, width, height);
    XSync(t->dpy, False);
}

static void egl_target_frame_will_render(void *data) { (void) data; }

static void egl_target_frame_rendered(void *data)
{
    struct n9_egl_target *t = data;
    if (!t) return;
    /* Tell the shell to repaint before releasing WebKit to render the next
       frame. The texture already shows this one -- it tracks the window without
       rebinding (measured) -- so this is only a wake-up. It also throttles the
       engine to the shell's draw rate, since a full pipe blocks here. */
    printf("frame\n");
    fflush(stdout);
    /* WebKit will not start another frame until this is dispatched. Forgetting
       it is the classic way to get a browser that renders exactly one frame. */
    wpe_renderer_backend_egl_target_dispatch_frame_complete(t->target);
}

static void egl_target_deinitialize(void *data)
{
    struct n9_egl_target *t = data;
    if (!t || !t->window) return;
    XDestroyWindow(t->dpy, t->window);
    t->window = 0;
}

static struct wpe_renderer_backend_egl_target_interface n9_egl_target_iface = {
    egl_target_create, egl_target_destroy, egl_target_initialize,
    egl_target_get_native_window, egl_target_resize,
    egl_target_frame_will_render, egl_target_frame_rendered,
    egl_target_deinitialize,
    NULL, NULL, NULL
};

/* ------------------------------- renderer backend egl offscreen target ---- */
/*
 * Used for GL work with no on-screen window. This device has no
 * EGL_KHR_surfaceless_context (measured), so a real 1x1 pbuffer is required --
 * returning a null window here would leave the context uncurrentable.
 */

struct n9_egl_offscreen {
    Display *dpy;
    Window window;
};

static void *egl_offscreen_create(void)
{
    struct n9_egl_offscreen *o = calloc(1, sizeof(*o));
    if (o) o->dpy = n9_display();
    return o;
}

static void egl_offscreen_destroy(void *data)
{
    struct n9_egl_offscreen *o = data;
    if (!o) return;
    if (o->window && o->dpy) XDestroyWindow(o->dpy, o->window);
    free(o);
}

static void egl_offscreen_initialize(void *data, void *backend_data)
{
    (void) backend_data;
    struct n9_egl_offscreen *o = data;
    if (!o || !o->dpy) return;
    /* An unmapped 1x1 window stands in for a pbuffer: it needs no compositor
       cooperation and uses the same visual-matching path as the real target. */
    o->window = n9_create_window(o->dpy, 1, 1);
    if (o->window)
        XUnmapWindow(o->dpy, o->window);
}

static EGLNativeWindowType egl_offscreen_get_native_window(void *data)
{
    struct n9_egl_offscreen *o = data;
    return o ? (EGLNativeWindowType) (uintptr_t) o->window : (EGLNativeWindowType) 0;
}

static struct wpe_renderer_backend_egl_offscreen_target_interface n9_egl_offscreen_iface = {
    egl_offscreen_create, egl_offscreen_destroy,
    egl_offscreen_initialize, egl_offscreen_get_native_window,
    NULL, NULL, NULL, NULL
};

/* ------------------------------------------------------------ pasteboard -- */
/* Minimal in-process clipboard: enough for copy/paste inside the browser.
   Wiring it to the X CLIPBOARD selection is a later job. */

static void pasteboard_get_types(void *data, struct wpe_pasteboard_string_vector *types)
{
    (void) data;
    if (types) { types->strings = NULL; types->length = 0; }
}

static void pasteboard_get_string(void *data, const char *type, struct wpe_pasteboard_string *string)
{
    (void) data; (void) type;
    if (string) { string->data = NULL; string->length = 0; }
}

static void pasteboard_write(void *data, struct wpe_pasteboard_string_map *map)
{
    (void) data; (void) map;
}

static void *pasteboard_initialize(struct wpe_pasteboard *pasteboard)
{
    (void) pasteboard;
    return NULL;
}

static struct wpe_pasteboard_interface n9_pasteboard = {
    pasteboard_initialize, pasteboard_get_types,
    pasteboard_get_string, pasteboard_write,
    NULL, NULL, NULL, NULL
};

/* ---------------------------------------------------------------- loader -- */

static void *n9_load_object(const char *name)
{
    if (!name) return NULL;
    if (!strcmp(name, "_wpe_renderer_host_interface"))        return &n9_renderer_host;
    if (!strcmp(name, "_wpe_view_backend_interface"))         return &n9_view_backend;
    if (!strcmp(name, "_wpe_renderer_backend_egl_interface")) return &n9_egl_backend_iface;
    if (!strcmp(name, "_wpe_renderer_backend_egl_target_interface"))
        return &n9_egl_target_iface;
    if (!strcmp(name, "_wpe_renderer_backend_egl_offscreen_target_interface"))
        return &n9_egl_offscreen_iface;
    if (!strcmp(name, "_wpe_pasteboard_interface"))           return &n9_pasteboard;

    fprintf(stderr, "[n9-wpe] unhandled object request: %s\n", name);
    return NULL;
}

__attribute__((visibility("default")))
struct wpe_loader_interface _wpe_loader_interface = {
    n9_load_object, NULL, NULL, NULL, NULL
};
