#ifndef PASSWORDSTORE_H
#define PASSWORDSTORE_H

// Saved site passwords, kept in MeeGo's own credential store (signond).
//
// Division of responsibility:
//   signond  holds the SECRET, encrypted, in the same store the system accounts
//            use. We never write a password to our own files.
//   us       hold a small index of host -> { identity id, username }. Neither is
//            secret, and signond gives no way to search identities by realm from
//            an application, so the mapping has to live somewhere.
//
// Retrieval goes through an AuthSession on the stock "password" method, NOT
// through Identity::queryInfo(): IdentityInfo::secret() is private with only
// libsignon's own internals as friends, so an application can never read it
// directly. /usr/lib/signon/libpasswordplugin.so runs inside signond, where
// reading is permitted, and returns the secret in the session response.
//
// Everything here is asynchronous because signond is a D-Bus service. Note it
// lives on the SESSION bus: launched through invoker the app inherits
// DBUS_SESSION_BUS_ADDRESS, but a bare shell launch will fail with
// "Not connected to D-BUS server".

#include <QObject>
#include <QString>
#include <QMap>
#include <QHash>
#include <QStringList>   // complete type needed: moc generates a QStringList return

namespace SignOn {
class Identity;
class AuthSession;
class SessionData;
class Error;
}

class PasswordStore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    explicit PasswordStore(QObject *parent = 0);
    ~PasswordStore();

    int count() const { return m_index.size(); }
    // False when signond could not be reached at all, so the UI can hide the
    // feature instead of offering something that will silently fail.
    bool available() const { return m_available; }

public slots:
    // Store or replace the credentials for a host.
    void save(const QString &host, const QString &username, const QString &password);

    // Asynchronous: answers with credentialsFound() or credentialsNotFound().
    void requestCredentials(const QString &host);

    void forget(const QString &host);
    bool hasCredentials(const QString &host) const;
    QString usernameFor(const QString &host) const;
    QStringList hosts() const;

    // Normalises "https://www.example.com/login?x=1" down to "example.com", so
    // credentials saved on one page match the whole site.
    static QString hostKey(const QString &urlOrHost);

    // Round-trips a throwaway credential through signond and logs the result.
    // Runs at startup when N9_TEST_PASSWORDS is set; the async wiring and the
    // host index cannot be checked any other way without a browser session.
    void runSelfTest();

signals:
    void credentialsFound(const QString &host, const QString &username, const QString &password);
    void credentialsNotFound(const QString &host);
    void saved(const QString &host);
    void changed();
    void error(const QString &message);

private slots:
    void onCredentialsStored(quint32 id);
    void onSessionResponse(const SignOn::SessionData &data);
    void onSignonError(const SignOn::Error &err);
    void onSelfTestSaved(const QString &host);
    void onSelfTestFound(const QString &host, const QString &user, const QString &pass);
    void onSelfTestMissing(const QString &host);

private:
    struct Entry {
        quint32 id;
        QString username;
    };

    void loadIndex();
    void saveIndex() const;

    QMap<QString, Entry> m_index;          // host -> identity
    QString m_indexPath;
    bool m_available;

    // In-flight operations, so the async replies can be attributed to a host.
    QString m_pendingSaveHost;
    QString m_pendingSaveUser;
    QHash<QObject *, QString> m_sessionHosts;
};

#endif
