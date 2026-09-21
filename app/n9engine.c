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
 *        set <javascript|images|pagecache> <0|1> | scale <magnification>
 *        set blocker <0|1> | cachemodel <viewer|browser> | clearmemcache
 *        renderscale <pixels per css pixel> | useragent <string>
 *        axis <x> <y> <dx> <dy> | layoutwidth <css px>
 *        text <utf8> | key <keysym> <0|1> | scrollby <dx> <dy>
 *        authok <b64user> <b64pass> | authcancel
 *        dlcancel <uri> | dlsave <uri> | contextmenu <x> <y>
 *   out: ... | ime <0|1> | imerect <x> <y> <w> <h>
 *        clearcache | clearcookies | quit
 *   out: ... | surface <w> <h>
 *   out: url <s> | title <s> | progress <0..1> | loading <0|1> | scroll <y>
 *        nav <canBack> <canForward> | failed <msg> | ready
 *        window <xid> | frame | auth <retry> <host[:port]> <realm>
 *        link <uri> | image <uri> | dlstart <total> <b64path> <uri>
 *        dlprogress <received> <total> <uri> | dlfinish <b64path> <uri>
 *        dlfail <b64error> <uri>
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

/* The WEB process. This is the one that matters: measured on device it peaks
 * at 116-129 MB for a single page against ~150 MB free, and it is the only
 * process whose growth can take the session down. */
#define N9_MEMORY_LIMIT_MB       150
/* The NETWORK process, sized separately. 200 MB was above what the device has
 * free, so the limit could never be reached and the settings never did
 * anything; measured, it peaks around 12 MB. */
#define N9_NET_MEMORY_LIMIT_MB   64
#define N9_CONSERVATIVE_FRACTION 0.33
#define N9_STRICT_FRACTION       0.50
/* Killing the web process loses the page outright; on hardware this slow the
 * user would rather wait through swapping than lose the session. */
#define N9_KILL_FRACTION         0.90

/* Where the shipped ruleset lives. Overridden by N9_BLOCKLIST, which is how it
   is tested without root -- /opt is not writable after a package install. */
#define N9_BLOCKLIST_DEFAULT     "/opt/n9browser/share/webmee/blocklist.json"

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

/*
 * Content blocker.
 *
 * WebKit compiles a Safari content-blocker ruleset into a DFA once and stores
 * it; after that, matching costs the network path almost nothing. This is the
 * one lever that helps with JavaScript ON *and* off -- with scripts stripped,
 * a page's tracking pixels, third-party images and iframes are still fetched.
 *
 * Both store calls are async and there is no synchronous variant, so the
 * filter can land a moment after the first page has started loading. That is
 * accepted rather than worked around: blocking startup on a DFA compile is the
 * worse trade, and the compile only happens when the ruleset changes.
 */
#define N9_FILTER_ID "n9block"
/* Defined with the other environment helpers further down; the blocker reads
   its initial state before that point. */
static gboolean env_flag(const char *name);
static WebKitUserContentManager *content_manager = NULL;
static gint64 filter_t0 = 0;
/* The compiled filter is kept alive for the life of the process so the toggle
   can put it back without recompiling; only the ADD is undone by turning it
   off. Default on -- see docs/performance.md on why an ambiguous measurement
   leaves it on rather than off. */
static WebKitUserContentFilter *active_filter = NULL;
static gboolean blocker_enabled = TRUE;

static void apply_blocker(void)
{
    if (!content_manager || !active_filter) return;
    if (blocker_enabled)
        webkit_user_content_manager_add_filter(content_manager, active_filter);
    else
        webkit_user_content_manager_remove_filter_by_id(content_manager, N9_FILTER_ID);
    fprintf(stderr, "[n9engine] content blocker %s\n",
            blocker_enabled ? "on" : "off");
}

static void filter_ready(WebKitUserContentFilter *filter, const char *how)
{
    if (!filter) return;
    active_filter = filter;          /* ref kept deliberately, see above */
    fprintf(stderr, "[n9engine] content blocker %s in %d ms\n",
            how, (int) ((g_get_monotonic_time() - filter_t0) / 1000));
    apply_blocker();
}

static void on_filter_saved(GObject *src, GAsyncResult *res, gpointer d)
{
    (void) d;
    GError *err = NULL;
    WebKitUserContentFilter *f = webkit_user_content_filter_store_save_from_file_finish(
        WEBKIT_USER_CONTENT_FILTER_STORE(src), res, &err);
    if (!f) {
        /* Say so loudly. A blocker that silently failed to compile looks
           exactly like one that is working on a page with no ads on it. */
        fprintf(stderr, "[n9engine] content blocker FAILED to compile: %s\n",
                err ? err->message : "(no error)");
        g_clear_error(&err);
        return;
    }
    filter_ready(f, "compiled");
}

static void on_filter_loaded(GObject *src, GAsyncResult *res, gpointer d)
{
    WebKitUserContentFilterStore *store = WEBKIT_USER_CONTENT_FILTER_STORE(src);
    WebKitUserContentFilter *f =
        webkit_user_content_filter_store_load_finish(store, res, NULL);
    if (f) {                         /* already compiled on an earlier run */
        filter_ready(f, "loaded");
        g_object_unref(store);
        return;
    }
    /* Not in the store yet, so compile it. This is the slow path and runs
       once; the identifier is fixed, so a changed ruleset needs the store
       directory cleared (deploy.sh does that when it ships a new list). */
    const char *json = (const char *) d;
    GFile *gf = g_file_new_for_path(json);
    filter_t0 = g_get_monotonic_time();
    webkit_user_content_filter_store_save_from_file(
        store, N9_FILTER_ID, gf, NULL, on_filter_saved, NULL);
    g_object_unref(gf);
    g_object_unref(store);
}

static void setup_content_blocker(WebKitUserContentManager *content)
{
    static char json[1024];
    const char *env = getenv("N9_BLOCKLIST");
    g_strlcpy(json, (env && *env) ? env : N9_BLOCKLIST_DEFAULT, sizeof json);
    if (!g_file_test(json, G_FILE_TEST_EXISTS)) {
        fprintf(stderr, "[n9engine] no blocklist at %s -- not blocking\n", json);
        return;
    }
    /* Same shape as N9_NO_JS / N9_NO_IMAGES: the value is tested, not its
       presence, so the shell can say "on" as well as "off", and it is in place
       before the first load rather than arriving after it. */
    blocker_enabled = !env_flag("N9_NO_BLOCKER");
    char *dir = g_build_filename(g_get_user_cache_dir(), "WebMee", "filters", NULL);
    g_mkdir_with_parents(dir, 0700);
    WebKitUserContentFilterStore *store = webkit_user_content_filter_store_new(dir);
    content_manager = content;
    filter_t0 = g_get_monotonic_time();
    fprintf(stderr, "[n9engine] content blocker %s -> %s\n", json, dir);
    webkit_user_content_filter_store_load(store, N9_FILTER_ID, NULL,
                                          on_filter_loaded, json);
    g_free(dir);
}

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

/*
 * Long press on a link.
 *
 * WPE's TouchGestureController turns a press held past 500 ms into a context
 * menu request (contextMenuThreshold in TouchGestureController.cpp), which
 * arrives here as the "context-menu" signal. There is no menu UI in the WPE
 * port at all, so the proposed WebKitContextMenu is of no use to us -- what is
 * worth having is the hit test result, which names the link under the finger.
 * The shell draws the menu with the platform's own, as the stock browser does.
 *
 * Returning TRUE always: it suppresses a menu that would never be drawn, and
 * says the event is handled so nothing else acts on it.
 */
/* TWO arguments, not three. The GTK port and WPE's pre-2022 API pass a GdkEvent
   in the middle, and WebKitWebViewWPE1.cpp declares the signal that way -- but
   this is the 2022 GLib API, so WebKitWebViewWPE2.cpp is what compiles and the
   event parameter is omitted from both the signal and the emit. Declaring the
   extra parameter silently shifts everything along: the hit test lands in the
   user_data slot and the handler sees NULL. */
static gboolean on_context_menu(WebKitWebView *v, WebKitContextMenu *menu,
                                WebKitHitTestResult *hit, gpointer d)
{
    (void) v; (void) menu; (void) d;
    if (!hit)
        return TRUE;
    /* A linked image is both. The link wins: the stock browser offers the link
       menu there, and the image is still reachable by pressing a bare one. */
    if (webkit_hit_test_result_context_is_link(hit)) {
        const char *uri = webkit_hit_test_result_get_link_uri(hit);
        if (uri && *uri)
            emit("link %s", uri);
    } else if (webkit_hit_test_result_context_is_image(hit)) {
        const char *uri = webkit_hit_test_result_get_image_uri(hit);
        if (uri && *uri)
            emit("image %s", uri);
    }
    return TRUE;
}

/*
 * Downloads.
 *
 * Nothing downloaded before this: there was no decide-policy handler, so a link
 * to a file WebKit cannot display simply did nothing, and the shell's
 * DownloadManager was a model driven by a fake timer with no caller.
 *
 * It has to be the engine and not the shell. Qt 4.7.4's QSslSocket cannot
 * complete a handshake with a modern server, so anything the shell fetched over
 * https would fail; the engine already has working TLS through libsoup.
 *
 * Keyed by uri, because that is what the shell's model is keyed by.
 */
static GHashTable *downloads = NULL;      /* uri -> WebKitDownload*, borrowed */

static char *b64dup(const char *s)
{
    return g_base64_encode((const guchar *) (s ? s : ""), s ? strlen(s) : 0);
}

static const char *download_uri(WebKitDownload *dl)
{
    WebKitURIRequest *req = webkit_download_get_request(dl);
    const char *uri = req ? webkit_uri_request_get_uri(req) : NULL;
    return uri ? uri : "";
}

static guint64 download_total(WebKitDownload *dl)
{
    WebKitURIResponse *res = webkit_download_get_response(dl);
    return res ? webkit_uri_response_get_content_length(res) : 0;
}

/* The server's suggested name is the user's filename; only disambiguate it. */
static char *unique_path(const char *suggested)
{
    const char *dir = getenv("N9_DOWNLOAD_DIR");
    if (!dir || !*dir)
        dir = "/home/user/MyDocs/Downloads";
    g_mkdir_with_parents(dir, 0755);

    const char *base = (suggested && *suggested) ? suggested : "download";
    char *path = g_build_filename(dir, base, NULL);
    for (int n = 1; n < 1000 && g_file_test(path, G_FILE_TEST_EXISTS); ++n) {
        g_free(path);
        const char *dot = strrchr(base, '.');
        char *stem = dot ? g_strndup(base, (gsize) (dot - base)) : g_strdup(base);
        char *name = g_strdup_printf("%s-%d%s", stem, n, dot ? dot : "");
        path = g_build_filename(dir, name, NULL);
        g_free(stem);
        g_free(name);
    }
    return path;
}

static gboolean on_decide_destination(WebKitDownload *dl, const char *suggested, gpointer d)
{
    (void) d;
    char *path = unique_path(suggested);
    /* An absolute PATH, not a URI: this is the 2022 GLib API, which asserts
       g_path_is_absolute on it. */
    webkit_download_set_destination(dl, path);

    const char *uri = download_uri(dl);
    g_hash_table_insert(downloads, g_strdup(uri), dl);

    char *b = b64dup(path);
    emit("dlstart %" G_GUINT64_FORMAT " %s %s", download_total(dl), b, uri);
    g_free(b);
    g_free(path);
    return TRUE;                       /* we decided; do not use the default */
}

static void on_download_received(WebKitDownload *dl, guint64 length, gpointer d)
{
    (void) length; (void) d;
    guint64 got = webkit_download_get_received_data_length(dl);
    /* received-data fires per chunk. One protocol line each would flood a pipe
       the shell also uses for frames, and the shell throttles the engine by not
       draining it -- so report per 64 KiB instead. */
    guint64 last = (guint64) GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(dl), "n9-last"));
    if (got - last < 65536)
        return;
    g_object_set_data(G_OBJECT(dl), "n9-last", GSIZE_TO_POINTER((gsize) got));
    emit("dlprogress %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %s",
         got, download_total(dl), download_uri(dl));
}

static void on_download_finished(WebKitDownload *dl, gpointer d)
{
    (void) d;
    const char *dest = webkit_download_get_destination(dl);
    char *b = b64dup(dest);
    emit("dlfinish %s %s", b, download_uri(dl));
    g_free(b);
    g_hash_table_remove(downloads, download_uri(dl));
}

static void on_download_failed(WebKitDownload *dl, GError *error, gpointer d)
{
    (void) d;
    /* A cancel arrives here too; the shell already knows it asked for that. */
    char *b = b64dup(error ? error->message : "download failed");
    emit("dlfail %s %s", b, download_uri(dl));
    g_free(b);
    g_hash_table_remove(downloads, download_uri(dl));
}

static void on_download_started(WebKitNetworkSession *sess, WebKitDownload *dl, gpointer d)
{
    (void) sess; (void) d;
    g_signal_connect(dl, "decide-destination", G_CALLBACK(on_decide_destination), NULL);
    g_signal_connect(dl, "received-data", G_CALLBACK(on_download_received), NULL);
    g_signal_connect(dl, "finished", G_CALLBACK(on_download_finished), NULL);
    g_signal_connect(dl, "failed", G_CALLBACK(on_download_failed), NULL);
}

/*
 * Turn a response WebKit cannot display into a download.
 *
 * Without this the click on a .zip is simply dropped: WebKit asks what to do,
 * nothing answers, and the default is to try to display content it has already
 * said it cannot.
 */
static gboolean on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type, gpointer d)
{
    (void) v; (void) d;
    if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
        return FALSE;
    WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(decision);
    if (webkit_response_policy_decision_is_mime_type_supported(r))
        return FALSE;                  /* let it render as usual */
    webkit_policy_decision_download(decision);
    return TRUE;
}

static gboolean on_load_failed(WebKitWebView *v, WebKitLoadEvent e,
                               const char *uri, GError *error, gpointer d)
{
    (void) v; (void) e; (void) uri; (void) d;
    emit("failed %s", error ? error->message : "unknown error");
    emit("loading 0");
    return FALSE;
}

/*
 * HTTP authentication.
 *
 * The WPE port's default "authenticate" handler is `return FALSE` -- there is
 * no built-in dialog, and an unanswered challenge just loads the server's 401.
 * That is what "I cannot open password-protected sites" looks like: not an
 * error, a page saying Unauthorized.
 *
 * The prompt belongs to the shell, so the request is parked here and answered
 * by command. Returning TRUE with a ref held is the documented way to handle
 * this signal asynchronously.
 */
static WebKitAuthenticationRequest *auth_request = NULL;

static gboolean on_authenticate(WebKitWebView *v, WebKitAuthenticationRequest *req,
                                gpointer d)
{
    (void) v; (void) d;

    /* Only the schemes a username and a password can answer. A client
       certificate or a PIN needs something this prompt does not have, and
       claiming to handle those would leave the request hanging for good. */
    switch (webkit_authentication_request_get_scheme(req)) {
    case WEBKIT_AUTHENTICATION_SCHEME_DEFAULT:
    case WEBKIT_AUTHENTICATION_SCHEME_HTTP_BASIC:
    case WEBKIT_AUTHENTICATION_SCHEME_HTTP_DIGEST:
    case WEBKIT_AUTHENTICATION_SCHEME_NTLM:
    case WEBKIT_AUTHENTICATION_SCHEME_NEGOTIATE:
        break;
    default:
        return FALSE;
    }

    /* ponytail: one challenge at a time. A second arriving while the first is
       still on screen continues without credentials rather than queueing --
       add a queue if that turns out to matter. It mostly cannot: the answer is
       stored FOR_SESSION, so the first one covers the whole protection space
       and the rest of the page's subresources never ask. */
    if (auth_request) {
        webkit_authentication_request_authenticate(auth_request, NULL);
        g_object_unref(auth_request);
    }
    auth_request = g_object_ref(req);

    /* The port is worth showing only when it is not the scheme's own -- a
       prompt that says "example.com:443" reads like a different host. */
    guint port = webkit_authentication_request_get_port(req);
    const char *host = webkit_authentication_request_get_host(req);
    const char *realm = webkit_authentication_request_get_realm(req);
    char *where = (port && port != 80 && port != 443)
        ? g_strdup_printf("%s:%u", host ? host : "", port)
        : g_strdup(host ? host : "");

    emit("auth %d %s %s",
         webkit_authentication_request_is_retry(req) ? 1 : 0,
         where, realm ? realm : "");
    g_free(where);
    return TRUE;
}

/*
 * Answer the parked challenge, once. NULL user means "no credentials".
 *
 * Not webkit_authentication_request_cancel(): that cancels the page load and
 * produces a network error, where continuing with no credentials loads the
 * server's own 401 body -- which is what every other browser shows and says
 * considerably more. Clearing the pointer here is what stops a second answer
 * completing the same challenge twice.
 */
static void answer_auth(const char *user, const char *password)
{
    if (!auth_request)
        return;
    if (user) {
        /* FOR_SESSION, not NONE: without it WebKit asks again for every
           subresource in the same realm, and again on the next page of the
           site. PERMANENT would additionally want a secret store this device
           does not have. */
        WebKitCredential *c = webkit_credential_new(user, password ? password : "",
                                                    WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION);
        webkit_authentication_request_authenticate(auth_request, c);
        webkit_credential_free(c);
    } else {
        webkit_authentication_request_authenticate(auth_request, NULL);
    }
    g_clear_object(&auth_request);
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
/*
 * Event timestamps, in real milliseconds.
 *
 * This used to be a counter bumped by 16 per EVENT, with a comment claiming it
 * was milliseconds. WPE's TouchGestureController measures real durations
 * against it -- 500 ms to call a press a context menu, 200 ms to capture a
 * gesture as a scroll -- so a press held for a full second still read as 16 ms
 * apart and was always a click. Long press could not work at all.
 *
 * Truncating to 32 bits wraps after ~49 days of uptime; the controller only
 * ever takes differences, so a wrap costs one confused gesture.
 */
static uint32_t now_ms(void)
{
    return (uint32_t) (g_get_monotonic_time() / 1000);
}

static void dispatch_touch(enum wpe_input_touch_event_type type, int slot, int x, int y)
{
    if (slot < 0 || slot >= N9_MAX_TOUCH) return;
    struct wpe_view_backend *b =
        webkit_web_view_backend_get_wpe_backend(webkit_web_view_get_backend(view));
    if (!b) return;

    const uint32_t t = now_ms();

    touchpoints[slot].type = type;
    touchpoints[slot].time = t;
    touchpoints[slot].id = slot;
    touchpoints[slot].x = x;
    touchpoints[slot].y = y;

    struct wpe_input_touch_event event = {
        touchpoints, N9_MAX_TOUCH, type, slot, t, 0
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
    /* And so did the challenge behind any prompt still on screen. The shell
       closes it on this event; drop our ref so nothing tries to answer a
       request whose load no longer exists. */
    g_clear_object(&auth_request);
}

/*
 * "JavaScript off" means the PAGE's scripts are off, not ours.
 *
 * enable-javascript=FALSE kills the whole JS engine, which also silences the
 * injected scroll reporter below and makes evaluate_javascript ("scrollby") a
 * no-op -- so with the default settings the URL bar never hid, the caret was
 * never lifted clear of the keyboard, and scroll position could not be read
 * back at all. It looked like those features were broken; they were never
 * reached.
 *
 * enable-javascript-markup=FALSE strips every script element and on* attribute
 * during parsing instead, so nothing the page ships ever runs -- which is the
 * whole of the 3.6x frame win -- while user scripts and evaluate_javascript
 * keep working. (Since 2.24; documented as "executing JavaScript is still
 * allowed if enable-javascript is TRUE".)
 */
static void set_javascript(WebKitSettings *st, gboolean on)
{
    webkit_settings_set_enable_javascript(st, TRUE);
    webkit_settings_set_enable_javascript_markup(st, on);
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
       rare enough to trace -- except the one line carrying a password, which
       the shell keeps a copy of on disk. */
    if (!strncmp(line, "authok ", 7))
        fprintf(stderr, "[n9engine] cmd: authok <credentials>\n");
    else if (strncmp(line, "touch ", 6))
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
    } else if (!strncmp(line, "authok ", 7)) {
        /* Base64 on both fields. The protocol is whitespace-separated with no
           escaping and only the LAST field may contain spaces -- a password
           routinely does, and so can a username. */
        char **f = g_strsplit(line + 7, " ", 2);
        if (f[0] && f[1]) {
            gsize ul = 0, pl = 0;
            guchar *u = g_base64_decode(f[0], &ul);
            guchar *p = g_base64_decode(f[1], &pl);
            /* g_base64_decode does not NUL-terminate. */
            char *us = g_strndup((const char *) u, ul);
            char *ps = g_strndup((const char *) p, pl);
            answer_auth(us, ps);
            g_free(us); g_free(ps); g_free(u); g_free(p);
        }
        g_strfreev(f);
    } else if (!strncmp(line, "contextmenu ", 12)) {
        /*
         * Raise the context menu at a point, in surface device pixels.
         *
         * WPE only decides a press was a long one when the finger LIFTS --
         * TouchGestureController returns its ContextMenuEvent from the `up`
         * branch -- so a menu driven by it cannot appear while the finger is
         * still down. The shell times the press itself and asks for the menu
         * at the threshold instead.
         *
         * The motion first, then the button: WebCore hit-tests wherever it
         * last believed the pointer to be, so without the move the menu is
         * raised for the wrong node. Button 2 is the right button, which is
         * what WebCore raises a context menu from.
         */
        int x, y;
        if (sscanf(line + 12, "%d %d", &x, &y) == 2) {
            struct wpe_view_backend *b =
                webkit_web_view_backend_get_wpe_backend(webkit_web_view_get_backend(view));
            if (b) {
                struct wpe_input_pointer_event e;
                memset(&e, 0, sizeof e);
                e.time = now_ms();
                e.x = x;
                e.y = y;
                e.type = wpe_input_pointer_event_type_motion;
                wpe_view_backend_dispatch_pointer_event(b, &e);
                e.type = wpe_input_pointer_event_type_button;
                e.button = 2;
                e.state = 1;
                e.modifiers = wpe_input_pointer_modifier_button2;
                wpe_view_backend_dispatch_pointer_event(b, &e);
                e.state = 0;
                e.modifiers = 0;
                wpe_view_backend_dispatch_pointer_event(b, &e);
            }
        }
    } else if (!strncmp(line, "dlsave ", 7)) {
        /* Download something the user chose rather than something the page
           navigated to. Same machinery: download-started fires either way. */
        if (session)
            g_object_unref(webkit_network_session_download_uri(session, line + 7));
    } else if (!strncmp(line, "dlcancel ", 9)) {
        WebKitDownload *dl = g_hash_table_lookup(downloads, line + 9);
        if (dl)
            webkit_download_cancel(dl);
    } else if (!strcmp(line, "authcancel")) {
        answer_auth(NULL, NULL);
    } else if (!strncmp(line, "key ", 4)) {
        unsigned keysym; int pressed;
        if (sscanf(line + 4, "%u %d", &keysym, &pressed) == 2) {
            struct wpe_view_backend *b =
                webkit_web_view_backend_get_wpe_backend(webkit_web_view_get_backend(view));
            if (b) {
                struct wpe_input_keyboard_event e;
                memset(&e, 0, sizeof e);
                e.time = now_ms();
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
                e.base.time = now_ms();
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
                set_javascript(st, val ? TRUE : FALSE);
            else if (!strcmp(key, "images"))
                webkit_settings_set_auto_load_images(st, val ? TRUE : FALSE);
            else if (!strcmp(key, "pagecache"))
                webkit_settings_set_enable_page_cache(st, val ? TRUE : FALSE);
            else if (!strcmp(key, "blocker")) {
                blocker_enabled = val ? TRUE : FALSE;
                apply_blocker();     /* no-op until the filter has compiled */
            }
        }
    } else if (!strncmp(line, "cachemodel ", 11)) {
        /* DOCUMENT_VIEWER is the one that disables the memory cache outright:
           WebProcessPoolGLib.cpp sets parameters.memoryCacheDisabled when the
           cache model is that. BROWSER is the normal state. These are the only
           runtime cache controls the GLib API has -- there is no setter for a
           cache SIZE, which is why the shell's three size sliders could never
           have worked. */
        int viewer = !strcmp(line + 11, "viewer");
        webkit_web_context_set_cache_model(
            webkit_web_view_get_context(view),
            viewer ? WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER
                   : WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
        fprintf(stderr, "[n9engine] cache model %s\n", viewer ? "viewer" : "browser");
    } else if (!strcmp(line, "clearmemcache")) {
        /* The MEMORY cache only. `clearcache` drops the disk cache too, which
           frees no RAM and costs refetches over 3G -- the wrong trade when the
           point is to shed memory. */
        if (session)
            webkit_website_data_manager_clear(
                webkit_network_session_get_website_data_manager(session),
                WEBKIT_WEBSITE_DATA_MEMORY_CACHE, 0, NULL, NULL, NULL);
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
        /* Strip the line TERMINATOR only. g_strchomp() takes every trailing
           space with it, and the last field of this protocol is allowed to be
           or to end in one: a space typed into a page is sent on its own as
           "text  ", which chomped down to "text" and matched no command at
           all. Every space was dropped, silently. */
        gsize n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
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

    /* The NETWORK process. Despite the generic-sounding name this call reaches
       only that one: the docs are explicit that "the periodic check for used
       memory is disabled by default on network processes". */
    WebKitMemoryPressureSettings *pressure = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(pressure, N9_NET_MEMORY_LIMIT_MB);
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

    /* The WEB process gets its own pressure settings, and this is the only way
       to give it any. An earlier comment here said 2.44 exposes no equivalent
       for the web process; that was wrong. WebKitWebContext has had a
       `memory-pressure-settings` property since 2.34. It is CONSTRUCT_ONLY, so
       it cannot be set on a context that already exists -- hence g_object_new
       rather than webkit_web_context_new(). Verified in the tree: the property
       feeds configuration.setMemoryPressureHandlerConfiguration(), which
       reaches WebProcessCreationParameters for every web process the context
       creates.
       This is what makes WebKit's own releaseMemory() run INSIDE the web
       process -- dropping decoded images, running a JSC GC, shrinking the
       caches. Nothing in the shell can reach any of that.
       Do NOT set WEBKIT_DISABLE_MEMORY_PRESSURE_MONITOR=1 to quieten the
       UI-process poller: the same flag sets shouldSuppressMemoryPressureHandler
       and turns off the handler being installed here. */
    WebKitMemoryPressureSettings *web_pressure = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(web_pressure, N9_MEMORY_LIMIT_MB);
    webkit_memory_pressure_settings_set_conservative_threshold(web_pressure, N9_CONSERVATIVE_FRACTION);
    webkit_memory_pressure_settings_set_strict_threshold(web_pressure, N9_STRICT_FRACTION);
    webkit_memory_pressure_settings_set_kill_threshold(web_pressure, N9_KILL_FRACTION);
    WebKitWebContext *context = WEBKIT_WEB_CONTEXT(
        g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
                     "memory-pressure-settings", web_pressure, NULL));
    webkit_memory_pressure_settings_free(web_pressure);
    /* Say what was actually applied. A setting that looks wired and is not has
       cost this project four separate controls already. */
    fprintf(stderr, "[n9engine] web process memory limit %d MB "
            "(conservative %.2f strict %.2f kill %.2f), network %d MB\n",
            N9_MEMORY_LIMIT_MB, N9_CONSERVATIVE_FRACTION, N9_STRICT_FRACTION,
            N9_KILL_FRACTION, N9_NET_MEMORY_LIMIT_MB);
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
    setup_content_blocker(content);

    /*
     * Process-swap-on-navigation is ON and cannot be turned off from here.
     *
     * It starts a fresh web process for every cross-site navigation and keeps
     * the old ones a while. Measured on device, five loads alternating between
     * two hosts: four live WPEWebProcess at once, free memory 101 -> 58 MB, a
     * 40-second stall, and one navigation lost outright. Ordinary browsing
     * hits this every few links; tab switching hits it every time.
     *
     * The preference is ProcessSwapOnCrossSiteNavigationEnabled, and
     * webkit_settings_set_feature_enabled() looks like the way to reach it --
     * it is not. The string is in the library, but the GLib feature list only
     * exposes preferences with a WebCore binding and this one has
     * `webcoreBinding: none, exposed: [ WebKit ]`, so it never appears in
     * webkit_settings_get_all_features(). Verified on device: the lookup
     * misses and the process count is unchanged. Turning it off means patching
     * APIProcessPoolConfiguration's default in deps/build-webkit.sh and
     * rebuilding WebKit.
     *
     * The process CACHE is a separate thing and is already off by itself:
     * WebProcessCache disables itself below 3 GB of RAM.
     */
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
    g_signal_connect(view, "authenticate", G_CALLBACK(on_authenticate), NULL);
    g_signal_connect(view, "context-menu", G_CALLBACK(on_context_menu), NULL);
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    downloads = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_signal_connect(session, "download-started", G_CALLBACK(on_download_started), NULL);
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
            set_javascript(st, FALSE);
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

    /*
     * "No page yet" is a state, not a document, so do not load one.
     *
     * about:blank was standing in for it, which put a url in the address bar
     * that has to be cleared before anything can be typed, and rendered a
     * document to show nothing.
     *
     * It does NOT save a web process, measured: one is already running at idle
     * with nothing loaded, because WebKit brings it up with the VIEW rather
     * than with the first load -- `surface` and `window` are both emitted
     * before any `loading 1`. What it saves is the document load and the first
     * paint, and what it buys is an empty address bar.
     *
     * Deferring is otherwise safe: the backend takes its size from the
     * `geometry` command whether or not a page is loading. With nothing
     * painted, no frame arrives, so the shell keeps its own start page up --
     * which is the intended look for an empty tab.
     */
    emit("ready");
    if (g_strcmp0(url, "about:blank") != 0) {
        fprintf(stderr, "[n9engine] loading %s (memory limit %d MB)\n",
                url, N9_MEMORY_LIMIT_MB);
        webkit_web_view_load_uri(view, url);
    } else {
        fprintf(stderr, "[n9engine] no start page, waiting for a load "
                        "(memory limit %d MB)\n", N9_MEMORY_LIMIT_MB);
    }

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
