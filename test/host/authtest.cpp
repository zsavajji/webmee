// Drives the real shell/qml/BrowserPage.qml auth flow with a stub engine and a
// stub credential store, and checks what gets answered, saved and forgotten.
//
// The case that matters most is the last one: the store answers asynchronously,
// so a reply can arrive for a challenge that has already been replaced, and
// answering the new one with the old one's password hands a site someone
// else's credentials.
#include <QApplication>
#include <QDeclarativeEngine>
#include <QDeclarativeComponent>
#include <QDeclarativeContext>
#include <QStringList>
#include <QTimer>
#include <QEventLoop>
#include <QDeclarativeExpression>
#include <QVariantMap>
#include <QDeclarativeExpression>
#include <cstdio>

class StubEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString url READ url NOTIFY urlChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool suggestionsEnabled READ suggestionsEnabled NOTIFY titleChanged)
public:
    bool suggestionsEnabled() const { return true; }
    bool m_loading;
    bool loading() const { return m_loading; }
    QStringList log;
    QString m_url, m_title;
    QString url() const { return m_url; }
    QString title() const { return m_title; }
    void setLoading(bool v) { m_loading = v; emit loadingChanged(); }
    void askAuth(const QString &w, const QString &r, bool retry)
        { emit authenticationRequested(w, r, retry); }
public slots:
    void load(const QString &a)              { log << "load " + a; }
    void switchTab(int id, const QString &u) { log << QString("tab %1 %2").arg(id).arg(u); }
    void closeTab(int id)                    { log << QString("tabclose %1").arg(id); }
    void authenticate(const QString &u, const QString &p)
        { log << QString("auth %1/%2").arg(u, p); }
    void cancelAuthentication()              { log << "authcancel"; }
    bool looksLikeUrl(const QString &t) const { return t.contains('.') || t.startsWith("http"); }
    QString searchUrlFor(const QString &t) const { return "http://search.example/?q=" + t; }
signals:
    void urlChanged();
    void titleChanged();
    void authenticationRequested(const QString &where, const QString &realm, bool retry);
    void webProcessCrashed();
    void loadingChanged();
};

// Stands in for PasswordStore. Lookups are deliberately NOT answered until the
// test says so, which is what lets the racing-challenge case be set up.
class StubPasswords : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(bool available READ available CONSTANT)
public:
    QStringList log;
    QMap<QString, QString> users, secrets;    // hostKey -> value
    bool m_available;
    StubPasswords() : m_available(true) {}
    int count() const { return users.size(); }
    bool available() const { return m_available; }
    void answer(const QString &key) {
        if (users.contains(key)) emit credentialsFound(key, users[key], secrets[key]);
        else emit credentialsNotFound(key);
    }
public slots:
    QString hostKey(const QString &h) const { return h.section(':', 0, 0).toLower(); }
    bool hasCredentials(const QString &h) const { return users.contains(hostKey(h)); }
    QString usernameFor(const QString &h) const { return users.value(hostKey(h)); }
    QStringList hosts() const { return users.keys(); }
    void requestCredentials(const QString &h) { log << "lookup " + hostKey(h); }
    void save(const QString &h, const QString &u, const QString &p) {
        log << QString("save %1 %2/%3").arg(hostKey(h), u, p);
        users[hostKey(h)] = u; secrets[hostKey(h)] = p; emit changed();
    }
    void forget(const QString &h) {
        if (!users.contains(hostKey(h))) return;
        log << "forget " + hostKey(h);
        users.remove(hostKey(h)); secrets.remove(hostKey(h)); emit changed();
    }
signals:
    void credentialsFound(const QString &host, const QString &username, const QString &password);
    void credentialsNotFound(const QString &host);
    void saved(const QString &host);
    void changed();
    void error(const QString &message);
};
// Stands in for HistoryStore, so the address-bar suggestion list has rows.
class StubHistory : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
public:
    int rows;
    StubHistory() : rows(2) {}
    int count() const { return rows; }
public slots:
    QVariantList suggest(const QString &text, int) {
        QVariantList out;
        if (text.isEmpty()) return out;
        for (int i = 0; i < rows; ++i) {
            QVariantMap m;
            m["url"] = QString("http://hit%1.example/").arg(i);
            m["title"] = QString("Hit %1").arg(i);
            out << m;
        }
        return out;
    }
signals:
    void changed();
};
#include "authtest.moc"

// The dialog's open and close are animated, so its status only reaches Open or
// Closed once the event loop has run. Nothing here is testing the animation;
// this just lets it finish before the status is read.
static void settle(int ms = 600)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, SLOT(quit()));
    loop.exec();
}

static int failures = 0;
static void is(const char *what, const QString &got, const QString &want)
{
    bool ok = (got == want);
    failures += !ok;
    printf("%s %-46s %s\n", ok ? "ok  " : "FAIL", what,
           ok ? qPrintable(got.isEmpty() ? QString("(nothing)") : got)
              : qPrintable(QString("got [%1] want [%2]").arg(got, want)));
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QDeclarativeEngine qml;
    for (int i = 2; i < argc; ++i) qml.addImportPath(argv[i]);

    StubEngine stub;
    stub.m_loading = false;
    StubPasswords pw;
    qml.rootContext()->setContextProperty("engine", &stub);
    qml.rootContext()->setContextProperty("passwords", &pw);
    StubHistory hist;
    qml.rootContext()->setContextProperty("history", &hist);

    QDeclarativeComponent c(&qml, QUrl::fromLocalFile(argv[1]));
    if (c.isError()) { printf("load error:\n%s\n", qPrintable(c.errorString())); return 2; }
    QObject *root = c.create();
    if (!root) { printf("create returned null\n"); return 2; }
    QObject *dlg = root->findChild<QObject *>("authDialog");
    if (!dlg) { printf("FAIL authDialog not found\n"); return 2; }

    const int Closed = 3;           // DialogStatus: Opening 0, Open 1, Closing 2, Closed 3
    #define OPEN (dlg->property("status").toInt() != Closed ? "open" : "shut")
    #define CLOSE_DIALOG do { QMetaObject::invokeMethod(dlg, "close"); settle(); } while (0)

    // --- nothing saved: the prompt opens straight away --------------------
    stub.log.clear(); pw.log.clear();
    stub.askAuth("files.example", "Staff", false);
    settle();
    is("unknown site prompts",            OPEN, "open");
    is("...without asking the store",     pw.log.join("|"), "");
    is("...and answers nothing yet",      stub.log.join("|"), "");
    CLOSE_DIALOG;

    // --- saved: answered without the prompt ever opening ------------------
    pw.users["files.example"] = "ada"; pw.secrets["files.example"] = "s3cret";
    stub.log.clear(); pw.log.clear();
    stub.askAuth("files.example", "Staff", false);
    settle();
    is("known site does not prompt yet",  OPEN, "shut");
    is("...it looks the password up",     pw.log.join("|"), "lookup files.example");
    pw.answer("files.example");
    settle();
    is("...and answers with it",          stub.log.join("|"), "auth ada/s3cret");
    is("...still without prompting",      OPEN, "shut");

    // --- a rejected password is dropped, not retried for ever -------------
    stub.log.clear(); pw.log.clear();
    stub.askAuth("files.example", "Staff", true);
    settle();
    is("a retry forgets the saved password", pw.log.join("|"), "forget files.example");
    is("...and prompts",                  OPEN, "open");
    QObject *userField = dlg->findChild<QObject *>("userField");
    is("...with the username already filled in",
       userField ? userField->property("text").toString() : "<no field>", "ada");
    CLOSE_DIALOG;

    // --- the store has no entry after all ---------------------------------
    pw.users["gone.example"] = "bob"; pw.secrets["gone.example"] = "x";
    stub.log.clear(); pw.log.clear();
    stub.askAuth("gone.example", "", false);
    pw.users.remove("gone.example");          // vanished between ask and answer
    pw.answer("gone.example");
    settle();
    is("a miss falls back to the prompt", OPEN, "open");
    CLOSE_DIALOG;

    // --- the racing challenge ---------------------------------------------
    // A lookup for one host is still in flight when the engine gives up on it
    // and challenges for another. The late reply must not answer the new one.
    pw.users["a.example"] = "ada";  pw.secrets["a.example"] = "apassword";
    pw.users["b.example"] = "bo";   pw.secrets["b.example"] = "bpassword";
    stub.log.clear(); pw.log.clear();
    stub.askAuth("a.example", "", false);          // lookup A starts
    stub.askAuth("b.example", "", false);          // ...replaced by B
    pw.answer("a.example");                        // A's reply lands late
    is("a stale reply answers nothing", stub.log.join("|"), "");
    pw.answer("b.example");
    is("the current one still works",   stub.log.join("|"), "auth bo/bpassword");

    // --- signing in by hand, with Remember ticked -------------------------
    pw.users.clear(); pw.secrets.clear();
    stub.log.clear(); pw.log.clear();
    stub.askAuth("new.example", "Area 51", false);
    settle();
    QObject *passField = dlg->findChild<QObject *>("passField");
    userField->setProperty("text", "carol");
    passField->setProperty("text", "pw with spaces");
    QMetaObject::invokeMethod(dlg, "accept");
    settle();
    is("signing in answers the challenge", stub.log.join("|"), "auth carol/pw with spaces");
    is("...and files the password",        pw.log.join("|"), "save new.example carol/pw with spaces");
    is("...and the prompt closes",         OPEN, "shut");
    is("...leaving no password in the field", passField->property("text").toString(), "");

    // The point of all of it: the next visit does not ask.
    stub.log.clear(); pw.log.clear();
    stub.askAuth("new.example", "Area 51", false);
    settle();
    pw.answer("new.example");
    settle();
    is("the next visit signs in by itself", stub.log.join("|"), "auth carol/pw with spaces");
    is("...with no prompt",                 OPEN, "shut");

    // --- a store that never answers ---------------------------------------
    stub.log.clear(); pw.log.clear();
    stub.askAuth("new.example", "Area 51", false);
    settle();
    is("a silent store does not prompt straight away", OPEN, "shut");
    settle(4600);                          // past the lookup timeout
    is("...but gives up and prompts",       OPEN, "open");
    is("...having answered nothing",        stub.log.join("|"), "");

    // --- signond unreachable (last: `available` is CONSTANT, so a
    //     binding that sees it false never sees it true again) ------ ----------------------------------------------
    pw.m_available = false;
    stub.log.clear(); pw.log.clear();
    stub.askAuth("files.example", "Staff", false);
    settle();
    is("no store means prompt, no lookup", pw.log.join("|"), "");
    is("...and the prompt is shown",       OPEN, "open");
    CLOSE_DIALOG;


    // --- the suggestion list must not outlive the navigation --------------
    // It is shown by a binding on urlField.activeFocus, so every path that
    // navigates has to actually drop that focus. A list left hanging over the
    // page it just loaded is the symptom.
    {
        QDeclarativeContext *c = QDeclarativeEngine::contextForObject(root);
        struct E {
            QDeclarativeContext *c; QObject *r;
            QString operator()(const QString &js) {
                QDeclarativeExpression e(c, r, js);
                QVariant v = e.evaluate();
                if (e.hasError()) return "ERR " + e.error().toString();
                return v.toString();
            }
        } ev = { c, root };

        ev("urlField.forceActiveFocus()");
        ev("urlField.text = 'hit'");
        settle(300);
        is("typing in the address bar opens the list", ev("suggestions.opacity > 0 ? 'open' : 'shut'"), "open");

        ev("suggestions.go('http://chosen.example/')");
        settle(300);
        is("choosing a suggestion drops the focus", ev("urlField.activeFocus ? 'focused' : 'blurred'"), "blurred");
        is("...and closes the list",                 ev("suggestions.opacity > 0 ? 'open' : 'shut'"), "shut");

        // Enter in the field, rather than tapping a row.
        ev("urlField.forceActiveFocus()");
        ev("urlField.text = 'hit'");
        settle(300);
        ev("suggestions.accept('hit')");
        settle(300);
        is("accepting by Enter closes it too", ev("suggestions.opacity > 0 ? 'open' : 'shut'"), "shut");

        // A navigation the address bar did not start -- a link tap -- while the
        // field still has focus from an abandoned edit.
        ev("urlField.forceActiveFocus()");
        ev("urlField.text = 'hit'");
        settle(300);
        is("list is open before the link tap", ev("suggestions.opacity > 0 ? 'open' : 'shut'"), "open");
        stub.setLoading(true);
        settle(300);
        is("a navigation from the page closes it", ev("suggestions.opacity > 0 ? 'open' : 'shut'"), "shut");
    }

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
