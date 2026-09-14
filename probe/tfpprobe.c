/*
 * Texture-from-pixmap canary.
 *
 * Decides whether the engine can keep its own X window and GPU compositing
 * while the Qt shell draws that window's contents as a texture inside the QML
 * scene -- which is the only way the page can follow Qt's portrait rotation,
 * since an X child window cannot be rotated.
 *
 * Walks the exact path the shell would: redirect the child window with
 * XComposite, name its backing pixmap, wrap that in an EGLImage, bind it as a
 * GLES2 texture in a SECOND context, draw it, and read the pixels back. Then
 * repaints the child and checks the texture follows WITHOUT rebinding -- if it
 * does not, the shell would have to recreate the image every frame.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define PW 854
#define PH 480
#define CW 400
#define CH 300

static int failures = 0;
static void report(const char *name, int ok, const char *fmt, ...)
{
    char buf[256] = {0};
    va_list ap; va_start(ap, fmt);
    if (fmt) vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("[%s] %-28s %s\n", ok ? "PASS" : "FAIL", name, buf);
    if (!ok) ++failures;
}

static PFNEGLCREATEIMAGEKHRPROC pCreateImage;
static PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture;

static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "  v_tex = a_pos * 0.5 + 0.5;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";
static const char *fs_src =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_tex); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        return 0;
    }
    return s;
}

static Window make_window(Display *d, Window parent, EGLDisplay ed, EGLConfig cfg,
                          int x, int y, int w, int h)
{
    EGLint vid = 0;
    eglGetConfigAttrib(ed, cfg, EGL_NATIVE_VISUAL_ID, &vid);
    XVisualInfo tmpl, *vi;
    int n = 0;
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.visualid = vid;
    vi = XGetVisualInfo(d, VisualIDMask, &tmpl, &n);
    if (!vi || !n) { fprintf(stderr, "no visual for id %d\n", vid); return 0; }

    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof swa);
    swa.colormap = XCreateColormap(d, parent, vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.background_pixel = 0;
    swa.event_mask = StructureNotifyMask;
    Window win = XCreateWindow(d, parent, x, y, w, h, 0, vi->depth, InputOutput,
                               vi->visual, CWColormap | CWBorderPixel |
                               CWBackPixel | CWEventMask, &swa);
    XFree(vi);
    return win;
}

/* Paint a flat colour into whichever surface is current, then publish it. */
static void paint(EGLDisplay ed, EGLSurface surf, float r, float g, float b)
{
    glDisable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    eglSwapBuffers(ed, surf);
}

/* Split the child: red on the half GL calls "up" (high y), blue on the low half.
   On a window surface GL y=0 is the BOTTOM row, so this is visually red-on-top.
   Sampling it back tells us whether an EGLImage from an X pixmap keeps GL's
   bottom-left origin or flips to the pixmap's top-left one -- which decides
   whether the shell has to invert its texture coordinates. */
static void paint_split(EGLDisplay ed, EGLSurface surf)
{
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, CW, CH / 2);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(0, CH / 2, CW, CH - CH / 2);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glFinish();
    eglSwapBuffers(ed, surf);
}

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no X display\n"); return 1; }
    Window root = DefaultRootWindow(d);

    int evb, erb, major = 0, minor = 0;
    int have_comp = XCompositeQueryExtension(d, &evb, &erb);
    if (have_comp) XCompositeQueryVersion(d, &major, &minor);
    report("XComposite", have_comp, "version %d.%d", major, minor);

    int dmaj = 0, dmin = 0, devb, derb;
    int have_dmg = XDamageQueryExtension(d, &devb, &derb);
    if (have_dmg) XDamageQueryVersion(d, &dmaj, &dmin);
    report("XDamage", have_dmg, "version %d.%d", dmaj, dmin);
    if (!have_comp) { printf("\nfatal: no XComposite\n"); return 1; }

    EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType) d);
    EGLint emaj, emin;
    if (!eglInitialize(ed, &emaj, &emin)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);

    const char *exts = eglQueryString(ed, EGL_EXTENSIONS);
    int have_img = exts && strstr(exts, "EGL_KHR_image_pixmap") != NULL;
    report("EGL_KHR_image_pixmap", have_img, NULL);

    pCreateImage = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    pDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
    pImageTargetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    report("entry points", pCreateImage && pImageTargetTexture, "%p %p",
           (void *) pCreateImage, (void *) pImageTargetTexture);
    if (!pCreateImage || !pImageTargetTexture) return 1;

    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(ed, cfg_attr, &cfg, 1, &ncfg) || !ncfg) {
        fprintf(stderr, "no EGL config\n"); return 1;
    }

    Window parent = make_window(d, root, ed, cfg, 0, 0, PW, PH);
    XMapWindow(d, parent);
    Window child = make_window(d, parent, ed, cfg, 0, 0, CW, CH);
    XMapWindow(d, child);
    XSync(d, False);
    report("windows created", parent && child, "parent 0x%lx child 0x%lx", parent, child);

    /* Redirect BEFORE the first frame: a window redirected mid-flight can lose
       whatever was already drawn. */
    XCompositeRedirectWindow(d, child, CompositeRedirectManual);
    XSync(d, False);

    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx_engine = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, ctx_attr);
    EGLContext ctx_shell  = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, ctx_attr);
    EGLSurface surf_child = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType) child, NULL);
    EGLSurface surf_parent = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType) parent, NULL);
    report("EGL surfaces", surf_child != EGL_NO_SURFACE && surf_parent != EGL_NO_SURFACE,
           "child %p parent %p (err 0x%x)", surf_child, surf_parent, eglGetError());
    if (surf_child == EGL_NO_SURFACE) return 1;

    /* --- "engine": paint the child orange ------------------------------- */
    eglMakeCurrent(ed, surf_child, surf_child, ctx_engine);
    paint(ed, surf_child, 1.0f, 0.5f, 0.0f);
    XSync(d, False);

    Pixmap pm = XCompositeNameWindowPixmap(d, child);
    XSync(d, False);
    report("NameWindowPixmap", pm != 0, "pixmap 0x%lx", pm);
    if (!pm) return 1;

    /* --- "shell": bind that pixmap as a texture -------------------------- */
    eglMakeCurrent(ed, surf_parent, surf_parent, ctx_shell);
    EGLImageKHR img = pCreateImage(ed, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
                                   (EGLClientBuffer) pm, NULL);
    report("eglCreateImageKHR", img != EGL_NO_IMAGE_KHR, "img %p err 0x%x", img, eglGetError());
    if (img == EGL_NO_IMAGE_KHR) return 1;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pImageTargetTexture(GL_TEXTURE_2D, img);
    GLenum gerr = glGetError();
    report("ImageTargetTexture2D", gerr == GL_NO_ERROR, "gl err 0x%x", gerr);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(prog, 0, "a_pos");
    glLinkProgram(prog);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);

    static const GLfloat quad[] = { -1,-1,  1,-1, -1,1,  1,1 };
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);

    unsigned char px[4];
    int ok_first, ok_follow;
    {
        glViewport(0, 0, PW, PH);
        glClearColor(0, 0, 1, 1);            /* blue: shows through if sampling fails */
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
        glReadPixels(PW / 2, PH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        ok_first = px[0] > 200 && px[1] > 90 && px[1] < 160 && px[2] < 60;
        report("texture sampled", ok_first, "rgba %d,%d,%d,%d (want ~255,128,0)",
               px[0], px[1], px[2], px[3]);
        eglSwapBuffers(ed, surf_parent);
    }

    /* --- repaint the child, redraw the shell WITHOUT rebinding ----------- */
    {
        eglMakeCurrent(ed, surf_child, surf_child, ctx_engine);
        paint(ed, surf_child, 0.0f, 1.0f, 0.0f);      /* green */
        XSync(d, False);

        eglMakeCurrent(ed, surf_parent, surf_parent, ctx_shell);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
        glReadPixels(PW / 2, PH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        ok_follow = px[1] > 200 && px[0] < 60;
        report("live without rebind", ok_follow, "rgba %d,%d,%d,%d (want ~0,255,0)",
               px[0], px[1], px[2], px[3]);
        eglSwapBuffers(ed, surf_parent);
    }

    /* --- which way up? -------------------------------------------------- */
    {
        unsigned char lo[4], hi[4];
        eglMakeCurrent(ed, surf_child, surf_child, ctx_engine);
        paint_split(ed, surf_child);
        XSync(d, False);

        eglMakeCurrent(ed, surf_parent, surf_parent, ctx_shell);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
        /* The quad maps texture v=0 to the LOW end of GL y, so reading the low
           and high rows back says which end of the texture holds the red half. */
        glReadPixels(PW / 2, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lo);
        glReadPixels(PW / 2, PH - 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, hi);
        int i;
        printf("       vertical scan of the drawn texture (parent %dx%d):\n", PW, PH);
        for (i = 0; i < 13; ++i) {
            int y = 4 + i * (PH - 9) / 12;
            unsigned char p[4];
            glReadPixels(PW / 2, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
            printf("         gl_y %3d (v %.2f)  %3d,%3d,%3d,%3d\n",
                   y, (double) y / PH, p[0], p[1], p[2], p[3]);
        }
        /* Sample well inside each half; the very first readback after a draw
           came back as blend garbage on this driver, and edge rows are the
           least trustworthy place to ask. */
        glReadPixels(PW / 2, PH / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lo);
        glReadPixels(PW / 2, PH * 3 / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, hi);
        int gl_origin = (hi[0] > 200 && lo[2] > 200);   /* window top at v=1 */
        int flipped   = (lo[0] > 200 && hi[2] > 200);   /* window top at v=0 */
        report("orientation", gl_origin || flipped, "%s (v=.25 %d,%d,%d  v=.75 %d,%d,%d)",
               gl_origin ? "bottom-left origin, v as-is" :
               flipped   ? "TOP-LEFT origin: item top-left samples v=0" : "indeterminate",
               lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        eglSwapBuffers(ed, surf_parent);
    }

    /* If it did not follow, does rebinding the same image fix it? That is the
       difference between a per-frame glEGLImageTargetTexture2DOES and nothing. */
    if (!ok_follow) {
        pImageTargetTexture(GL_TEXTURE_2D, img);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
        glReadPixels(PW / 2, PH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        report("live after rebind", px[1] > 200 && px[0] < 60,
               "rgba %d,%d,%d,%d", px[0], px[1], px[2], px[3]);
    }

    printf("\n%s (%d failure%s)\n", failures ? "TEXTURE-FROM-PIXMAP UNUSABLE" :
           "TEXTURE-FROM-PIXMAP WORKS", failures, failures == 1 ? "" : "s");

    pDestroyImage(ed, img);
    XFreePixmap(d, pm);
    XCloseDisplay(d);
    return failures ? 1 : 0;
}
