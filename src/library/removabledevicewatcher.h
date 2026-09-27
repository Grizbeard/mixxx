#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

/// Tells the library features that list removable devices when one has been
/// mounted or unmounted, so that their sidebar trees follow without having to
/// be collapsed and expanded again.
///
/// It watches the directories devices get mounted under. Mounting a device
/// creates a directory in one of them and unmounting removes it, and that is
/// what this reacts to; activity inside a device does not reach it. A root
/// that does not exist yet - /run/media/$USER before anything has been mounted,
/// say - is picked up when it appears, provided its parent is below a top-level
/// directory: watching /run itself would wake up for every pid file.
///
/// Changes are coalesced. devicesChanged() is emitted once things have been
/// quiet for kSettleMs, by which time a mount that created its directory first
/// has finished.
class RemovableDeviceWatcher : public QObject {
    Q_OBJECT
  public:
    static constexpr int kSettleMs = 500;

    explicit RemovableDeviceWatcher(
            QStringList rootPaths, QObject* pParent = nullptr);

    /// The directories Mixxx looks in for removable devices: /media,
    /// /media/$USER and /run/media/$USER on Linux. Empty on other platforms,
    /// where a watcher built from them does nothing.
    static QStringList defaultRootPaths();

  signals:
    void devicesChanged();

  private:
    void slotDirectoryChanged();
    void updateWatchedPaths();

    const QStringList m_rootPaths;
    QFileSystemWatcher m_watcher;
    QTimer m_settleTimer;
};
