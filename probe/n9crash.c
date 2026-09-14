/*
 * Crash reporter, as an LD_PRELOAD shim.
 *
 * The device has no gdb and core_pattern is /dev/null, so a post-mortem has to
 * come from inside the process. Preloading is what makes it reachable without
 * root: our own binaries carry DT_RPATH, which the loader searches BEFORE
 * LD_LIBRARY_PATH, so a rebuilt module in a writable directory can never win --
 * but LD_PRELOAD is applied earlier still, and is inherited by the web process
 * WebKit spawns.
 *
 * Prints the signal, the faulting address, the PC, a backtrace and the memory
 * map; tools/symbolise.sh turns those into source lines against the unstripped
 * libraries kept on the build host.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <execinfo.h>
#include <ucontext.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <GLES2/gl2.h>

/*
 * The crash lands inside the PowerVR GLSL compiler, so the useful evidence is
 * the shader it was compiling. Interposing is free here: LD_PRELOAD puts these
 * ahead of libGLESv2, and RTLD_NEXT still reaches the real ones.
 */
static void w(const char *s, size_t n) { ssize_t r = write(2, s, n); (void) r; }

/* Keyed, not indexed: this driver hands out shader names like 350005, so an
   array indexed by id stored nothing at all. */
#define N9_MAX_SHADER 16
#define N9_SHADER_BYTES 32768
static struct {
    GLuint id;
    int len;
    char src[N9_SHADER_BYTES];
} g_slot[N9_MAX_SHADER];
static unsigned g_next_slot;

static int slot_of(GLuint id, int create)
{
    for (int i = 0; i < N9_MAX_SHADER; ++i)
        if (g_slot[i].id == id && g_slot[i].len)
            return i;
    if (!create)
        return -1;
    int i = (int) (g_next_slot++ % N9_MAX_SHADER);
    g_slot[i].id = id;
    g_slot[i].len = 0;
    return i;
}
static GLuint g_compiling;
static GLuint g_linking;

/*
 * WebKit reaches GL through libepoxy, which dlsym()s every entry point off a
 * dlopen()ed handle -- so ordinary LD_PRELOAD symbol interposition never runs.
 * Interposing dlsym ITSELF is the way in: epoxy asks us for the pointer and we
 * hand back our wrapper.
 */
static void reclaim_handler(void);
static void *(*real_dlsym)(void *, const char *);

/* The handle epoxy used. NOT RTLD_NEXT: epoxy dlopen()s libGLESv2 with
 * RTLD_LOCAL, so it is not in the global scope and RTLD_NEXT finds nothing --
 * which silently turned every wrapped call into a no-op and, incidentally,
 * "fixed" the crash by never compiling a shader at all. */
static void *g_gl_handle;

static void *gl_real(const char *name)
{
    if (!real_dlsym)
        return NULL;
    void *p = g_gl_handle ? real_dlsym(g_gl_handle, name) : NULL;
    if (!p)
        p = real_dlsym(RTLD_NEXT, name);
    if (!p)
        p = real_dlsym(RTLD_DEFAULT, name);
    return p;
}

static void n9_dump_shader(GLuint id)
{
    int i = slot_of(id, 0);
    if (i < 0) {
        char miss[64];
        int n = snprintf(miss, sizeof miss, "\n---- shader %u: source not captured ----\n", id);
        w(miss, (size_t) n);
        return;
    }
    char hdr[64];
    int n = snprintf(hdr, sizeof hdr, "\n---- shader %u (%d bytes) ----\n", id, g_slot[i].len);
    w(hdr, (size_t) n);
    w(g_slot[i].src, (size_t) g_slot[i].len);
}


/* The 2011 GLES2 header has no GLchar and spells this "const char**"; the
   interposer must match that exactly or it silently fails to override. */
void glShaderSource(GLuint shader, GLsizei count,
                    const char **string, const GLint *length)
{
    static void (*real)(GLuint, GLsizei, const char **, const GLint *);
    if (!real) real = (void (*)(GLuint, GLsizei, const char **, const GLint *))
        gl_real("glShaderSource");
    if (!real) { w("[n9-crash] glShaderSource unresolved\n", 38); return; }

    {
        int sl = slot_of(shader, 1);
        int off = 0;
        for (GLsizei i = 0; i < count && off < N9_SHADER_BYTES - 1; ++i) {
            int len = length && length[i] >= 0 ? length[i] : (int) strlen(string[i]);
            if (off + len > N9_SHADER_BYTES - 1)
                len = N9_SHADER_BYTES - 1 - off;
            memcpy(g_slot[sl].src + off, string[i], len);
            off += len;
        }
        g_slot[sl].src[off] = 0;
        g_slot[sl].len = off;
    }
    real(shader, count, string, length);
}

void glCompileShader(GLuint shader)
{
    static void (*real)(GLuint);
    if (!real) real = (void (*)(GLuint)) gl_real("glCompileShader");
    if (!real) { w("[n9-crash] glCompileShader unresolved\n", 39); return; }
    reclaim_handler();
    g_compiling = shader;
    if (getenv("N9_LOG_SHADERS"))
        n9_dump_shader(shader);
    /* Save every shader to its own file, so the exact text can be replayed
       against the compiler outside the browser. */
    {
        const char *dir = getenv("N9_SHADER_DUMP");
        int sl = slot_of(shader, 0);
        if (dir && sl >= 0) {
            char path[256];
            snprintf(path, sizeof path, "%s/shader-%u.glsl", dir, shader);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                ssize_t r = write(fd, g_slot[sl].src, (size_t) g_slot[sl].len);
                (void) r;
                close(fd);
            }
        }
    }
    real(shader);
    g_compiling = 0;
}

void glAttachShader(GLuint program, GLuint shader)
{
    static void (*real)(GLuint, GLuint);
    if (!real) real = (void (*)(GLuint, GLuint)) gl_real("glAttachShader");
    if (!real) { w("[n9-crash] glAttachShader unresolved\n", 38); return; }
    real(program, shader);
}

/* PowerVR does the real translation at LINK time, not at glCompileShader --
   which is why the first capture caught nothing. */
void glLinkProgram(GLuint program)
{
    static void (*real)(GLuint);
    if (!real) real = (void (*)(GLuint)) gl_real("glLinkProgram");
    if (!real) { w("[n9-crash] glLinkProgram unresolved\n", 37); return; }
    reclaim_handler();
    g_linking = program;
    real(program);
    g_linking = 0;
}

static void handler(int sig, siginfo_t *info, void *ctx)
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
    w(buf, (size_t) n);

    void *frames[48];
    int nf = backtrace(frames, 48);
    w("[n9-crash] backtrace:\n", 22);
    backtrace_symbols_fd(frames, nf, 2);

    if (g_linking) {
        char hdr[96];
        int hn = snprintf(hdr, sizeof hdr,
                          "[n9-crash] DIED LINKING PROGRAM %u:\n", g_linking);
        w(hdr, (size_t) hn);
        for (int i = 0; i < N9_MAX_SHADER; ++i)
            if (g_slot[i].len)
                n9_dump_shader(g_slot[i].id);
        w("\n[n9-crash] end of shader\n", 26);
    } else if (g_compiling) {
        char hdr[96];
        int hn = snprintf(hdr, sizeof hdr,
                          "[n9-crash] DIED COMPILING SHADER %u:\n", g_compiling);
        w(hdr, (size_t) hn);
        n9_dump_shader(g_compiling);
        w("\n[n9-crash] end of shader\n", 26);
    } else {
        w("[n9-crash] not inside glCompileShader or glLinkProgram\n", 55);
    }

    w("[n9-crash] maps:\n", 17);
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd >= 0) {
        char m[1024];
        ssize_t got;
        while ((got = read(fd, m, sizeof m)) > 0)
            w(m, (size_t) got);
        close(fd);
    }
    w("[n9-crash] end\n", 15);

    signal(sig, SIG_DFL);
    raise(sig);
}

/* Hand epoxy our wrappers for the four calls we care about, and the real
   pointer for everything else. The real dlsym is fetched with dlvsym, which is
   not itself interposed, so there is no recursion. ARM EABI glibc versions it
   GLIBC_2.4, not the GLIBC_2.0 used on older ports. */
void *dlsym(void *handle, const char *symbol)
{
    if (!real_dlsym)
        real_dlsym = (void *(*)(void *, const char *))
            dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.4");
    if (!real_dlsym)
        return NULL;
    if (symbol && getenv("N9_LOG_DLSYM") && (strstr(symbol, "Shader")
        || strstr(symbol, "Program") || strstr(symbol, "Compile"))) {
        char b[128];
        int n = snprintf(b, sizeof b, "[n9-dlsym] %s\n", symbol);
        w(b, (size_t) n);
    }
    if (symbol && !strncmp(symbol, "gl", 2) && handle && handle != RTLD_NEXT
        && handle != RTLD_DEFAULT)
        g_gl_handle = handle;
    if (symbol) {
        if (!strcmp(symbol, "glShaderSource"))  return (void *) glShaderSource;
        if (!strcmp(symbol, "glCompileShader")) return (void *) glCompileShader;
        if (!strcmp(symbol, "glAttachShader"))  return (void *) glAttachShader;
        if (!strcmp(symbol, "glLinkProgram"))   return (void *) glLinkProgram;
    }
    return real_dlsym(handle, symbol);
}

typedef void (*egl_proc)(void);
egl_proc eglGetProcAddress(const char *name)
{
    static egl_proc (*real)(const char *);
    if (!real) real = (egl_proc (*)(const char *)) gl_real("eglGetProcAddress");
    if (name && getenv("N9_LOG_DLSYM") && (strstr(name, "Shader")
        || strstr(name, "Program") || strstr(name, "Compile"))) {
        char b[128];
        int n = snprintf(b, sizeof b, "[n9-eglproc] %s\n", name);
        w(b, (size_t) n);
    }
    if (name) {
        if (!strcmp(name, "glShaderSource"))  return (egl_proc) glShaderSource;
        if (!strcmp(name, "glCompileShader")) return (egl_proc) glCompileShader;
        if (!strcmp(name, "glAttachShader"))  return (egl_proc) glAttachShader;
        if (!strcmp(name, "glLinkProgram"))   return (egl_proc) glLinkProgram;
    }
    return real ? real(name) : NULL;
}

static void install(void);

/* The WPE backend installs its own handler and is loaded AFTER this preload,
 * so its sigaction overwrites ours -- and its handler cannot dump shaders.
 * Re-arm at the first GL call, by which time every library is in. */
static void reclaim_handler(void)
{
    static int done;
    if (!done) { done = 1; install(); }
}

__attribute__((constructor))
static void install(void)
{
    real_dlsym = (void *(*)(void *, const char *))
        dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.4");
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    fprintf(stderr, "[n9-crash] armed in pid %d (dlsym hook %s)\n",
            (int) getpid(), real_dlsym ? "on" : "OFF");
}
