#ifndef BOOKMARKSTORE_H
#define BOOKMARKSTORE_H

// Favourites, exposed to QML as a list model and persisted with QSettings.
//
// QSettings rather than SQLite: bookmarks are a short, ordered list that is read
// once at startup and written on change, so a database buys nothing and costs a
// dependency. The file is plain INI, which also means it can be edited or backed
// up by hand -- worth something on a device where the browser is the only way to
// get data off it.
//
// Plain C++03: this half is built with the Harmattan SDK's GCC 4.4 for ABI
// compatibility with the device's Qt 4.7.4.

#include <QAbstractListModel>
#include <QList>
#include <QString>

class BookmarkStore : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        TitleRole = Qt::UserRole + 1,
        UrlRole,
        HostRole            // shown under the title; cheaper than a favicon
    };

    explicit BookmarkStore(QObject *parent = 0);
    ~BookmarkStore();

    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role) const;

    int count() const { return m_items.size(); }

public slots:
    // Returns the new bookmarked state, so QML can bind a star to it directly.
    bool toggle(const QString &url, const QString &title);

    void add(const QString &url, const QString &title);
    void removeAt(int row);
    void removeUrl(const QString &url);
    bool contains(const QString &url) const;
    void setTitle(int row, const QString &title);
    void move(int from, int to);
    void clear();

    QString urlAt(int row) const;
    QString titleAt(int row) const;

signals:
    void countChanged();
    // Lets the URL bar's star re-evaluate without knowing which row changed.
    void bookmarksChanged();

private:
    struct Bookmark {
        QString title;
        QString url;
    };

    void load();
    void save() const;
    int indexOfUrl(const QString &url) const;
    static QString normalise(const QString &url);
    static QString hostOf(const QString &url);

    QList<Bookmark> m_items;
    QString m_path;
};

#endif
