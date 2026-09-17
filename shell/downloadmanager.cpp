#include "downloadmanager.h"
#include <QDir>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>
#include <QFile>
#include <QDebug>

DownloadManager::DownloadManager(QObject *parent)
    : QAbstractListModel(parent)
{
    m_dir = QDir::homePath() + "/MyDocs/Downloads";
    QDir().mkpath(m_dir);

    // QML1 needs role names declared up front.
    QHash<int, QByteArray> roles;
    roles[NameRole]       = "name";
    roles[UrlRole]        = "url";
    roles[PathRole]       = "path";
    roles[ReceivedRole]   = "received";
    roles[TotalRole]      = "total";
    roles[ProgressRole]   = "progress";
    roles[StateRole]      = "state";
    roles[StatusTextRole] = "statusText";
    setRoleNames(roles);

}

int DownloadManager::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QString DownloadManager::humanSize(qint64 bytes)
{
    if (bytes < 0) return QString("?");
    if (bytes < 1024) return QString::number(bytes) + " B";
    if (bytes < 1024 * 1024) return QString::number(bytes / 1024.0, 'f', 1) + " kB";
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
}

QVariant DownloadManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return QVariant();
    const Item &it = m_items.at(index.row());

    switch (role) {
    case NameRole:     return it.name;
    case UrlRole:      return it.url;
    case PathRole:     return it.path;
    case ReceivedRole: return it.received;
    case TotalRole:    return it.total;
    case ProgressRole: return it.total > 0 ? qreal(it.received) / qreal(it.total) : qreal(0);
    case StateRole:    return it.state;
    case StatusTextRole:
        switch (it.state) {
        case Finished:  return humanSize(it.total);
        case Failed:    return it.error.isEmpty() ? tr("Failed") : it.error;
        case Cancelled: return tr("Cancelled");
        case Queued:    return tr("Waiting");
        default:
            return it.total > 0
                ? humanSize(it.received) + tr(" of ") + humanSize(it.total)
                : humanSize(it.received);
        }
    }
    return QVariant();
}

int DownloadManager::activeCount() const
{
    int n = 0;
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).state == Running || m_items.at(i).state == Queued)
            ++n;
    return n;
}

int DownloadManager::indexOfUrl(const QString &url) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).url == url && m_items.at(i).state == Running)
            return i;
    return -1;
}

void DownloadManager::touched(int row)
{
    QModelIndex ix = index(row, 0);
    emit dataChanged(ix, ix);
}


void DownloadManager::start(const QString &url, const QString &path, qint64 totalBytes)
{
    Item it;
    it.url = url;
    it.path = path;
    it.name = QFileInfo(it.path).fileName();
    it.received = 0;
    it.total = totalBytes;
    it.state = Running;

    beginInsertRows(QModelIndex(), m_items.size(), m_items.size());
    m_items.append(it);
    endInsertRows();

    emit countChanged();
    emit activeCountChanged();
    emit downloadStarted(it.name);
}

void DownloadManager::updateProgress(const QString &url, qint64 received, qint64 total)
{
    int row = indexOfUrl(url);
    if (row < 0) return;
    m_items[row].received = received;
    if (total > 0) m_items[row].total = total;
    touched(row);
}

void DownloadManager::finish(const QString &url)
{
    int row = indexOfUrl(url);
    if (row < 0) return;
    m_items[row].state = Finished;
    m_items[row].received = m_items[row].total;
    touched(row);
    emit activeCountChanged();
    emit downloadFinished(m_items[row].name, m_items[row].path);
}

void DownloadManager::fail(const QString &url, const QString &error)
{
    int row = indexOfUrl(url);
    if (row < 0) return;
    m_items[row].state = Failed;
    m_items[row].error = error;
    touched(row);
    emit activeCountChanged();
    emit downloadFailed(m_items[row].name, error);
}

void DownloadManager::cancel(int row)
{
    if (row < 0 || row >= m_items.size()) return;
    if (m_items[row].state != Running && m_items[row].state != Queued) return;
    m_items[row].state = Cancelled;
    // Stop the transfer first: deleting a file the engine is still writing to
    // only gets it recreated.
    emit cancelRequested(m_items[row].url);
    // A partial file is worse than none: the user cannot tell it is truncated.
    QFile::remove(m_items[row].path);
    touched(row);
    emit activeCountChanged();
}

void DownloadManager::removeEntry(int row)
{
    if (row < 0 || row >= m_items.size()) return;
    if (m_items[row].state == Running)
        cancel(row);
    beginRemoveRows(QModelIndex(), row, row);
    m_items.removeAt(row);
    endRemoveRows();
    emit countChanged();
    emit activeCountChanged();
}

void DownloadManager::clearFinished()
{
    for (int i = m_items.size() - 1; i >= 0; --i)
        if (m_items.at(i).state != Running && m_items.at(i).state != Queued)
            removeEntry(i);
}

void DownloadManager::openFile(int row)
{
    if (row < 0 || row >= m_items.size()) return;
    if (m_items.at(row).state != Finished) return;
    // Hands off to the platform's handler for the MIME type.
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_items.at(row).path));
}

// ---- stub only: pretend bytes are arriving -------------------------------
