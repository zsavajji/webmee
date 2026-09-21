#include "browserengine.h"
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QDirIterator>
#include <QSettings>
#include <QDebug>
#include <QX11Info>
#include <QApplication>
#include <QClipboard>
#include <MDataUri>
#include <maemo-meegotouch-interfaces/shareuiinterface.h>
#include <X11/Xlib.h>
#include <stdio.h>

// Where the engine and its libraries live on device.
// Stays /opt/n9browser even though the app is WebMee: WebKit bakes this path
// in as PKGLIBEXECDIR at compile time and there is no runtime override in a
// release build -- WEBKIT_EXEC_PATH is inside #if ENABLE(DEVELOPER_MODE) and is
// compiled out. Moving it means recompiling Source/WebKit, which buys nothing a
// user can see.
static const char *kEnginePrefix = "/opt/n9browser";

// XK_Return. The QML side sends the same keysym for a real Return key press;
// this is for the newline the virtual keyboard commits as text instead.
static const int kKeysymReturn = 0xFF0D;

// One place for the file and one for the write. The recurring bug in this
// project is a setting that updates a member and reaches the engine but is
// never written down, or is written under a key nothing reads back -- and both
// look exactly like a working setting until the app is restarted. Anything
// added here has to appear in BOTH the constructor's load and a persist() call,
// so keep the two lists next to each other.
static QString settingsDir()
{
    return QDir::homePath() + "/.config/WebMee";
}

static QString settingsFile()
{
    return settingsDir() + "/settings.conf";
}

// Paths and error strings are base64 on the wire: both are free text that can
// contain a space, and only the last field of a message may.
static QString fromB64(const QString &s)
{
    return QString::fromUtf8(QByteArray::fromBase64(s.toLatin1()));
}

static void persist(const char *key, const QVariant &value)
{
    QSettings(settingsFile(), QSettings::IniFormat).setValue(key, value);
}

BrowserEngine::BrowserEngine(QObject *parent)
    : QObject(parent)
    , m_progress(0)
    , m_loading(false)
    , m_canGoBack(false)
    , m_canGoForward(false)
    , m_historyIndex(-1)
    // JavaScript defaults OFF on this device. Measured: identical CPU with it on
    // or off, and 3.6x the frames with it off -- a page's scripts occupy the one
    // core rather than burning it. Persisted, so this is only the value a fresh
    // install starts from; Settings overrides it and the choice sticks.
    , m_javascript(false)
    , m_images(true)
    // On by default. The one measurement taken was on a network that already
    // filters ads upstream, so it could not show the gain; an ambiguous result
    // leaves the protective default in place rather than removing it.
    , m_blocker(true)
    , m_suggestions(true)
    , m_uaPreset("mobile")
    , m_searchEngine("DuckDuckGo")
    , m_pageCacheOn(true)
    , m_memoryCacheOn(true)
    , m_zoom(0)
    , m_renderScale(1.0)
    , m_layoutWidth(345)
    , m_scrollY(0)
    , m_caretY(0)
    , m_caretH(0)
    , m_appliedLayoutWidth(0)
    , m_appliedRenderScale(0)
    , m_appliedJavascript(true)
    , m_appliedImages(true)
    , m_appliedBlocker(true)
    , m_engine(0)
    , m_engineWindow(0)
    , m_sawFrame(false)
    , m_parentWindow(0)
{
    // /home/user/.cache, not MyDocs: MyDocs is FAT and exported over USB, so a
    // cache there would be visible clutter and could vanish mid-session when
    // the phone is plugged into a PC.
    // QSettings will not create this, and BrowserEngine is constructed before
    // the stores that do -- so do not rely on their order.
    QDir().mkpath(settingsDir());
    {
        QSettings cfg(settingsFile(), QSettings::IniFormat);
        m_searchEngine = cfg.value("search/engine", m_searchEngine).toString();
        m_renderScale = cfg.value("render/scale", m_renderScale).toDouble();
        m_uaPreset = cfg.value("ua/preset", m_uaPreset).toString();
        m_layoutWidth = cfg.value("layout/width", m_layoutWidth).toInt();
        m_javascript = cfg.value("content/javascript", m_javascript).toBool();
        m_images = cfg.value("content/images", m_images).toBool();
        m_blocker = cfg.value("content/blocker", m_blocker).toBool();
        m_suggestions = cfg.value("suggestions/enabled", m_suggestions).toBool();
        // An install from before the base width changed has 320 persisted, and
        // the settings page no longer offers it -- it would show 345 selected
        // while the engine used 320. Migrate rather than leave them disagreeing.
        if (m_layoutWidth == 320)
            m_layoutWidth = 345;
    }

    m_diskCachePath = QDir::homePath() + "/.cache/WebMee";
    m_dataPath = QDir::homePath() + "/.local/share/WebMee";
    QDir().mkpath(m_diskCachePath);
    QDir().mkpath(m_dataPath);
}

void BrowserEngine::startEngine()
{
    if (m_engine)
        return;

    m_log.setFileName(QDir::homePath() + "/.cache/WebMee/engine.log");
    if (m_log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        m_log.write("--- WebMee engine log ---\n");

    m_engine = new QProcess(this);
    // Keep the channels separate and relay stderr by hand. Qt 4 has no
    // ForwardedErrorChannel (that is Qt 5), and ForwardedChannels would forward
    // stdout too -- which is the protocol, not diagnostics.
    m_engine->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_engine, SIGNAL(readyReadStandardError()), SLOT(onEngineStderr()));
    connect(m_engine, SIGNAL(readyReadStandardOutput()), SLOT(onEngineOutput()));
    connect(m_engine, SIGNAL(error(QProcess::ProcessError)),
            SLOT(onEngineError(QProcess::ProcessError)));
    connect(m_engine, SIGNAL(finished(int, QProcess::ExitStatus)),
            SLOT(onEngineFinished(int, QProcess::ExitStatus)));

    // Read before the environment is built, so the hook, the engine and the UI
    // all agree. "=0" means on: with JavaScript defaulting off, the useful
    // override is turning it back ON for a measurement.
    const QByteArray jsHook = qgetenv("N9_NO_JS");
    if (!jsHook.isEmpty())
        m_javascript = (jsHook == "0");
    const QByteArray imgHook = qgetenv("N9_NO_IMAGES");
    if (!imgHook.isEmpty())
        m_images = (imgHook == "0");

    QStringList env = QProcess::systemEnvironment();
    // applauncherd's environment has no DISPLAY -- Qt reaches X through the
    // booster, the engine cannot. Pass the display Qt is actually on rather
    // than assuming ":0": the shell redirects the engine's window with
    // XComposite, which only works if both are on the same server.
    if (Display *dpy = QX11Info::display())
        env << QString("DISPLAY=%1").arg(QString::fromLatin1(DisplayString(dpy)));
    // The engine, not the shell, owns these directories -- it hands them to
    // WebKit's network session. Without them WebKit silently used its own
    // default location and the cache page reported on a directory nothing
    // ever wrote to.
    // Before the first load, not after: the very first request decides which
    // layout a site serves.
    env << QString("N9_USER_AGENT=%1").arg(userAgentString());
    env << QString("N9_LAYOUT_WIDTH=%1").arg(m_layoutWidth);
    // Same reason, and the one that actually matters now that JavaScript
    // defaults off: "ready" is emitted immediately before load_uri, so a
    // "set javascript 0" sent in reply lands after the first page has already
    // started loading with scripts enabled. Always a value, never bare presence
    // -- the engine has to be able to hear "on" too.
    // The list is seeded from our own environment, so drop any inherited copy
    // rather than depend on which of two entries for one key the child sees.
    for (int i = env.size() - 1; i >= 0; --i)
        if (env.at(i).startsWith("N9_NO_JS=") || env.at(i).startsWith("N9_NO_IMAGES=")
            || env.at(i).startsWith("N9_NO_BLOCKER="))
            env.removeAt(i);
    env << QString("N9_NO_JS=%1").arg(m_javascript ? 0 : 1);
    env << QString("N9_NO_IMAGES=%1").arg(m_images ? 0 : 1);
    // Before the first load, like the other two: a filter added in reply to
    // `ready` lands after load_uri has gone out, so the first page of every
    // launch would be unfiltered.
    env << QString("N9_NO_BLOCKER=%1").arg(m_blocker ? 0 : 1);
    env << QString("N9_CACHE_DIR=%1").arg(m_diskCachePath);
    env << QString("N9_DATA_DIR=%1").arg(m_dataPath);
    if (!m_downloadDir.isEmpty())
        env << QString("N9_DOWNLOAD_DIR=%1").arg(m_downloadDir);
    // Prepend any inherited path rather than replacing it: since libwpe
    // resolves the backend by soname, this is the only way to test a freshly
    // built one against the installed tree without root.
    const QString inherited = QString::fromLocal8Bit(qgetenv("LD_LIBRARY_PATH"));
    env << QString("LD_LIBRARY_PATH=%1%2/lib")
           .arg(inherited.isEmpty() ? QString() : inherited + ":")
           .arg(kEnginePrefix);
    env << QString("FONTCONFIG_FILE=%1/etc/fonts/fonts.conf").arg(kEnginePrefix);
    // NOT how libwpe picks the backend -- it was built with a compile-time
    // default and ignores the environment (the variable it would read is
    // WPE_BACKEND_LIBRARY, and only when no default was compiled in). This is
    // solely so the engine can dlopen the already-loaded module and dlsym
    // n9_backend_move_resize out of it.
    env << QString("WPE_BACKEND=%1/lib/libWPEBackend-n9.so").arg(kEnginePrefix);
    // TLS. GIO finds its modules by a path compiled into GLib, which points at
    // the build-time prefix; say it out loud so the tree works wherever it is
    // unpacked. Without the module every https:// URL fails outright.
    env << QString("GIO_MODULE_DIR=%1/lib/gio/modules").arg(kEnginePrefix);
    // OpenSSL's compiled-in openssldir is inside our own tree, and the device's
    // 2011 CA store is too old to verify most of the web; point at the bundle
    // deploy.sh ships.
    env << QString("SSL_CERT_FILE=%1/etc/ssl/ca-certificates.crt").arg(kEnginePrefix);
    // How the engine finds our window and the rectangle to fill. Read by the
    // backend module at window-creation time.
    if (m_parentWindow)
        env << QString("N9_PARENT_WINDOW=%1").arg(m_parentWindow);
    if (!m_viewport.isEmpty())
        env << QString("N9_VIEW_RECT=0,0,%1,%2")
               .arg(m_viewport.width()).arg(m_viewport.height());
    m_engine->setEnvironment(env);

    // N9_ENGINE lets a freshly built engine be tested against an installed
    // tree, which is otherwise root-owned and not writable for iteration.
    QString exe = QString::fromLocal8Bit(qgetenv("N9_ENGINE"));
    if (exe.isEmpty())
        exe = QString("%1/bin/webmee-engine").arg(kEnginePrefix);
    if (!QFileInfo(exe).isExecutable()) {
        qWarning() << "BrowserEngine: no engine at" << exe;
        return;
    }
    // N9_STARTUP_URL exists so the whole shell-to-engine path can be exercised
    // without going through the touchscreen.
    QString startup = QString::fromLocal8Bit(qgetenv("N9_STARTUP_URL"));
    if (startup.isEmpty())
        startup = "about:blank";
    m_engine->start(exe, QStringList() << startup);
}

void BrowserEngine::send(const QString &command)
{
    if (!m_engine || m_engine->state() != QProcess::Running)
        return;
    m_engine->write(command.toUtf8() + "\n");
}

void BrowserEngine::onEngineOutput()
{
    m_pending += m_engine->readAllStandardOutput();
    // The engine flushes per line, but a read can still split one: keep any
    // trailing partial line for the next round.
    int nl;
    while ((nl = m_pending.indexOf('\n')) >= 0) {
        QByteArray line = m_pending.left(nl);
        m_pending.remove(0, nl + 1);
        handleEvent(QString::fromUtf8(line).trimmed());
    }
}

void BrowserEngine::handleEvent(const QString &line)
{
    if (line.isEmpty())
        return;

    const int sp = line.indexOf(' ');
    const QString verb = sp < 0 ? line : line.left(sp);
    const QString arg = sp < 0 ? QString() : line.mid(sp + 1);

    if (verb == "url") {
        if (m_url != arg) { m_url = arg; emit urlChanged(); }
        if (m_scrollY) { m_scrollY = 0; emit scrollChanged(); }
    } else if (verb == "title") {
        if (m_title != arg) {
            m_title = arg;
            emit titleChanged();
            // The title arrives after the load commits, so this is the first
            // point where a history entry is worth writing.
            emit pageVisited(m_url, m_title);
        }
    } else if (verb == "progress") {
        qreal p = arg.toDouble();
        if (!qFuzzyCompare(m_progress + 1.0, p + 1.0)) {
            m_progress = p;
            emit loadProgressChanged();
        }
    } else if (verb == "loading") {
        bool l = (arg.toInt() != 0);
        if (m_loading != l) { m_loading = l; emit loadingChanged(); }
    } else if (verb == "nav") {
        const QStringList f = arg.split(' ');
        if (f.size() == 2) {
            m_canGoBack = (f.at(0).toInt() != 0);
            m_canGoForward = (f.at(1).toInt() != 0);
            emit historyChanged();
        }
    } else if (verb == "failed") {
        qWarning() << "BrowserEngine: load failed:" << arg;
        if (arg.startsWith("web process")) {
            if (m_log.isOpen()) {
                m_log.write(QString("*** %1\n").arg(arg).toUtf8());
                m_log.flush();
            }
            emit webProcessCrashed();
        }
        emit loadFailed(arg);
    } else if (verb == "window") {
        m_engineWindow = arg.toULongLong();
        emit engineWindowChanged(m_engineWindow);
    } else if (verb == "imerect") {
        const QStringList f = arg.split(' ');
        if (f.size() == 4) {
            m_caretY = f.at(1).toInt();
            m_caretH = f.at(3).toInt();
            emit caretChanged();
        }
    } else if (verb == "auth") {
        // <retry> <host[:port]> <realm...> -- the realm is last because it is
        // free text from the server and may contain spaces.
        emit authenticationRequested(arg.section(' ', 1, 1),
                                     arg.section(' ', 2),
                                     arg.section(' ', 0, 0).toInt() != 0);
    } else if (verb == "dlstart") {
        // <total> <b64path> <uri>
        emit downloadStarted(arg.section(' ', 2),
                             fromB64(arg.section(' ', 1, 1)),
                             arg.section(' ', 0, 0).toLongLong());
    } else if (verb == "dlprogress") {
        emit downloadProgress(arg.section(' ', 2),
                              arg.section(' ', 0, 0).toLongLong(),
                              arg.section(' ', 1, 1).toLongLong());
    } else if (verb == "dlfinish") {
        emit downloadFinished(arg.section(' ', 1));
    } else if (verb == "dlfail") {
        emit downloadFailed(arg.section(' ', 1), fromB64(arg.section(' ', 0, 0)));
    } else if (verb == "image") {
        if (!arg.isEmpty())
            emit imageLongPressed(arg);
    } else if (verb == "link") {
        // The uri is the rest of the line. A url cannot contain a space, but it
        // is last anyway so an encoded one would survive.
        if (!arg.isEmpty())
            emit linkLongPressed(arg);
    } else if (verb == "ime") {
        emit inputFocusChanged(arg.toInt() != 0);
    } else if (verb == "scroll") {
        const int y = arg.toInt();
        if (y != m_scrollY) { m_scrollY = y; emit scrollChanged(); }
    } else if (verb == "surface") {
        const QStringList f = arg.split(' ');
        if (f.size() == 2)
            emit surfaceResized(QSize(f.at(0).toInt(), f.at(1).toInt()));
    } else if (verb == "frame") {
        if (!m_sawFrame) {
            m_sawFrame = true;
            qWarning("BrowserEngine: first frame from engine");
        }
        emit frameReady();
    } else if (verb == "ready") {
        // The engine only accepts commands once its view exists; anything sent
        // before this is dropped on the floor.
        emit engineReady();
        // Anything set before the process existed was dropped by send(); replay
        // it now that there is something to send to.
        if (!m_viewport.isEmpty())
            setViewportSize(m_viewport);
        if (m_zoom > 0)
            setZoom(m_zoom);
        // Initial state: send everything, and record it as applied so the
        // first commitSettings() does not resend the lot.
        pushSettings();
        send(QString("renderscale %1").arg(m_renderScale, 0, 'g', 3));
        send("useragent " + userAgentString());
        send(QString("layoutwidth %1").arg(m_layoutWidth));
        m_appliedJavascript = m_javascript;
        m_appliedImages = m_images;
        m_appliedBlocker = m_blocker;
        m_appliedUa = userAgentString();
        m_appliedRenderScale = m_renderScale;
        m_appliedLayoutWidth = m_layoutWidth;
    }
}

void BrowserEngine::onEngineStderr()
{
    const QByteArray err = m_engine->readAllStandardError();
    if (err.isEmpty())
        return;
    fprintf(stderr, "%s", err.constData());
    // Launched from the app grid there is nowhere for stderr to go, so a crash
    // leaves no trace at all. Keep the tail of it on disk instead.
    if (m_log.isOpen()) {
        m_log.write(err);
        m_log.flush();
        if (m_log.size() > 256 * 1024) {     // bounded; this is a phone
            m_log.resize(0);
            m_log.write("--- log truncated ---\n");
        }
    }
}

void BrowserEngine::onEngineError(QProcess::ProcessError err)
{
    qWarning() << "BrowserEngine: engine process error" << err;
}

void BrowserEngine::onEngineFinished(int code, QProcess::ExitStatus status)
{
    qWarning() << "BrowserEngine: engine exited, code" << code << "status" << status;
    if (m_loading) { m_loading = false; emit loadingChanged(); }
}

// name -> query template, %1 replaced by the percent-encoded terms.
static const char *kSearchEngines[][2] = {
    // The html/ endpoint, not duckduckgo.com: the main site renders nothing
    // without scripts, and JavaScript is off by default here.
    { "DuckDuckGo", "https://html.duckduckgo.com/html/?q=%1" },
    { "Google",     "https://www.google.com/search?q=%1" },
    { "Bing",       "https://www.bing.com/search?q=%1" },
    { "Wikipedia",  "https://en.m.wikipedia.org/w/index.php?search=%1" },
    { "Startpage",  "https://www.startpage.com/sp/search?query=%1" },
    { 0, 0 }
};

QStringList BrowserEngine::searchEngines() const
{
    QStringList names;
    for (int i = 0; kSearchEngines[i][0]; ++i)
        names << QString::fromLatin1(kSearchEngines[i][0]);
    return names;
}

void BrowserEngine::setSearchEngine(const QString &name)
{
    if (m_searchEngine == name || !searchEngines().contains(name))
        return;
    m_searchEngine = name;
    persist("search/engine", m_searchEngine);
    emit settingsChanged();
}

// A bare word is a search; something with a dot and no spaces is a host.
bool BrowserEngine::looksLikeUrl(const QString &text) const
{
    const QString s = text.trimmed();
    if (s.isEmpty())
        return false;
    if (s.contains("://") || s.startsWith("about:") || s.startsWith("file:"))
        return true;
    return s.contains('.') && !s.contains(' ');
}

QString BrowserEngine::searchUrlFor(const QString &terms) const
{
    // Fall back to the first entry rather than repeating its url here; a second
    // copy is a second thing to forget to change.
    QString tmpl = QString::fromLatin1(kSearchEngines[0][1]);
    for (int i = 0; kSearchEngines[i][0]; ++i) {
        if (m_searchEngine == QString::fromLatin1(kSearchEngines[i][0])) {
            tmpl = QString::fromLatin1(kSearchEngines[i][1]);
            break;
        }
    }
    return tmpl.arg(QString::fromLatin1(QUrl::toPercentEncoding(terms.trimmed())));
}

// Turn whatever the user typed into something loadable. Bare words become a
// search against the configured engine; anything with a dot and no scheme gets
// http://.
QString BrowserEngine::normalise(const QString &address) const
{
    QString s = address.trimmed();
    if (s.isEmpty())
        return QString("about:blank");
    if (s.contains("://") || s.startsWith("about:") || s.startsWith("file:"))
        return s;
    if (looksLikeUrl(s))
        return "http://" + s;
    return searchUrlFor(s);
}

void BrowserEngine::load(const QString &address)
{
    const QString target = normalise(address);
    m_url = target;
    emit urlChanged();
    send("load " + target);
}

void BrowserEngine::goBack()    { send("back"); }
void BrowserEngine::goForward() { send("forward"); }
void BrowserEngine::reload()    { send("reload"); }
void BrowserEngine::stop()      { send("stop"); }

void BrowserEngine::share()
{
    shareLink(m_url);
}

void BrowserEngine::shareLink(const QString &url)
{
    if (url.isEmpty() || url == "about:blank")
        return;
    // The platform share sheet takes a data URI, not a bare string: the mime
    // type is what lets it offer the right targets, and the title is what they
    // show. A bare url would share as plain text.
    MDataUri duri;
    duri.setMimeType("text/url");
    duri.setTextData(url);
    if (!m_title.isEmpty() && url == m_url)
        duri.setAttribute("title", m_title);
    duri.setAttribute("source", url);

    ShareUiInterface shareIf("com.nokia.ShareUi");
    if (!duri.isValid() || !shareIf.isValid()) {
        qWarning() << "BrowserEngine: share unavailable for" << url;
        return;
    }
    shareIf.share(QStringList() << duri.toString());
}

void BrowserEngine::saveLink(const QString &url)
{
    if (!url.isEmpty())
        send("dlsave " + url);
}

void BrowserEngine::requestContextMenu(int x, int y)
{
    send(QString("contextmenu %1 %2").arg(x).arg(y));
}

void BrowserEngine::cancelDownload(const QString &url)
{
    send("dlcancel " + url);
}

void BrowserEngine::copyToClipboard(const QString &text)
{
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

void BrowserEngine::setViewportSize(const QSize &size)
{
    m_viewport = size;
    if (size.isEmpty())
        return;
    // Position is always 0,0: the window is redirected offscreen and sampled as
    // a texture, so it never has to line up with anything on screen. The size
    // still resizes WebKit's viewport, which must stay in step with the window
    // or the bottom of the page goes unpainted.
    send(QString("geometry 0 0 %1 %2").arg(size.width()).arg(size.height()));
}

void BrowserEngine::sendTouch(const QString &kind, int slot, int x, int y)
{
    send(QString("touch %1 %2 %3 %4").arg(kind).arg(slot).arg(x).arg(y));
}

void BrowserEngine::setLayoutWidth(int px)
{
    if (px < 240 || px > 1024 || px == m_layoutWidth)
        return;
    m_layoutWidth = px;
    persist("layout/width", m_layoutWidth);
    emit settingsChanged();     // applied by commitSettings()
}

void BrowserEngine::setRenderScale(double scale)
{
    if (scale < 0.25 || scale > 4.0 || qFuzzyCompare(m_renderScale, scale))
        return;
    m_renderScale = scale;
    persist("render/scale", m_renderScale);
    emit settingsChanged();     // applied by commitSettings()
}

void BrowserEngine::scrollBy(int dx, int dy)
{
    send(QString("scrollby %1 %2").arg(dx).arg(dy));
}

void BrowserEngine::sendText(const QString &text)
{
    if (text.isEmpty())
        return;
    // A newline is not text. Return is a KEY, and a page is entitled to act on
    // it -- submit the form, send the message -- which it can only do if it
    // arrives as a key event rather than as a character to insert. The virtual
    // keyboard delivers Enter as a committed "\n" rather than as a key event,
    // so this is the path it actually takes.
    //
    // This used to replace newlines with spaces, which silently changed what
    // was typed. They still cannot go on the wire as-is: the protocol is one
    // message per line.
    QString flat = text;
    flat.replace("\r\n", "\n");
    flat.replace('\r', '\n');
    const QStringList lines = flat.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            sendKey(kKeysymReturn, true);
            sendKey(kKeysymReturn, false);
        }
        if (!lines.at(i).isEmpty())
            send("text " + lines.at(i));
    }
}

void BrowserEngine::authenticate(const QString &user, const QString &password)
{
    // Base64 both halves: the protocol splits on spaces and only the last
    // field may contain them, and a password is exactly the kind of string
    // that has a space, a newline or a non-ASCII character in it.
    send(QString("authok %1 %2")
         .arg(QString::fromLatin1(user.toUtf8().toBase64().constData()))
         .arg(QString::fromLatin1(password.toUtf8().toBase64().constData())));
}

void BrowserEngine::cancelAuthentication()
{
    send("authcancel");
}

void BrowserEngine::sendKey(int keysym, bool pressed)
{
    send(QString("key %1 %2").arg(keysym).arg(pressed ? 1 : 0));
}

void BrowserEngine::setSurfaceScale(double magnification)
{
    send(QString("scale %1").arg(magnification, 0, 'g', 4));
}

void BrowserEngine::setZoom(double level)
{
    m_zoom = level;
    // 'g' with 4 digits, and C locale: the engine parses this with
    // g_ascii_strtod, which does not know about a comma decimal separator.
    send(QString("zoom %1").arg(level, 0, 'g', 4));
}

void BrowserEngine::commitSettings()
{
    // Only what actually changed: each viewport change costs a relayout, and a
    // redundant one is indistinguishable from a wanted one to the engine.
    if (m_appliedJavascript != m_javascript || m_appliedImages != m_images
        || m_appliedBlocker != m_blocker) {
        m_appliedJavascript = m_javascript;
        m_appliedImages = m_images;
        m_appliedBlocker = m_blocker;
        pushSettings();
    }
    if (m_appliedUa != userAgentString()) {
        m_appliedUa = userAgentString();
        send("useragent " + m_appliedUa);
    }
    // Scale before width: both end in apply_surface(), and doing the cheaper
    // one first avoids laying out twice at an intermediate size.
    if (!qFuzzyCompare(m_appliedRenderScale, m_renderScale)) {
        m_appliedRenderScale = m_renderScale;
        send(QString("renderscale %1").arg(m_renderScale, 0, 'g', 3));
    }
    if (m_appliedLayoutWidth != m_layoutWidth) {
        m_appliedLayoutWidth = m_layoutWidth;
        send(QString("layoutwidth %1").arg(m_layoutWidth));
    }
}

void BrowserEngine::pushSettings()
{
    send(QString("set javascript %1").arg(m_javascript ? 1 : 0));
    send(QString("set images %1").arg(m_images ? 1 : 0));
    send(QString("set blocker %1").arg(m_blocker ? 1 : 0));
}

void BrowserEngine::setJavascriptEnabled(bool on)
{
    if (m_javascript == on) return;
    m_javascript = on;
    persist("content/javascript", m_javascript);
    emit settingsChanged();     // applied by commitSettings()
}

void BrowserEngine::setContentBlocker(bool on)
{
    if (m_blocker == on) return;
    m_blocker = on;
    persist("content/blocker", m_blocker);
    emit settingsChanged();     // applied by commitSettings()
}

void BrowserEngine::setSuggestionsEnabled(bool on)
{
    if (m_suggestions == on) return;
    m_suggestions = on;
    persist("suggestions/enabled", m_suggestions);
    emit settingsChanged();     // nothing to commit: the engine never sees this
}

void BrowserEngine::setImagesEnabled(bool on)
{
    if (m_images == on) return;
    m_images = on;
    persist("content/images", m_images);
    emit settingsChanged();     // applied by commitSettings()
}

// Safari strings, not Chrome ones: this engine IS WebKit, so a site that
// tailors to Safari serves markup we can actually render. Claiming Chrome would
// invite Blink-specific paths.
QString BrowserEngine::userAgentString() const
{
    if (m_uaPreset == "desktop")
        return "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
               "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 "
               "Safari/605.1.15";
    return "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) "
           "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 "
           "Mobile/15E148 Safari/604.1";
}

void BrowserEngine::setUserAgentPreset(const QString &preset)
{
    if (m_uaPreset == preset) return;
    m_uaPreset = preset;
    persist("ua/preset", m_uaPreset);
    emit settingsChanged();     // applied by commitSettings()
}

int BrowserEngine::cacheUsageMb() const
{
    // Recursive: WebKit nests its cache (WebKitCache/Version 16/<salt>/...), so
    // summing only the top level reported zero however full the cache was.
    qint64 total = 0;
    QDirIterator it(m_diskCachePath, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return int(total / (1024 * 1024));
}

void BrowserEngine::clearCache()
{
    // WebKit holds these files open; deleting them underneath it corrupts the
    // cache index rather than emptying it. Ask the owner to do it.
    send("clearcache");
    emit cacheChanged();
}

void BrowserEngine::clearCookies()
{
    send("clearcookies");
}

void BrowserEngine::clearAll()
{
    clearCache();
    clearCookies();
}

void BrowserEngine::onMemoryPressure(int level)
{
    // Order matters: drop what is cheapest to rebuild first.
    //
    // Every line here used to set a member and emit a signal, so Low did
    // nothing at all and Critical cleared the DISK cache -- which frees no RAM
    // and costs refetches over 3G. These three commands are the only runtime
    // cache controls WebKit's GLib API actually has, and each one now goes to
    // the engine.
    //
    // The engine has its own WebKitMemoryPressureSettings too, and that is the
    // better lever because it runs inside the web process; this is the shell's
    // own shedding on top of it.
    const bool wantPageCache = (level == 0);
    const bool wantMemoryCache = (level < 2);

    if (m_pageCacheOn != wantPageCache) {
        m_pageCacheOn = wantPageCache;
        send(QString("set pagecache %1").arg(m_pageCacheOn ? 1 : 0));
    }
    if (m_memoryCacheOn != wantMemoryCache) {
        m_memoryCacheOn = wantMemoryCache;
        send(QString("cachemodel %1").arg(m_memoryCacheOn ? "browser" : "viewer"));
    }
    // Emptying it is separate from disabling it: at Low the cache stays
    // enabled and is simply dropped, which is the cheap half.
    if (level > 0)
        send("clearmemcache");

    emit memoryPressure(level);
}
