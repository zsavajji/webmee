#ifndef HISTORYSTORE_H
#define HISTORYSTORE_H

// Visited pages, for the address bar's suggestions and the history list.
//
// QSettings INI like BookmarkStore, for the same reasons -- but capped, because
// unlike favourites this grows on its own. Oldest entries fall off the end.
//
// Suggestions are returned as a plain list rather than through a filter proxy
// model: QML1 takes a JS array as a model directly, so a proxy would be a class
// to maintain for no behaviour.
//
// Plain C++03: built with the Harmattan SDK's GCC 4.4.

#include <QAbstractListModel>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QVariantList>

class HistoryStore : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        TitleRole = Qt::UserRole + 1,
        UrlRole,
        HostRole,
        VisitedRole
    };

    explicit HistoryStore(QObject *parent = 0);
    ~HistoryStore();

    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role) const;

    int count() const { return m_items.size(); }

public slots:
    // Upsert by URL: revisiting moves an entry to the top and refreshes its
    // title rather than adding a duplicate.
    void visit(const QString &url, const QString &title);

    // Best matches for what has been typed so far, most recently visited first.
    // Each entry is { url, title, host }.
    QVariantList suggest(const QString &prefix, int limit = 6) const;

    void removeAt(int row);
    void clear();
    QString urlAt(int row) const;

signals:
    void countChanged();

private:
    struct Entry {
        QString url;
        QString title;
        QDateTime visited;
    };

    void load();
    void save();
    int indexOfUrl(const QString &url) const;

    QList<Entry> m_items;
    QString m_path;
};

#endif
