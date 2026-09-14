/*
 * Compile one GLSL fragment shader from a file and report the result.
 *
 * Replays shaders captured out of the browser (probe/n9crash.c, N9_SHADER_DUMP)
 * against the driver's compiler in isolation, so a shader can be modified and
 * retested in seconds instead of rebuilding WebKit. A crash here is the answer
 * too -- run each variant as its own process.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: glcompile <shader.glsl>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    static char src[65536];
    size_t n = fread(src, 1, sizeof src - 1, f);
    src[n] = 0;
    fclose(f);

    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 2; }
    EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType) d);
    eglInitialize(ed, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(ed, ca, &cfg, 1, &ncfg) || !ncfg) return 2;
    EGLint vid = 0;
    eglGetConfigAttrib(ed, cfg, EGL_NATIVE_VISUAL_ID, &vid);
    XVisualInfo tmpl, *vi; int nvi = 0;
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.visualid = vid;
    vi = XGetVisualInfo(d, VisualIDMask, &tmpl, &nvi);
    if (!vi || !nvi) return 2;
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
    static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext c = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, xa);
    if (!eglMakeCurrent(ed, s, s, c)) return 2;

    printf("compiling %s (%u bytes)...\n", argv[1], (unsigned) n);
    fflush(stdout);

    GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
    const char *p = src;
    glShaderSource(sh, 1, &p, NULL);
    glCompileShader(sh);            /* this is where it dies, if it dies */

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok) {
        printf("RESULT: compiled OK\n");
        return 0;
    }
    char log[2048] = {0};
    glGetShaderInfoLog(sh, sizeof log, NULL, log);
    printf("RESULT: compile FAILED: %s\n", log);
    return 1;
}
