// Drives the real shell/qml/main.qml on the host with a stub engine and checks
// the tab bookkeeping: which row is current, what the engine is told to switch
// to and close, and what the rows say after each step.
//
// Everything goes through QDeclarativeExpression evaluated in the root's own
// context, so the file's ids (tabModel) and functions are reachable exactly as
// the QML sees them -- no test hooks in the production file.
#include <QApplication>
#include <QDeclarativeEngine>
#include <QDeclarativeComponent>
#include <QDeclarativeContext>
#include <QDeclarativeExpression>
#include <QStringList>
#include <cstdio>

// Stands in for BrowserEngine: records the commands, and can play back the
// url/title the real engine reports once a page settles.
class StubEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString url READ url NOTIFY urlChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(int scrollY READ scrollY NOTIFY scrollChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
public:
    QStringList log;
    QString m_url, m_title;
    int m_scrollY; bool m_loading;
    StubEngine() : m_scrollY(0), m_loading(false) {}
    QString url() const { return m_url; }
    QString title() const { return m_title; }
    int scrollY() const { return m_scrollY; }
    bool loading() const { return m_loading; }
    void setScroll(int y) { m_scrollY = y; emit scrollChanged(); }
    // The real engine zeroes scrollY at load commit, so a restore is always
    // relative to the top; the stub has to do the same or the test would pass
    // on an implementation that only works from a scrolled page.
    void finishLoad() {
        m_loading = true;  emit loadingChanged();
        m_scrollY = 0;     emit scrollChanged();
        m_loading = false; emit loadingChanged();
    }
    void arrive(const QString &u, const QString &t) {
        m_url = u; emit urlChanged();
        m_title = t; emit titleChanged();
    }
public slots:
    void load(const QString &a)              { log << "load " + a; }
    void scrollBy(int dx, int dy)            { log << QString("scrollby %1 %2").arg(dx).arg(dy); }
        // The page also listens for these; declaring them keeps the real
    // Connections blocks bindable, which is itself worth checking.
    void askAuth(const QString &w, const QString &r, bool retry)
        { emit authenticationRequested(w, r, retry); }
signals:
    void urlChanged();
    void titleChanged();
    void scrollChanged();
    void loadingChanged();
    void authenticationRequested(const QString &where, const QString &realm, bool retry);
    void webProcessCrashed();
};
#include "tabtest.moc"

static QObject *g_root = 0;
static int failures = 0;

static QString eval(const QString &js)
{
    QDeclarativeExpression e(QDeclarativeEngine::contextForObject(g_root), g_root, js);
    QVariant v = e.evaluate();
    if (e.hasError()) { printf("   !! %s: %s\n", qPrintable(js),
                               qPrintable(e.error().toString())); ++failures; }
    return v.toString();
}
static void run(const QString &js) { eval(js); }

static void is(const char *what, const QString &got, const QString &want)
{
    bool ok = (got == want);
    failures += !ok;
    printf("%s %-38s %s\n", ok ? "ok  " : "FAIL", what,
           ok ? qPrintable(got.isEmpty() ? QString("(empty)") : got)
              : qPrintable(QString("got [%1] want [%2]").arg(got, want)));
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QDeclarativeEngine qml;
    for (int i = 2; i < argc; ++i) qml.addImportPath(argv[i]);

    StubEngine stub;
    qml.rootContext()->setContextProperty("engine", &stub);

    QDeclarativeComponent c(&qml, QUrl::fromLocalFile(argv[1]));
    if (c.isError()) { printf("load error:\n%s\n", qPrintable(c.errorString())); return 2; }
    g_root = c.create();
    if (!g_root) { printf("create returned null\n"); return 2; }

    // --- the reported bug: tab 0's url was never written back, so coming
    //     back to it loaded about:blank -----------------------------------
    stub.arrive("http://news.example/story", "A story");
    is("tab 0 url follows the page",  eval("tabModel.get(0).tabUrl"), "http://news.example/story");
    is("tab 0 title follows the page", eval("tabModel.get(0).tabTitle"), "A story");

    // --- open in a new tab ----------------------------------------------
    stub.log.clear();
    run("openTab('about:blank')");
    is("new tab is current", eval("currentTab"), "1");
    is("two tabs",           eval("tabModel.count"), "2");
    is("engine told to load", stub.log.join("|"), "load about:blank");
    is("new tab is labelled",   eval("tabModel.get(1).tabTitle"), "New tab");
    stub.arrive("http://maps.example/", "Maps");
    is("old tab kept its url",     eval("tabModel.get(0).tabUrl"), "http://news.example/story");
    is("new tab took the new one", eval("tabModel.get(1).tabUrl"), "http://maps.example/");

    // --- switching back --------------------------------------------------
    stub.log.clear();
    run("showTab(0)");
    is("switching loads that tab url", stub.log.join("|"), "load http://news.example/story");
    is("current is 0", eval("currentTab"), "0");
    stub.log.clear();
    run("showTab(0)");
    is("switching to the current tab does nothing", stub.log.join("|"), "");

    // --- closing a tab that is NOT current -------------------------------
    run("openTab('http://third.example/')");     // index 2, id 2
    stub.arrive("http://third.example/", "Third");
    run("showTab(0)");
    stub.log.clear();
    run("closeTab(1)");                          // the maps tab, id 1
    is("closing a later tab sends nothing", stub.log.join("|"), "");
    is("still on tab 0", eval("currentTab"), "0");
    is("two left",       eval("tabModel.count"), "2");
    is("row 1 is now the third tab", eval("tabModel.get(1).tabUrl"), "http://third.example/");

    // --- closing a tab BEFORE the current one: the index must shift ------
    run("showTab(1)");                           // on the third tab (id 2)
    stub.log.clear();
    run("closeTab(0)");                          // close the news tab under it
    is("current index shifted down", eval("currentTab"), "0");
    is("and it is the same tab",     eval("tabModel.get(0).tabUrl"), "http://third.example/");
    is("closing under the current tab sends nothing", stub.log.join("|"), "");

    // --- closing the CURRENT tab must move the view somewhere ------------
    run("openTab('http://fourth.example/')");    // index 1, id 3, current
    stub.arrive("http://fourth.example/", "Fourth");
    stub.log.clear();
    run("closeTab(1)");
    is("closing the current tab loads the next", stub.log.join("|"),
       "load http://third.example/");
    is("current fell back to row 0", eval("currentTab"), "0");

    // --- the last tab cannot be closed -----------------------------------
    stub.log.clear();
    run("closeTab(0)");
    is("last tab is kept",  eval("tabModel.count"), "1");
    is("and nothing was sent", stub.log.join("|"), "");

    // --- scroll position survives a tab switch ---------------------------
    // One tab is left here, showing third.example.
    stub.setScroll(900);
    run("openTab('http://b.example/')");
    is("switching away banks the position", eval("tabModel.get(0).tabScroll"), "900");
    is("a new tab starts at the top",       eval("tabModel.get(1).tabScroll"), "0");

    stub.setScroll(0);
    stub.log.clear();
    run("showTab(0)");
    is("nothing is scrolled before the load finishes", stub.log.join("|"),
       "load http://third.example/");
    stub.finishLoad();
    is("position restored when the load finishes", stub.log.join("|"),
       "load http://third.example/|scrollby 0 900");

    stub.log.clear();
    stub.finishLoad();
    is("and only once", stub.log.join("|"), "");

    // Switching away mid-load must not drop tab 0's position into tab 1.
    stub.setScroll(0);
    run("showTab(1)");
    stub.log.clear();
    run("showTab(0)");          // pending 900 for row 0 ...
    run("showTab(1)");          // ... but we are on row 1 when it lands
    stub.finishLoad();
    is("a pending restore does not follow the switch", stub.log.join("|"),
       "load http://third.example/|load http://b.example/");

    // The auth prompt moved to authtest.cpp once it grew a credential
    // store to stub; duplicating it here only duplicated the stubs.

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
