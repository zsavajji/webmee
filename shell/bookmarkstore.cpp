#include "bookmarkstore.h"
#include <QSettings>
#include <QDir>
#include <QUrl>
#include <QDebug>

BookmarkStore::BookmarkStore(QObject *parent)
    : QAbstractListModel(parent)
{
    QHash<int, QByteArray> roles;
    roles[TitleRole] = "title";
    roles[UrlRole]   = "url";
    roles[HostRole]  = "host";
    setRoleNames(roles);            // QML1 needs role names declared up front

    QString dir = QDir::homePath() + "/.config/WebMee";
    QDir().mkpath(dir);
    m_path = dir + "/bookmarks.conf";

    load();
}

BookmarkStore::~BookmarkStore()
{
    save();
}

int BookmarkStore::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant BookmarkStore::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return QVariant();
    const Bookmark &b = m_items.at(index.row());

    switch (role) {
    case TitleRole: return b.title.isEmpty() ? hostOf(b.url) : b.title;
    case UrlRole:   return b.url;
    case HostRole:  return hostOf(b.url);
    }
    return QVariant();
}

// Compare bookmarks without being fooled by a trailing slash or a differing
// scheme case, which is how the same page ends up starred twice.
QString BookmarkStore::normalise(const QString &url)
{
    QString s = url.trimmed();
    if (s.endsWith('/'))
        s.chop(1);
    int sep = s.indexOf("://");
    if (sep > 0)
        return s.left(sep).toLower() + s.mid(sep);
    return s;
}

QString BookmarkStore::hostOf(const QString &url)
{
    QUrl u(url);
    QString h = u.host();
    if (h.isEmpty())
        return url;
    if (h.startsWith("www."))
        h = h.mid(4);
    return h;
}

int BookmarkStore::indexOfUrl(const QString &url) const
{
    const QString key = normalise(url);
    for (int i = 0; i < m_items.size(); ++i)
        if (normalise(m_items.at(i).url) == key)
            return i;
    return -1;
}

bool BookmarkStore::contains(const QString &url) const
{
    return indexOfUrl(url) >= 0;
}

void BookmarkStore::add(const QString &url, const QString &title)
{
    if (url.trimmed().isEmpty() || url == "about:blank")
        return;
    if (contains(url))
        return;

    Bookmark b;
    b.url = url.trimmed();
    b.title = title.trimmed();

    // Newest first: on a small screen the most recent addition should be the
    // one you do not have to scroll to.
    beginInsertRows(QModelIndex(), 0, 0);
    m_items.prepend(b);
    endInsertRows();

    save();
    emit countChanged();
    emit bookmarksChanged();
}

void BookmarkStore::removeAt(int row)
{
    if (row < 0 || row >= m_items.size())
        return;
    beginRemoveRows(QModelIndex(), row, row);
    m_items.removeAt(row);
    endRemoveRows();
    save();
    emit countChanged();
    emit bookmarksChanged();
}

void BookmarkStore::removeUrl(const QString &url)
{
    removeAt(indexOfUrl(url));
}

bool BookmarkStore::toggle(const QString &url, const QString &title)
{
    int row = indexOfUrl(url);
    if (row >= 0) {
        removeAt(row);
        return false;
    }
    add(url, title);
    return true;
}

void BookmarkStore::setTitle(int row, const QString &title)
{
    if (row < 0 || row >= m_items.size())
        return;
    m_items[row].title = title;
    QModelIndex ix = index(row, 0);
    emit dataChanged(ix, ix);
    save();
}

void BookmarkStore::move(int from, int to)
{
    if (from < 0 || from >= m_items.size() || to < 0 || to >= m_items.size() || from == to)
        return;
    // beginMoveRows wants the destination in pre-move coordinates, which is one
    // further along when moving down.
    if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to))
        return;
    m_items.move(from, to);
    endMoveRows();
    save();
    emit bookmarksChanged();
}

void BookmarkStore::clear()
{
    if (m_items.isEmpty())
        return;
    beginResetModel();
    m_items.clear();
    endResetModel();
    save();
    emit countChanged();
    emit bookmarksChanged();
}

QString BookmarkStore::urlAt(int row) const
{
    return (row >= 0 && row < m_items.size()) ? m_items.at(row).url : QString();
}

QString BookmarkStore::titleAt(int row) const
{
    return (row >= 0 && row < m_items.size()) ? m_items.at(row).title : QString();
}

void BookmarkStore::load()
{
    QSettings s(m_path, QSettings::IniFormat);
    int n = s.beginReadArray("bookmarks");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        Bookmark b;
        b.url = s.value("url").toString();
        b.title = s.value("title").toString();
        if (!b.url.isEmpty())
            m_items.append(b);
    }
    s.endArray();
}

void BookmarkStore::save() const
{
    QSettings s(m_path, QSettings::IniFormat);
    // remove() before the write: without it, shrinking the list leaves stale
    // entries behind, because QSettings arrays are written by index.
    s.remove("bookmarks");
    s.beginWriteArray("bookmarks", m_items.size());
    for (int i = 0; i < m_items.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("url", m_items.at(i).url);
        s.setValue("title", m_items.at(i).title);
    }
    s.endArray();
    s.sync();
}
