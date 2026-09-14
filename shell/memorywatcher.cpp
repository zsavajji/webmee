#include "memorywatcher.h"
#include <QSocketNotifier>
#include <QFile>
#include <QByteArray>
#include <QDebug>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

MemoryWatcher::MemoryWatcher(QObject *parent)
    : QObject(parent)
    , m_source("none")
    , m_usageFd(-1)
    , m_lowEventFd(-1)
    , m_criticalEventFd(-1)
    , m_lowNotifier(0)
    , m_criticalNotifier(0)
    , m_level(Normal)
    , m_availableMb(0)
    , m_lowMb(140)
    , m_criticalMb(70)
    , m_totalKb(0)
{
    m_totalKb = totalMemoryKb();
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(sampleMeminfo()));
}

MemoryWatcher::~MemoryWatcher()
{
    teardownCgroup();
}

// Plain stdio, not QFile: procfs files report size 0, and QFile::atEnd() is
// defined as bytesAvailable()==0, which for a non-sequential file is
// size()-pos(). So `while (!f.atEnd())` never executes a single iteration on
// /proc. That silently returned 0 here, which made setupCgroup() bail on the
// m_totalKb check and fall back to polling -- with the poller equally broken.
long MemoryWatcher::totalMemoryKb()
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return 0;
    char line[256];
    long kb = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemTotal: %ld kB", &kb) == 1)
            break;
    }
    fclose(f);
    return kb;
}

void MemoryWatcher::setThresholds(int lowMb, int criticalMb)
{
    m_lowMb = lowMb;
    m_criticalMb = criticalMb;
}

// Find the cgroup mount carrying the memory controller. Harmattan uses
// /syspart, but read /proc/mounts rather than hardcoding it.
bool MemoryWatcher::setupCgroup()
{
    FILE *mounts = fopen("/proc/mounts", "r");
    if (!mounts)
        return false;

    QString path;
    char line[512], dev[128], mnt[256], type[64], opts[256];
    while (fgets(line, sizeof line, mounts)) {
        if (sscanf(line, "%127s %255s %63s %255s", dev, mnt, type, opts) != 4)
            continue;
        if (strcmp(type, "cgroup") == 0 && strstr(opts, "memory")) {
            path = QString::fromLocal8Bit(mnt);
            break;
        }
    }
    fclose(mounts);
    if (path.isEmpty())
        return false;

    m_cgroupPath = path;
    m_usageFd = ::open(QString(path + "/memory.usage_in_bytes").toLocal8Bit().constData(), O_RDONLY);
    if (m_usageFd < 0)
        return false;

    if (m_totalKb <= 0)
        return false;

    // Available-MB thresholds become usage thresholds: usage = total - available.
    const int lowUsageBytes = int((m_totalKb - qint64(m_lowMb) * 1024) * 1024);
    const int critUsageBytes = int((m_totalKb - qint64(m_criticalMb) * 1024) * 1024);

    bool ok = registerThreshold(lowUsageBytes, &m_lowNotifier, &m_lowEventFd);
    ok = registerThreshold(critUsageBytes, &m_criticalNotifier, &m_criticalEventFd) && ok;
    if (!ok) {
        teardownCgroup();
        return false;
    }

    m_source = "cgroup:" + path;
    return true;
}

// One eventfd per threshold: cgroup.event_control takes a single threshold per
// registration, and fires when usage crosses it in EITHER direction.
bool MemoryWatcher::registerThreshold(int usageBytesThreshold, QSocketNotifier **outNotifier, int *outEventFd)
{
    int efd = ::eventfd(0, 0);
    if (efd < 0)
        return false;

    int cfd = ::open(QString(m_cgroupPath + "/cgroup.event_control").toLocal8Bit().constData(), O_WRONLY);
    if (cfd < 0) { ::close(efd); return false; }

    bool ok = ::dprintf(cfd, "%d %d %u", efd, m_usageFd, (unsigned) usageBytesThreshold) > 0;
    ::close(cfd);
    if (!ok) { ::close(efd); return false; }

    *outEventFd = efd;
    *outNotifier = new QSocketNotifier(efd, QSocketNotifier::Read, this);
    connect(*outNotifier, SIGNAL(activated(int)), this, SLOT(onCgroupEvent()));
    return true;
}

void MemoryWatcher::teardownCgroup()
{
    delete m_lowNotifier; m_lowNotifier = 0;
    delete m_criticalNotifier; m_criticalNotifier = 0;
    if (m_lowEventFd >= 0) { ::close(m_lowEventFd); m_lowEventFd = -1; }
    if (m_criticalEventFd >= 0) { ::close(m_criticalEventFd); m_criticalEventFd = -1; }
    if (m_usageFd >= 0) { ::close(m_usageFd); m_usageFd = -1; }
}

long MemoryWatcher::cgroupUsageKb() const
{
    if (m_usageFd < 0)
        return -1;
    char buf[64];
    if (::lseek(m_usageFd, 0, SEEK_SET) < 0)
        return -1;
    ssize_t n = ::read(m_usageFd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return long(strtoll(buf, 0, 10) / 1024);
}

void MemoryWatcher::start(int pollIntervalMs)
{
    if (setupCgroup()) {
        recompute();                 // establish the starting level
        qDebug() << "MemoryWatcher:" << m_source << "available" << m_availableMb << "MB";
        return;
    }
    m_source = "meminfo";
    qWarning() << "MemoryWatcher: cgroup unavailable, polling /proc/meminfo";
    m_timer.start(pollIntervalMs);
    sampleMeminfo();
}

void MemoryWatcher::stop()
{
    m_timer.stop();
    teardownCgroup();
}

void MemoryWatcher::onCgroupEvent()
{
    // The eventfd counter must be drained or the notifier re-fires forever.
    uint64_t counter = 0;
    if (m_lowEventFd >= 0)      { ssize_t r = ::read(m_lowEventFd, &counter, sizeof(counter)); (void)r; }
    if (m_criticalEventFd >= 0) { ssize_t r = ::read(m_criticalEventFd, &counter, sizeof(counter)); (void)r; }
    recompute();
}

void MemoryWatcher::recompute()
{
    long usedKb = cgroupUsageKb();
    if (usedKb < 0 || m_totalKb <= 0)
        return;

    int available = int((m_totalKb - usedKb) / 1024);
    if (available != m_availableMb) {
        m_availableMb = available;
        emit levelChanged();
    }

    int newLevel = Normal;
    if (available <= m_criticalMb)   newLevel = Critical;
    else if (available <= m_lowMb)   newLevel = Low;

    if (newLevel != m_level) {
        m_level = newLevel;
        emit levelChanged();
        emit pressure(m_level);
    }
}

// Fallback: no cgroup available.
void MemoryWatcher::sampleMeminfo()
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return;

    long memFree = 0, buffers = 0, cached = 0, v = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemFree: %ld kB", &v) == 1)      memFree = v;
        else if (sscanf(line, "Buffers: %ld kB", &v) == 1) buffers = v;
        else if (sscanf(line, "Cached: %ld kB", &v) == 1)  cached  = v;
    }
    fclose(f);

    // This kernel predates MemAvailable (Linux 3.14), so approximate it the way
    // that field is defined: free plus reclaimable page cache. MemFree alone
    // reads alarmingly low on a perfectly healthy system.
    int available = int((memFree + buffers + cached) / 1024);
    if (available != m_availableMb) {
        m_availableMb = available;
        emit levelChanged();
    }

    int newLevel = Normal;
    if (available <= m_criticalMb)   newLevel = Critical;
    else if (available <= m_lowMb)   newLevel = Low;

    if (newLevel != m_level) {
        m_level = newLevel;
        emit levelChanged();
        emit pressure(m_level);
    }
}
