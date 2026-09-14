/*
 * Does MeeGo's credential store hand a password back?
 *
 * signond is the obvious place for browser passwords -- it is the encrypted
 * store the system's own accounts use, and IdentityInfo::setRealms() maps
 * exactly onto "these credentials belong to this site".
 *
 * The open question is retrieval. In many signond versions the secret is
 * deliberately NOT returned by queryInfo(): the password is handed to an
 * authentication plugin inside an AuthSession and never travels back to the
 * calling application. IdentityInfo::secret() exists as a getter either way, so
 * the header cannot answer this -- only the daemon can.
 *
 * ANSWERED, at compile time: IdentityInfo::secret() is PRIVATE, with only
 * AuthServiceImpl and IdentityImpl as friends. An application can never read it
 * back directly.
 *
 * So this probe now tests the sanctioned route instead: signond ships a stock
 * "password" plugin (/usr/lib/signon/libpasswordplugin.so) whose job is exactly
 * to return stored credentials through an AuthSession. If that works, a browser
 * needs no plugin of its own -- store with method "password", then run a session
 * and read Secret out of the response.
 */

#include <QCoreApplication>
#include <QTimer>
#include <QDebug>
#include <QStringList>

#include <SignOn/Identity>
#include <SignOn/identityinfo.h>   // no camel-case forwarder ships for this one
#include <SignOn/Error>
#include <SignOn/AuthSession>
#include <SignOn/SessionData>

static const char *kUser   = "n9probe-user";
static const char *kSecret = "correct-horse-battery-staple";
static const char *kRealm  = "example.com";

class Probe : public QObject
{
    Q_OBJECT
public:
    Probe() : m_writer(0), m_reader(0), m_id(0) {}

public slots:
    // Must be a slot: QTimer::singleShot dispatches through the meta-object.
    void run()
    {
        SignOn::IdentityInfo info;
        info.setCaption("n9browser probe");
        info.setUserName(kUser);
        info.setSecret(kSecret, true);          // true = ask signond to store it
        info.setRealms(QStringList() << kRealm);
        // The stock password plugin is both the method and the mechanism.
        info.setMethod("password", QStringList() << "password");

        m_writer = SignOn::Identity::newIdentity(info, this);
        connect(m_writer, SIGNAL(credentialsStored(quint32)), SLOT(onStored(quint32)));
        connect(m_writer, SIGNAL(error(const SignOn::Error &)), SLOT(onError(const SignOn::Error &)));
        m_writer->storeCredentials();
    }

private slots:
    void onStored(quint32 id)
    {
        m_id = id;
        qDebug() << "[store] OK, identity id =" << id;

        // Retrieve through an AuthSession on the stock "password" method: that
        // plugin runs inside signond, where reading the secret IS allowed, and
        // returns it in the session response.
        m_reader = SignOn::Identity::existingIdentity(id, this);
        if (!m_reader) {
            qDebug() << "[read ] existingIdentity() returned null";
            finish(2);
            return;
        }
        SignOn::AuthSession *session = m_reader->createSession("password");
        if (!session) {
            qDebug() << "[read ] createSession(\"password\") returned null";
            finish(2);
            return;
        }
        connect(session, SIGNAL(response(const SignOn::SessionData &)),
                SLOT(onResponse(const SignOn::SessionData &)));
        connect(session, SIGNAL(error(const SignOn::Error &)),
                SLOT(onError(const SignOn::Error &)));

        SignOn::SessionData data;
        data.setUserName(kUser);
        session->process(data, "password");
    }

    void onResponse(const SignOn::SessionData &data)
    {
        qDebug() << "[read ] userName:" << data.UserName();
        const QString got = data.Secret();
        if (got.isEmpty()) {
            qDebug() << "[read ] secret  : EMPTY -- the password plugin did not return it";
            qDebug() << "VERDICT: a custom signond plugin is needed after all.";
            finish(1);
        } else if (got == QLatin1String(kSecret)) {
            qDebug() << "[read ] secret  : returned intact";
            qDebug() << "VERDICT: the STOCK password plugin returns it -- no custom plugin needed.";
            finish(0);
        } else {
            qDebug() << "[read ] secret  : MISMATCH ->" << got;
            finish(3);
        }
    }

    void onError(const SignOn::Error &err)
    {
        qDebug() << "[error]" << err.type() << err.message();
        finish(4);
    }

    void finish(int code)
    {
        // Leave nothing behind in the user's credential store.
        if (m_id && m_reader)
            m_reader->remove();
        QTimer::singleShot(300, qApp, SLOT(quit()));
        qApp->setProperty("exitCode", code);
    }

private:
    SignOn::Identity *m_writer;
    SignOn::Identity *m_reader;
    quint32 m_id;
};

#include "signonprobe.moc"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Probe p;
    QTimer::singleShot(0, &p, SLOT(run()));
    // Do not hang forever if signond never answers.
    QTimer::singleShot(15000, &app, SLOT(quit()));
    app.exec();
    int code = app.property("exitCode").isValid() ? app.property("exitCode").toInt() : 5;
    if (code == 5) qDebug() << "VERDICT: timed out -- signond did not respond.";
    return code;
}
