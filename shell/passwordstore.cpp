#include "passwordstore.h"

#include <SignOn/Identity>
#include <SignOn/identityinfo.h>   // no camel-case forwarder ships for this one
#include <SignOn/AuthSession>
#include <SignOn/SessionData>
#include <SignOn/Error>

#include <QSettings>
#include <QDir>
#include <QUrl>
#include <QStringList>
#include <QDebug>

PasswordStore::PasswordStore(QObject *parent)
    : QObject(parent), m_available(false)
{
    QString dir = QDir::homePath() + "/.config/WebMee";
    QDir().mkpath(dir);
    m_indexPath = dir + "/passwords.index";

    loadIndex();

    // Cheap reachability check: signond is on the session bus, and without
    // DBUS_SESSION_BUS_ADDRESS every call fails. Better to know now and hide the
    // feature than to fail silently at save time.
    m_available = !qgetenv("DBUS_SESSION_BUS_ADDRESS").isEmpty();
    if (!m_available)
        qWarning() << "PasswordStore: no session bus; signond unreachable "
                      "(launch through invoker, not the bare binary)";
}

PasswordStore::~PasswordStore()
{
    saveIndex();
}

// One key per site, not per page: credentials saved on /login should match the
// whole host. www. is stripped so example.com and www.example.com agree.
QString PasswordStore::hostKey(const QString &urlOrHost)
{
    QString s = urlOrHost.trimmed();
    if (s.isEmpty())
        return QString();

    QString host = s.contains("://") ? QUrl(s).host() : s;
    if (host.isEmpty())
        host = s;
    host = host.toLower();
    if (host.startsWith("www."))
        host = host.mid(4);
    int colon = host.indexOf(':');          // strip any :port
    if (colon > 0)
        host = host.left(colon);
    return host;
}

bool PasswordStore::hasCredentials(const QString &host) const
{
    return m_index.contains(hostKey(host));
}

QString PasswordStore::usernameFor(const QString &host) const
{
    QMap<QString, Entry>::const_iterator it = m_index.find(hostKey(host));
    return it == m_index.end() ? QString() : it.value().username;
}

QStringList PasswordStore::hosts() const
{
    return m_index.keys();
}

void PasswordStore::save(const QString &host, const QString &username, const QString &password)
{
    const QString key = hostKey(host);
    if (key.isEmpty() || username.isEmpty() || password.isEmpty())
        return;
    if (!m_available) {
        emit error(tr("Credential store unavailable"));
        return;
    }

    // Replace rather than accumulate: signond has no "update by realm", and two
    // identities for one site would make retrieval ambiguous.
    if (m_index.contains(key))
        forget(key);

    SignOn::IdentityInfo info;
    info.setCaption(QString("WebMee: %1").arg(key));
    info.setUserName(username);
    info.setSecret(password, true);
    info.setRealms(QStringList() << key);
    // The stock password plugin is both method and mechanism; it is what makes
    // the secret retrievable later.
    info.setMethod("password", QStringList() << "password");

    m_pendingSaveHost = key;
    m_pendingSaveUser = username;

    SignOn::Identity *identity = SignOn::Identity::newIdentity(info, this);
    connect(identity, SIGNAL(credentialsStored(quint32)), SLOT(onCredentialsStored(quint32)));
    connect(identity, SIGNAL(error(const SignOn::Error &)), SLOT(onSignonError(const SignOn::Error &)));
    identity->storeCredentials();
}

void PasswordStore::onCredentialsStored(quint32 id)
{
    if (m_pendingSaveHost.isEmpty())
        return;

    Entry e;
    e.id = id;
    e.username = m_pendingSaveUser;
    m_index.insert(m_pendingSaveHost, e);
    saveIndex();

    const QString host = m_pendingSaveHost;
    m_pendingSaveHost.clear();
    m_pendingSaveUser.clear();

    emit saved(host);
    emit changed();
}

void PasswordStore::requestCredentials(const QString &host)
{
    const QString key = hostKey(host);
    QMap<QString, Entry>::const_iterator it = m_index.find(key);
    if (it == m_index.end() || !m_available) {
        emit credentialsNotFound(key);
        return;
    }

    SignOn::Identity *identity = SignOn::Identity::existingIdentity(it.value().id, this);
    if (!identity) {
        emit credentialsNotFound(key);
        return;
    }

    // The sanctioned retrieval path -- see the note in the header about why
    // queryInfo() cannot work.
    SignOn::AuthSession *session = identity->createSession("password");
    if (!session) {
        emit credentialsNotFound(key);
        return;
    }

    m_sessionHosts.insert(session, key);
    connect(session, SIGNAL(response(const SignOn::SessionData &)),
            SLOT(onSessionResponse(const SignOn::SessionData &)));
    connect(session, SIGNAL(error(const SignOn::Error &)),
            SLOT(onSignonError(const SignOn::Error &)));

    SignOn::SessionData data;
    data.setUserName(it.value().username);
    session->process(data, "password");
}

void PasswordStore::onSessionResponse(const SignOn::SessionData &data)
{
    const QString host = m_sessionHosts.take(sender());
    const QString secret = data.Secret();

    if (secret.isEmpty()) {
        emit credentialsNotFound(host);
        return;
    }
    emit credentialsFound(host, data.UserName(), secret);
}

void PasswordStore::forget(const QString &host)
{
    const QString key = hostKey(host);
    QMap<QString, Entry>::iterator it = m_index.find(key);
    if (it == m_index.end())
        return;

    if (m_available) {
        SignOn::Identity *identity = SignOn::Identity::existingIdentity(it.value().id, this);
        if (identity)
            identity->remove();
    }

    m_index.erase(it);
    saveIndex();
    emit changed();
}

void PasswordStore::onSignonError(const SignOn::Error &err)
{
    // Drop any in-flight session bookkeeping for this sender, or the map leaks.
    if (m_sessionHosts.contains(sender())) {
        const QString host = m_sessionHosts.take(sender());
        emit credentialsNotFound(host);
    }
    m_pendingSaveHost.clear();
    m_pendingSaveUser.clear();

    qWarning() << "PasswordStore: signon error" << err.type() << err.message();
    emit error(err.message());
}

// The index holds identity ids and usernames only -- never a password.
void PasswordStore::loadIndex()
{
    QSettings s(m_indexPath, QSettings::IniFormat);
    int n = s.beginReadArray("logins");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        QString host = s.value("host").toString();
        if (host.isEmpty())
            continue;
        Entry e;
        e.id = s.value("id").toUInt();
        e.username = s.value("username").toString();
        if (e.id)
            m_index.insert(host, e);
    }
    s.endArray();
}

void PasswordStore::saveIndex() const
{
    QSettings s(m_indexPath, QSettings::IniFormat);
    // Without the remove(), shrinking the list leaves stale indices behind.
    s.remove("logins");
    s.beginWriteArray("logins", m_index.size());
    int i = 0;
    for (QMap<QString, Entry>::const_iterator it = m_index.begin(); it != m_index.end(); ++it, ++i) {
        s.setArrayIndex(i);
        s.setValue("host", it.key());
        s.setValue("id", it.value().id);
        s.setValue("username", it.value().username);
    }
    s.endArray();
    s.sync();
}

// ---- self test ------------------------------------------------------------

static const char *kTestHost = "https://www.selftest.example.com/login?a=1";
static const char *kTestUser = "n9-selftest";
static const char *kTestPass = "s3lf-t3st-secret";

void PasswordStore::runSelfTest()
{
    qDebug() << "PasswordStore selftest: available =" << m_available
             << " hostKey =" << hostKey(kTestHost);
    connect(this, SIGNAL(saved(QString)), SLOT(onSelfTestSaved(QString)));
    connect(this, SIGNAL(credentialsFound(QString,QString,QString)),
            SLOT(onSelfTestFound(QString,QString,QString)));
    connect(this, SIGNAL(credentialsNotFound(QString)), SLOT(onSelfTestMissing(QString)));
    save(kTestHost, kTestUser, kTestPass);
}

void PasswordStore::onSelfTestSaved(const QString &host)
{
    qDebug() << "PasswordStore selftest: saved for" << host << "-> requesting it back";
    requestCredentials(host);
}

void PasswordStore::onSelfTestFound(const QString &host, const QString &user, const QString &pass)
{
    const bool ok = (user == QLatin1String(kTestUser) && pass == QLatin1String(kTestPass));
    qDebug() << "PasswordStore selftest:" << (ok ? "PASS" : "FAIL")
             << " host =" << host << " user =" << user
             << " secret" << (ok ? "matches" : QString("MISMATCH: " + pass));
    forget(host);            // leave nothing behind in the user's store
}

void PasswordStore::onSelfTestMissing(const QString &host)
{
    qDebug() << "PasswordStore selftest: FAIL -- no credentials returned for" << host;
}
