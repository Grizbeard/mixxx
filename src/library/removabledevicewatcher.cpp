#include "library/removabledevicewatcher.h"

#include <QDir>
#include <QFileInfo>

#include "moc_removabledevicewatcher.cpp"

namespace {

/// "/run" is 1, "/run/media" is 2.
int pathDepth(const QString& path) {
    return static_cast<int>(QDir::cleanPath(path).count(QLatin1Char('/')));
}

} // namespace

RemovableDeviceWatcher::RemovableDeviceWatcher(
        QStringList rootPaths, QObject* pParent)
        : QObject(pParent),
          m_rootPaths(std::move(rootPaths)) {
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(kSettleMs);
    connect(&m_settleTimer,
            &QTimer::timeout,
            this,
            &RemovableDeviceWatcher::devicesChanged);
    connect(&m_watcher,
            &QFileSystemWatcher::directoryChanged,
            this,
            &RemovableDeviceWatcher::slotDirectoryChanged);
    updateWatchedPaths();
}

// static
QStringList RemovableDeviceWatcher::defaultRootPaths() {
#if defined(__LINUX__)
    const QString user = QString::fromLocal8Bit(qgetenv("USER"));
    return {
            QStringLiteral("/media"),
            QStringLiteral("/media/") + user,
            QStringLiteral("/run/media/") + user,
    };
#else
    return {};
#endif
}

void RemovableDeviceWatcher::slotDirectoryChanged() {
    // A root may have just appeared, or gone away with its parent still here.
    updateWatchedPaths();
    m_settleTimer.start();
}

void RemovableDeviceWatcher::updateWatchedPaths() {
    QStringList wanted;
    for (const QString& root : m_rootPaths) {
        const QFileInfo rootInfo(root);
        if (rootInfo.isDir()) {
            wanted.append(rootInfo.absoluteFilePath());
            continue;
        }
        const QString parent = rootInfo.absolutePath();
        if (pathDepth(parent) >= 2 && QFileInfo(parent).isDir()) {
            wanted.append(parent);
        }
    }
    wanted.removeDuplicates();

    const QStringList watched = m_watcher.directories();
    for (const QString& path : watched) {
        if (!wanted.contains(path)) {
            m_watcher.removePath(path);
        }
    }
    for (const QString& path : std::as_const(wanted)) {
        if (!watched.contains(path)) {
            m_watcher.addPath(path);
        }
    }
}
