/* GLES2 limits that constrain WebKit's compositing shaders on this GPU. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType) d);
    eglInitialize(ed, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);
    /* Window, not pbuffer: this driver offers no pbuffer config at all, and it
       has no EGL_KHR_surfaceless_context either, so a real window is the only
       way to make a context current. */
    static const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(ed, ca, &cfg, 1, &n) || !n) {
        fprintf(stderr, "no pbuffer config (0x%x)\n", eglGetError());
        return 1;
    }
    EGLint vid = 0;
    eglGetConfigAttrib(ed, cfg, EGL_NATIVE_VISUAL_ID, &vid);
    XVisualInfo tmpl, *vi; int nvi = 0;
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.visualid = vid;
    vi = XGetVisualInfo(d, VisualIDMask, &tmpl, &nvi);
    if (!vi || !nvi) { fprintf(stderr, "no visual for id %d\n", vid); return 1; }
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof swa);
    swa.colormap = XCreateColormap(d, DefaultRootWindow(d), vi->visual, AllocNone);
    swa.border_pixel = 0; swa.background_pixel = 0;
    Window win = XCreateWindow(d, DefaultRootWindow(d), 0, 0, 16, 16, 0, vi->depth,
                               InputOutput, vi->visual,
                               CWColormap | CWBorderPixel | CWBackPixel, &swa);
    XMapWindow(d, win);
    XSync(d, False);
    EGLSurface s = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType) win, NULL);
    if (s == EGL_NO_SURFACE) {
        fprintf(stderr, "no window surface (0x%x)\n", eglGetError());
        return 1;
    }
    static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext c = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, xa);
    if (c == EGL_NO_CONTEXT) {
        fprintf(stderr, "no context (0x%x)\n", eglGetError());
        return 1;
    }
    if (!eglMakeCurrent(ed, s, s, c)) { fprintf(stderr, "makecurrent failed 0x%x\n", eglGetError()); return 1; }

    struct { GLenum e; const char *n; } q[] = {
        { GL_MAX_FRAGMENT_UNIFORM_VECTORS, "MAX_FRAGMENT_UNIFORM_VECTORS" },
        { GL_MAX_VERTEX_UNIFORM_VECTORS,   "MAX_VERTEX_UNIFORM_VECTORS" },
        { GL_MAX_VARYING_VECTORS,          "MAX_VARYING_VECTORS" },
        { GL_MAX_TEXTURE_IMAGE_UNITS,      "MAX_TEXTURE_IMAGE_UNITS" },
        { GL_MAX_TEXTURE_SIZE,             "MAX_TEXTURE_SIZE" },
        { GL_MAX_VERTEX_ATTRIBS,           "MAX_VERTEX_ATTRIBS" },
    };
    for (unsigned i = 0; i < sizeof q / sizeof q[0]; ++i) {
        GLint v = -1;
        glGetIntegerv(q[i].e, &v);
        printf("%-32s %d\n", q[i].n, v);
    }
    printf("\nWebKit's rounded-rect clip shader declares 30 vec4 + 10 mat4 = 70 vec4\n");
    GLint frag = 0;
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &frag);
    printf("=> %s (needs 70, has %d)\n", frag >= 70 ? "FITS" : "EXCEEDS THE LIMIT", frag);
    return 0;
}
