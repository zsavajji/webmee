// GLib canary. ICU proved the toolchain can build modern C++ against a 2009
// sysroot; this proves GLib's *runtime* assumptions hold on Linux 2.6.32.
//
// GLib's configure skipped statx, memfd_create, getrandom, copy_file_range,
// renameat2 and close_range, but compiled in pipe2 -- so pipe2 is the one
// modern syscall that has to be checked against this kernel. The rest of the
// checks exercise the subsystems WebKit leans on hardest: the main loop, the
// GObject type system (which needs libffi closures), GRegex (PCRE2), GVariant,
// and the file-write path we patched to stop using fallocate().

#include <glib.h>
#include <glib-object.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static void report(const char *name, gboolean ok, const char *detail)
{
    printf("[%s] %-28s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
    if (!ok) ++failures;
}

static gboolean quit_cb(gpointer loop) { g_main_loop_quit(loop); return G_SOURCE_REMOVE; }

int main(void)
{
    printf("GLib compiled : %d.%d.%d\n", GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
    printf("GLib runtime  : %u.%u.%u\n\n", glib_major_version, glib_minor_version, glib_micro_version);

    // pipe2 -- the one modern syscall GLib compiled in
    {
        GError *err = NULL;
        int fds[2] = { -1, -1 };
        gboolean ok = g_unix_open_pipe(fds, FD_CLOEXEC, &err);
        if (ok) {
            char buf[8] = {0};
            ok = (write(fds[1], "ok", 2) == 2) && (read(fds[0], buf, 2) == 2)
                 && strncmp(buf, "ok", 2) == 0;
            close(fds[0]); close(fds[1]);
        }
        report("g_unix_open_pipe (pipe2)", ok, err ? err->message : NULL);
        g_clear_error(&err);
    }

    // main loop: GLib's poll/epoll source machinery
    {
        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        g_timeout_add(30, quit_cb, loop);
        gint64 t0 = g_get_monotonic_time();
        g_main_loop_run(loop);
        gint64 dt = g_get_monotonic_time() - t0;
        g_main_loop_unref(loop);
        char d[64]; g_snprintf(d, sizeof d, "%" G_GINT64_FORMAT " us", dt);
        report("GMainLoop + timeout", dt >= 20000 && dt < 5000000, d);
    }

    // GObject type system -- needs libffi closures to work
    {
        GObject *o = g_object_new(G_TYPE_OBJECT, NULL);
        gboolean ok = G_IS_OBJECT(o) && strcmp(G_OBJECT_TYPE_NAME(o), "GObject") == 0;
        if (o) g_object_unref(o);
        report("GObject type system", ok, NULL);
    }

    // GRegex -> PCRE2
    {
        GError *err = NULL;
        GRegex *re = g_regex_new("^(\\w+)-(\\d+)$", 0, 0, &err);
        GMatchInfo *mi = NULL;
        gboolean ok = FALSE;
        if (re && g_regex_match(re, "harmattan-2011", 0, &mi)) {
            gchar *a = g_match_info_fetch(mi, 1), *b = g_match_info_fetch(mi, 2);
            ok = a && b && strcmp(a, "harmattan") == 0 && strcmp(b, "2011") == 0;
            g_free(a); g_free(b);
        }
        if (mi) g_match_info_free(mi);
        if (re) g_regex_unref(re);
        report("GRegex (PCRE2)", ok, err ? err->message : NULL);
        g_clear_error(&err);
    }

    // GVariant roundtrip -- GIO's whole IPC layer rests on this
    {
        GVariant *v = g_variant_new("(sit)", "n9", -7, (guint64) 1 << 40);
        gchar *txt = g_variant_print(v, TRUE);
        const gchar *s = NULL; gint32 i = 0; guint64 t = 0;
        g_variant_get(v, "(&sit)", &s, &i, &t);
        gboolean ok = s && strcmp(s, "n9") == 0 && i == -7 && t == ((guint64) 1 << 40);
        report("GVariant roundtrip", ok, txt);
        g_free(txt); g_variant_unref(v);
    }

    // the write path we patched (fallocate removed) must still be correct
    {
        GError *err = NULL;
        gchar *path = g_build_filename(g_get_tmp_dir(), "n9glibprobe.txt", NULL);
        const gchar *payload = "flexbox and grid, eventually";
        gboolean ok = g_file_set_contents(path, payload, -1, &err);
        if (ok) {
            gchar *back = NULL; gsize len = 0;
            ok = g_file_get_contents(path, &back, &len, &err)
                 && len == strlen(payload) && strcmp(back, payload) == 0;
            g_free(back);
        }
        g_unlink(path);
        report("g_file_set_contents", ok, err ? err->message : NULL);
        g_clear_error(&err); g_free(path);
    }

    // GIO smoke test -- GFile is the layer WebKit uses for local resources
    {
        GFile *f = g_file_new_for_path("/etc");
        gchar *base = g_file_get_basename(f);
        gboolean ok = base && strcmp(base, "etc") == 0 && g_file_query_exists(f, NULL);
        report("GIO GFile", ok, NULL);
        g_free(base); g_object_unref(f);
    }

    printf("\n%s (%d failure%s)\n", failures ? "NOT CLEAR" : "ALL CLEAR",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
