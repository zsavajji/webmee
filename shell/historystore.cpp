#include "historystore.h"

#include <QDir>
#include <QSettings>
#include <QUrl>

// Enough to make suggestions useful, small enough that loading it at startup on
// a 2011 phone is not something the user waits for.
static const int kMaxEntries = 400;

HistoryStore::HistoryStore(QObject *parent)
    : QAbstractListModel(parent)
{
    QHash<int, QByteArray> roles;
    roles[TitleRole] = "title";
    roles[UrlRole] = "url";
    roles[HostRole] = "host";
    roles[VisitedRole] = "visited";
    setRoleNames(roles);      // Qt 4 spelling; Qt 5 renamed this to roleNames()

    QString dir = QDir::homePath() + "/.config/WebMee";
    QDir().mkpath(dir);
    m_path = dir + "/history.conf";
    load();
}

HistoryStore::~HistoryStore()
{
    save();
}

int HistoryStore::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant HistoryStore::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return QVariant();
    const Entry &e = m_items.at(index.row());
    switch (role) {
    case TitleRole:   return e.title.isEmpty() ? e.url : e.title;
    case UrlRole:     return e.url;
    case HostRole:    return QUrl(e.url).host();
    case VisitedRole: return e.visited;
    default:          return QVariant();
    }
}

int HistoryStore::indexOfUrl(const QString &url) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).url == url)
            return i;
    return -1;
}

void HistoryStore::visit(const QString &url, const QString &title)
{
    // about:blank and the like are navigation plumbing, not places.
    if (url.isEmpty() || url.startsWith("about:") || url.startsWith("data:"))
        return;

    const int existing = indexOfUrl(url);
    if (existing >= 0) {
        Entry e = m_items.at(existing);
        if (!title.isEmpty())
            e.title = title;
        e.visited = QDateTime::currentDateTime();
        beginRemoveRows(QModelIndex(), existing, existing);
        m_items.removeAt(existing);
        endRemoveRows();
        beginInsertRows(QModelIndex(), 0, 0);
        m_items.prepend(e);
        endInsertRows();
        save();
        return;
    }

    Entry e;
    e.url = url;
    e.title = title;
    e.visited = QDateTime::currentDateTime();
    beginInsertRows(QModelIndex(), 0, 0);
    m_items.prepend(e);
    endInsertRows();

    while (m_items.size() > kMaxEntries) {
        const int last = m_items.size() - 1;
        beginRemoveRows(QModelIndex(), last, last);
        m_items.removeAt(last);
        endRemoveRows();
    }

    emit countChanged();
    save();
}

QVariantList HistoryStore::suggest(const QString &prefix, int limit) const
{
    QVariantList out;
    const QString needle = prefix.trimmed().toLower();
    if (needle.isEmpty())
        return out;

    for (int i = 0; i < m_items.size() && out.size() < limit; ++i) {
        const Entry &e = m_items.at(i);
        // Match the URL or the title: people remember either.
        if (!e.url.toLower().contains(needle) && !e.title.toLower().contains(needle))
            continue;
        QVariantMap m;
        m["url"] = e.url;
        m["title"] = e.title.isEmpty() ? e.url : e.title;
        m["host"] = QUrl(e.url).host();
        out.append(m);
    }
    return out;
}

void HistoryStore::removeAt(int row)
{
    if (row < 0 || row >= m_items.size())
        return;
    beginRemoveRows(QModelIndex(), row, row);
    m_items.removeAt(row);
    endRemoveRows();
    emit countChanged();
    save();
}

void HistoryStore::clear()
{
    if (m_items.isEmpty())
        return;
    beginRemoveRows(QModelIndex(), 0, m_items.size() - 1);
    m_items.clear();
    endRemoveRows();
    emit countChanged();
    save();
}

QString HistoryStore::urlAt(int row) const
{
    return (row >= 0 && row < m_items.size()) ? m_items.at(row).url : QString();
}

void HistoryStore::load()
{
    QSettings s(m_path, QSettings::IniFormat);
    const int n = s.beginReadArray("history");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        Entry e;
        e.url = s.value("url").toString();
        e.title = s.value("title").toString();
        e.visited = s.value("visited").toDateTime();
        if (!e.url.isEmpty())
            m_items.append(e);
    }
    s.endArray();
}

void HistoryStore::save()
{
    QSettings s(m_path, QSettings::IniFormat);
    s.remove("history");
    s.beginWriteArray("history", m_items.size());
    for (int i = 0; i < m_items.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("url", m_items.at(i).url);
        s.setValue("title", m_items.at(i).title);
        s.setValue("visited", m_items.at(i).visited);
    }
    s.endArray();
    s.sync();
}
