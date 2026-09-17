// WebMee shell entry point.
//
// Built with the SDK's original GCC 4.4 against Qt 4.7.4 so it is ABI-correct
// with the device's Qt; the rendering engine is a separate GCC-14 process.

#include <QtGui/QApplication>
#include <QtDeclarative>
#include <QDeclarativeView>
#include <QDeclarativeContext>
#include <QTextCodec>
#include <QDeclarativeComponent>
#include <QDeclarativeEngine>
#include <QFile>
#include <stdio.h>
#include <QGLWidget>
#include <QGraphicsView>

#include "browserengine.h"
#include "webviewitem.h"
#include "downloadmanager.h"
#include "memorywatcher.h"
#include "bookmarkstore.h"
#include "historystore.h"
#include "passwordstore.h"

#ifdef HARMATTAN_BOOSTER
#include <MDeclarativeCache>
#endif

Q_DECL_EXPORT int main(int argc, char **argv)   // Q_DECL_EXPORT: applauncherd dlopen's main
{
    // Qt 4 converts const char* as Latin-1 unless told otherwise; without this
    // every non-ASCII literal and tr() string is mangled.
    QTextCodec *codec = QTextCodec::codecForName("UTF-8");
    QTextCodec::setCodecForTr(codec);
    QTextCodec::setCodecForCStrings(codec);
    QTextCodec::setCodecForLocale(codec);

#ifdef HARMATTAN_BOOSTER
    QApplication *app = MDeclarativeCache::qApplication(argc, argv);
    QDeclarativeView *view = MDeclarativeCache::qDeclarativeView();
#else
    QApplication *app = new QApplication(argc, argv);
    QDeclarativeView *view = new QDeclarativeView;
#endif

    // The page is drawn as a GLES2 texture inside the scene, which needs a GL
    // viewport -- and with one, Qt cannot track dirty regions across the swap,
    // so the whole viewport must be repainted each frame.
    view->setViewport(new QGLWidget);
    view->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    view->setAttribute(Qt::WA_OpaquePaintEvent);
    view->setAttribute(Qt::WA_NoSystemBackground);
    view->viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    view->viewport()->setAttribute(Qt::WA_NoSystemBackground);
    // QGraphicsView only routes touch to items when its VIEWPORT accepts touch;
    // without this the page sees synthesised single-point mouse events and
    // pinch is invisible.
    view->viewport()->setAttribute(Qt::WA_AcceptTouchEvents);

    qmlRegisterType<WebViewItem>("com.webmee", 1, 0, "WebView");

    // Declared before the view so they outlive it: context properties are raw
    // pointers the QML engine holds, and destruction here is reverse order.
    BrowserEngine engine;
    DownloadManager downloads;
    MemoryWatcher memory;
    BookmarkStore bookmarks;
    HistoryStore history;
    PasswordStore passwords;

    // Context properties are raw pointers held by the QML engine, so `engine`
    // must outlive the view. It is declared first here, which means it is
    // destroyed last.
    view->rootContext()->setContextProperty("engine", &engine);
    view->rootContext()->setContextProperty("downloads", &downloads);
    view->rootContext()->setContextProperty("memory", &memory);
    view->rootContext()->setContextProperty("bookmarks", &bookmarks);
    view->rootContext()->setContextProperty("history", &history);
    view->rootContext()->setContextProperty("passwords", &passwords);

    // Memory pressure drives cache shedding. Thresholds are in MB of estimated
    // available memory; see MemoryWatcher for why /proc/meminfo is the source.
    QObject::connect(&engine, SIGNAL(pageVisited(QString,QString)),
                     &history, SLOT(visit(QString,QString)));

    QObject::connect(&memory, SIGNAL(pressure(int)), &engine, SLOT(onMemoryPressure(int)));

    // Downloads run in the engine and are drawn by the shell. One constant for
    // where they land, set here rather than hardcoded on both sides.
    engine.setDownloadDir(downloads.downloadDir());
    QObject::connect(&engine, SIGNAL(downloadStarted(QString,QString,qint64)),
                     &downloads, SLOT(start(QString,QString,qint64)));
    QObject::connect(&engine, SIGNAL(downloadProgress(QString,qint64,qint64)),
                     &downloads, SLOT(updateProgress(QString,qint64,qint64)));
    QObject::connect(&engine, SIGNAL(downloadFinished(QString)),
                     &downloads, SLOT(finish(QString)));
    QObject::connect(&engine, SIGNAL(downloadFailed(QString,QString)),
                     &downloads, SLOT(fail(QString,QString)));
    QObject::connect(&downloads, SIGNAL(cancelRequested(QString)),
                     &engine, SLOT(cancelDownload(QString)));
    memory.setThresholds(140, 70);
    memory.start();

    if (!qgetenv("N9_TEST_PASSWORDS").isEmpty())
        passwords.runSelfTest();

    // N9_PROBE_QML=<file> answers "does this type exist on THIS device?" for a
    // list of candidate names, one per line as "import.path TypeName". QML
    // types resolve at creation time, so this is the only reliable enumeration:
    // qmldir lists the .qml components but not what the plugin registers.
    if (!qgetenv("N9_PROBE_QML").isEmpty()) {
        QFile list(QString::fromLocal8Bit(qgetenv("N9_PROBE_QML")));
        if (list.open(QIODevice::ReadOnly)) {
            while (!list.atEnd()) {
                const QString line = QString::fromUtf8(list.readLine()).trimmed();
                if (line.isEmpty() || line.startsWith('#'))
                    continue;
                const int sp = line.lastIndexOf(' ');
                if (sp <= 0)
                    continue;
                const QString imp = line.left(sp);
                const QString type = line.mid(sp + 1);
                QDeclarativeComponent c(view->engine());
                c.setData(QString("import QtQuick 1.1\nimport %1\n%2 {}\n")
                          .arg(imp).arg(type).toUtf8(), QUrl());
                printf("%s %s %s\n", qPrintable(imp), qPrintable(type),
                       c.isError() ? "MISSING" : "ok");
            }
            fflush(stdout);
        }
        return 0;
    }

    // N9_TEST_QML instantiates every page and reports errors instead of waiting
    // for the user to tap one. Missing QML types resolve at creation time, not
    // load time -- SelectionListItem does not exist on Harmattan, and the
    // Settings page simply failed to open with nothing in the log.
    if (!qgetenv("N9_TEST_QML").isEmpty()) {
        const char *pages[] = { "BrowserPage", "SettingsPage", "BookmarksPage",
                                "DownloadsPage", "TabsSheet", "main", 0 };
        int bad = 0;
        for (int i = 0; pages[i]; ++i) {
            QDeclarativeComponent c(view->engine(),
                QUrl(QString("qrc:/qml/%1.qml").arg(pages[i])));
            if (c.isError()) {
                ++bad;
                qWarning("QML %s: %s", pages[i],
                         qPrintable(c.errorString().trimmed()));
            } else {
                qWarning("QML %s: ok", pages[i]);
            }
        }
        qWarning("QML self-test: %d page(s) with errors", bad);
        return bad ? 1 : 0;
    }

    view->setSource(QUrl("qrc:/qml/main.qml"));

    WebViewItem *page = view->rootObject()
        ? view->rootObject()->findChild<WebViewItem *>("webViewArea") : 0;
    if (page) {
        QObject::connect(page, SIGNAL(surfaceSizeChanged(QSize)),
                         &engine, SLOT(setViewportSize(QSize)));
        QObject::connect(&engine, SIGNAL(engineWindowChanged(qulonglong)),
                         page, SLOT(setEngineWindow(qulonglong)));
        QObject::connect(&engine, SIGNAL(frameReady()), page, SLOT(frameReady()));
        QObject::connect(&engine, SIGNAL(webProcessCrashed()), page, SLOT(pageCrashed()));
        QObject::connect(page, SIGNAL(touched(QString,int,int,int)),
                         &engine, SLOT(sendTouch(QString,int,int,int)));
        QObject::connect(page, SIGNAL(scaleRequested(double)),
                         &engine, SLOT(setSurfaceScale(double)));
        QObject::connect(&engine, SIGNAL(surfaceResized(QSize)),
                         page, SLOT(setSurfaceSize(QSize)));
    } else {
        qWarning("no WebView item named webViewArea; the page will not be shown");
    }

    view->showFullScreen();

    // The item's first layout happened inside setSource(), before the signal
    // above was connected, so its size has to be collected by hand once. Until
    // the engine starts this only records it -- it is then handed over as
    // N9_VIEW_RECT at spawn.
    if (page && page->width() > 0 && page->height() > 0)
        engine.setViewportSize(QSize(int(page->width()), int(page->height())));

    // The engine's window is still a CHILD of ours -- not so it can be seen
    // there (XComposite redirects it offscreen and we sample it as a texture)
    // but so it stays inside our window tree and dies with us.
    engine.setParentWindow((qulonglong) view->winId());
    engine.startEngine();

    int rc = app->exec();
    delete view;
    delete app;
    return rc;
}
