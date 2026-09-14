#ifndef MEMORYWATCHER_H
#define MEMORYWATCHER_H

// Memory pressure -- the thing most likely to decide whether this browser is
// usable. ~1 GB total, ~250 MB free at rest, and the swap is ramzswap
// (compressed RAM), so reclaiming costs CPU on the single core rather than
// buying real capacity.
//
// Mechanism: the cgroup memory threshold notifier. Harmattan mounts a cgroup
// hierarchy at /syspart with the memory controller and exposes
// cgroup.event_control, which gives PUSH notification instead of polling:
//
//     efd = eventfd(0, 0);
//     cfd = open("<cgroup>/cgroup.event_control", O_WRONLY);
//     dprintf(cfd, "%d %d %u", efd, usage_fd, threshold_bytes);
//     // read(efd) now completes when usage crosses threshold_bytes
//
// That is exactly what Harmattan's own libmemnotify does in its "cgroups"
// watcher (maemo-tools-old/libmemnotify, src/cgroups_watcher.cpp). We implement
// it directly rather than linking libmemnotifyqt for two reasons: the device's
// /dev/memnotify sensor -- the library's default -- does not exist on this
// kernel, and pressure handling is needed most in the ENGINE process, which is
// GCC-14/WPE with no Qt at all.
//
// Falls back to polling /proc/meminfo if the cgroup is unavailable.

#include <QObject>
#include <QTimer>
#include <QString>

class QSocketNotifier;

class MemoryWatcher : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int level READ level NOTIFY levelChanged)
    Q_PROPERTY(int availableMb READ availableMb NOTIFY levelChanged)
    Q_PROPERTY(QString source READ source CONSTANT)

public:
    enum Level { Normal = 0, Low = 1, Critical = 2 };

    explicit MemoryWatcher(QObject *parent = 0);
    ~MemoryWatcher();

    int level() const { return m_level; }
    int availableMb() const { return m_availableMb; }
    QString source() const { return m_source; }

    // Thresholds are in MB of AVAILABLE memory; converted internally to the
    // cgroup's usage-based thresholds.
    void setThresholds(int lowMb, int criticalMb);
    void start(int pollIntervalMs = 4000);
    void stop();

signals:
    void levelChanged();
    void pressure(int level);      // transitions only, so handlers do not thrash

private slots:
    void onCgroupEvent();
    void sampleMeminfo();

private:
    bool setupCgroup();
    void teardownCgroup();
    bool registerThreshold(int usageBytesThreshold, QSocketNotifier **outNotifier, int *outEventFd);
    void recompute();
    static long totalMemoryKb();
    long cgroupUsageKb() const;

    QString m_cgroupPath;
    QString m_source;
    int m_usageFd;                 // open fd on memory.usage_in_bytes
    int m_lowEventFd;
    int m_criticalEventFd;
    QSocketNotifier *m_lowNotifier;
    QSocketNotifier *m_criticalNotifier;

    QTimer m_timer;                // fallback path only
    int m_level;
    int m_availableMb;
    int m_lowMb;
    int m_criticalMb;
    long m_totalKb;
};

#endif
