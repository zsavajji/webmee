#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

// Downloads, exposed to QML as a list model.
//
// Files land in /home/user/MyDocs/Downloads -- the platform convention, and the
// right choice for two reasons: MyDocs is its own 8.8 GB partition (6.8 GB free,
// versus 1.6 GB on /home), and it is the volume exported over USB/MTP, so a
// downloaded file is visible from a PC and to the stock file manager.
//
// The stub simulates transfers on a timer. When the engine lands, each entry is
// backed by a WebKitDownload and the same signals drive the same UI.

#include <QAbstractListModel>
#include <QList>
#include <QString>

class DownloadManager : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged)
    Q_PROPERTY(QString downloadDir READ downloadDir CONSTANT)

public:
    enum State { Queued, Running, Finished, Failed, Cancelled };

    enum Roles {
        NameRole = Qt::UserRole + 1,
        UrlRole,
        PathRole,
        ReceivedRole,
        TotalRole,
        ProgressRole,
        StateRole,
        StatusTextRole
    };

    explicit DownloadManager(QObject *parent = 0);

    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role) const;

    int count() const { return m_items.size(); }
    int activeCount() const;
    QString downloadDir() const { return m_dir; }

public slots:
    // Called by the engine when it decides a response is a download.
    // `path` is the destination the ENGINE chose and is already writing to --
    // it picks the name because it is the side that has the server's suggestion
    // and has to hand WebKit a destination before any data arrives.
    void start(const QString &url, const QString &path, qint64 totalBytes);
    void cancel(int row);
    void removeEntry(int row);
    void openFile(int row);
    void clearFinished();

    // Engine-driven progress, once downloads are real.
    void updateProgress(const QString &url, qint64 received, qint64 total);
    void finish(const QString &url);
    void fail(const QString &url, const QString &error);

signals:
    void countChanged();
    void activeCountChanged();
    void downloadStarted(const QString &name);
    void downloadFinished(const QString &name, const QString &path);
    void downloadFailed(const QString &name, const QString &error);
    // Cancelling is the engine's to do; the row only asks.
    void cancelRequested(const QString &url);

private slots:

private:
    struct Item {
        QString name;
        QString url;
        QString path;
        qint64 received;
        qint64 total;
        int state;
        QString error;
    };

    int indexOfUrl(const QString &url) const;
    void touched(int row);
    static QString humanSize(qint64 bytes);

    QList<Item> m_items;
    QString m_dir;
};

#endif
