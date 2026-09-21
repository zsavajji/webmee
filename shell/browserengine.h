#ifndef BROWSERENGINE_H
#define BROWSERENGINE_H

// The QML-facing surface of the browser engine.
//
// This drives the real engine now. webmee-engine is spawned as a child process and
// speaks a line protocol over its stdin/stdout:
//     out: load/back/forward/reload/stop/geometry/quit
//     in:  url/title/progress/loading/nav/failed/ready
// Its stderr is left alone for diagnostics -- mixing them into stdout would
// corrupt the protocol.
//
// The engine renders into an X window parented to ours, so the shell keeps
// ownership of orientation, the status bar and the chrome. It is handed our
// window id and the view rectangle through the environment at spawn time.
//
// Deliberately plain C++03: this half is built with the original GCC 4.4 from
// the Harmattan SDK so it is ABI-compatible with the device's Qt 4.7.4. The
// engine half is GCC 14 / C++20 in its own process.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QFile>
#include <QSize>
#include <QProcess>

class BrowserEngine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double renderScale READ renderScale WRITE setRenderScale NOTIFY settingsChanged)
    Q_PROPERTY(int layoutWidth READ layoutWidth WRITE setLayoutWidth NOTIFY settingsChanged)
    Q_PROPERTY(int scrollY READ scrollY NOTIFY scrollChanged)
    // The caret, in CSS pixels, as WebKit reports it.
    Q_PROPERTY(int caretY READ caretY NOTIFY caretChanged)
    Q_PROPERTY(int caretH READ caretH NOTIFY caretChanged)
    Q_PROPERTY(QStringList searchEngines READ searchEngines CONSTANT)
    Q_PROPERTY(QString searchEngine READ searchEngine WRITE setSearchEngine NOTIFY settingsChanged)
    Q_PROPERTY(QString url READ url NOTIFY urlChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(qreal loadProgress READ loadProgress NOTIFY loadProgressChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY historyChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY historyChanged)
    Q_PROPERTY(bool javascriptEnabled READ javascriptEnabled WRITE setJavascriptEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool imagesEnabled READ imagesEnabled WRITE setImagesEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString userAgentPreset READ userAgentPreset WRITE setUserAgentPreset NOTIFY settingsChanged)
    // Purely a shell preference -- the engine is never told. It lives here
    // because this is where persisted settings live, and splitting that would
    // give the same setting two owners.
    Q_PROPERTY(bool suggestionsEnabled READ suggestionsEnabled WRITE setSuggestionsEnabled NOTIFY settingsChanged)

    // --- caching -----------------------------------------------------------
    // There are no cache SIZE knobs here, because WebKit's GLib API has none.
    // Three used to exist -- disk MB, memory MB, bfcache entries -- and each
    // set a member, emitted a signal, and reached the engine never. What the
    // API does expose at runtime is three switches, which the engine now takes
    // as `set pagecache`, `cachemodel` and `clearmemcache`, and which
    // onMemoryPressure() drives. Usage is still worth reporting: it is read off
    // the directory, so it is a fact rather than a setting.
    Q_PROPERTY(QString diskCachePath READ diskCachePath CONSTANT)
    Q_PROPERTY(int cacheUsageMb READ cacheUsageMb NOTIFY cacheChanged)

public:
    explicit BrowserEngine(QObject *parent = 0);

    QString url() const { return m_url; }
    QString title() const { return m_title; }
    qreal loadProgress() const { return m_progress; }
    bool loading() const { return m_loading; }
    bool canGoBack() const { return m_canGoBack; }
    bool canGoForward() const { return m_canGoForward; }

    bool javascriptEnabled() const { return m_javascript; }
    void setJavascriptEnabled(bool on);
    bool suggestionsEnabled() const { return m_suggestions; }
    void setSuggestionsEnabled(bool on);
    bool imagesEnabled() const { return m_images; }
    void setImagesEnabled(bool on);
    QString userAgentPreset() const { return m_uaPreset; }
    void setUserAgentPreset(const QString &preset);
    // The UA string a preset maps to. WebKit's default announces
    // "X11; Linux armv7l" with no Mobile token, so sites serve desktop layouts.
    QString userAgentString() const;

    // Which engine a bare search runs against. Named, persisted, and shown in
    // the address bar so a search is never a silent redirect.
    QStringList searchEngines() const;
    QString searchEngine() const { return m_searchEngine; }
    void setSearchEngine(const QString &name);
    // What the given text will actually do, for the address bar to label it.
    Q_INVOKABLE QString searchUrlFor(const QString &terms) const;
    Q_INVOKABLE bool looksLikeUrl(const QString &text) const;

    QString diskCachePath() const { return m_diskCachePath; }
    int cacheUsageMb() const;

public slots:
    void load(const QString &address);
    void goBack();
    void goForward();
    void reload();
    void stop();
    void share();
    // Share a specific url rather than the current page, for the long-press
    // menu. Goes through the platform share sheet (com.nokia.ShareUi).
    void shareLink(const QString &url);
    // Put text on the system clipboard. The shell does this, not the engine:
    // Qt already has the clipboard and the engine would need the backend's
    // pasteboard interface to reach the same place.
    Q_INVOKABLE void copyToClipboard(const QString &text);

    void clearCache();
    void clearCookies();
    void clearAll();

    // Driven by MemoryWatcher. 0 normal, 1 low, 2 critical. Sheds progressively
    // and, unlike the version this replaces, over the wire: Low drops the
    // bfcache and empties the memory cache, Critical additionally switches the
    // cache model to DOCUMENT_VIEWER, which is what disables the memory cache
    // outright. Normal restores both. Being OOM-killed loses the whole session,
    // so shedding early is the better trade.
    void onMemoryPressure(int level);

    // Called by WebViewItem whenever the QML page area resizes. Only the SIZE
    // matters now: the engine's window is composited as a texture, so where it
    // physically sits is irrelevant.
    void setViewportSize(const QSize &size);

    // One touchpoint, in page coordinates, straight through to the engine.
    void sendTouch(const QString &kind, int slot, int x, int y);
    void setZoom(double level);
    // Magnification for the rendered surface. Not page zoom: the engine raises
    // device size and scale factor together, so the layout does not change.
    void setSurfaceScale(double magnification);
    // Text typed on the on-screen keyboard, committed into the focused field.
    void sendText(const QString &text);

    // Answer the pending HTTP authentication challenge. Exactly one of these
    // must follow every authenticationRequested(): until one arrives the
    // engine is holding the request and the page is not loading.
    void authenticate(const QString &user, const QString &password);
    void cancelAuthentication();
    // Where downloads land. Set from main.cpp so the shell and the engine use
    // one constant rather than hardcoding the same path twice.
    void setDownloadDir(const QString &dir) { m_downloadDir = dir; }
    void cancelDownload(const QString &url);
    // Raise the page's context menu at a point, in surface device pixels.
    void requestContextMenu(int x, int y);
    // Download a url the user picked, rather than one a page navigated to.
    void saveLink(const QString &url);
    // An editing key, as an xkb keysym (Backspace 0xFF08, Return 0xFF0D...).
    void sendKey(int keysym, bool pressed);
    // Device pixels rasterised per CSS pixel. 1.0 is fast and soft, 1.5 matches
    // the panel and is sharp. The layout does not change either way.
    double renderScale() const { return m_renderScale; }
    void setRenderScale(double scale);
    // CSS pixels the page lays out to. Wider also means taller, because the
    // viewport is the panel divided by the scale this implies.
    int layoutWidth() const { return m_layoutWidth; }
    int scrollY() const { return m_scrollY; }
    int caretY() const { return m_caretY; }
    int caretH() const { return m_caretH; }
    Q_INVOKABLE void scrollBy(int dx, int dy);
    void setLayoutWidth(int px);

    // Settings are recorded and persisted immediately but NOT sent to the
    // engine until this is called -- changing the layout width or render scale
    // resizes the surface and relayouts the page, so applying each keystroke of
    // a settings session in turn wedges the browser. QML calls this when the
    // settings page is left.
    Q_INVOKABLE void commitSettings();

signals:
    void urlChanged();
    void titleChanged();
    void loadProgressChanged();
    void loadingChanged();
    void historyChanged();
    void settingsChanged();
    void cacheChanged();
    void engineReady();
    void loadFailed(const QString &message);
    void memoryPressure(int level);
    // The engine's X window, reported once its EGL target exists. WebViewItem
    // redirects it and samples it as a texture.
    void engineWindowChanged(qulonglong window);
    // A frame finished rendering into that window.
    void frameReady();
    // A real page settled; HistoryStore records it.
    void pageVisited(const QString &url, const QString &title);
    // The web process died. WebKit restarts it, but the page is gone.
    void webProcessCrashed();
    void scrollChanged();
    // The page focused or blurred an editable: show or hide the keyboard.
    void inputFocusChanged(bool focused);
    // A server asked for HTTP credentials. `where` is the host, with the port
    // appended only when it is not the scheme's own; `realm` is whatever the
    // server called the protected area, and may be empty. `retry` means the
    // last attempt was rejected.
    void authenticationRequested(const QString &where, const QString &realm, bool retry);
    // A link was long-pressed. The shell opens its own menu; WPE has no menu
    // UI of its own, so the engine only reports what was under the finger.
    void linkLongPressed(const QString &url);
    void imageLongPressed(const QString &url);
    // Downloads. The engine runs them -- Qt 4.7.4 cannot complete a TLS
    // handshake with a modern server, so the shell could not fetch https at
    // all. It picks the destination too, because WebKit needs one before any
    // data arrives. These map straight onto DownloadManager's slots.
    void downloadStarted(const QString &url, const QString &path, qint64 total);
    void downloadProgress(const QString &url, qint64 received, qint64 total);
    void downloadFinished(const QString &url);
    void downloadFailed(const QString &url, const QString &error);
    void caretChanged();
    // The engine is now rendering a surface of this size, in device pixels.
    void surfaceResized(const QSize &size);

public:
    // Explicit public: these follow a signals: section, and in Qt "signals" is
    // protected by default, so without this they inherit that access.
    // setParentWindow must be called before startEngine -- the window id is
    // passed to the engine at spawn time.
    void setParentWindow(qulonglong winId) { m_parentWindow = winId; }
    void startEngine();

private:
    void pushSettings();

private slots:
    void onEngineOutput();
    void onEngineError(QProcess::ProcessError err);
    void onEngineFinished(int code, QProcess::ExitStatus status);
    void onEngineStderr();

private:
    void send(const QString &command);
    void handleEvent(const QString &line);
    QString normalise(const QString &address) const;

    QString m_url;
    QString m_title;
    qreal m_progress;
    bool m_loading;

    bool m_canGoBack;
    bool m_canGoForward;
    int m_historyIndex;   // unused now; the engine owns history

    bool m_javascript;
    bool m_images;
    bool m_suggestions;
    QString m_uaPreset;
    QString m_searchEngine;

    QString m_diskCachePath;
    // What the engine was last told, so a return to Normal only undoes what
    // was actually done. Shedding twice is harmless; restoring something that
    // was never shed re-enables a cache the user had turned off.
    bool m_pageCacheOn;
    bool m_memoryCacheOn;

    QString m_dataPath;
    QString m_downloadDir;
    double m_zoom;
    double m_renderScale;
    int m_layoutWidth;
    int m_scrollY;
    int m_caretY;
    int m_caretH;

    // What the engine has actually been told, so commitSettings() sends only
    // what changed.
    int m_appliedLayoutWidth;
    double m_appliedRenderScale;
    QString m_appliedUa;
    bool m_appliedJavascript;
    bool m_appliedImages;
    QSize m_viewport;
    QProcess *m_engine;
    qulonglong m_engineWindow;
    bool m_sawFrame;
    qulonglong m_parentWindow;
    QFile m_log;
    QByteArray m_pending;         // partial line from the engine
};

#endif
