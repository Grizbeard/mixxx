// Tests for RemovableDeviceWatcher, against temporary directories standing in
// for the places devices get mounted under.

#include "library/removabledevicewatcher.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "test/mixxxtest.h"

namespace {

// Comfortably more than the settle time, for a slow machine.
constexpr int kWaitMs = RemovableDeviceWatcher::kSettleMs + 3000;

class RemovableDeviceWatcherTest : public MixxxTest {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_tempDir.isValid());
        m_base = QDir(m_tempDir.path());
    }

    QString path(const QString& relative) const {
        return m_base.filePath(relative);
    }

    QTemporaryDir m_tempDir;
    QDir m_base;
};

TEST_F(RemovableDeviceWatcherTest, MountAndUnmountAreEachReported) {
    ASSERT_TRUE(m_base.mkpath("run/media/user"));
    RemovableDeviceWatcher watcher({path("run/media/user")});
    QSignalSpy spy(&watcher, &RemovableDeviceWatcher::devicesChanged);

    ASSERT_TRUE(m_base.mkdir("run/media/user/MUSIC-1A2B3C4D"));
    ASSERT_TRUE(spy.wait(kWaitMs));
    EXPECT_EQ(spy.count(), 1);

    ASSERT_TRUE(m_base.rmdir("run/media/user/MUSIC-1A2B3C4D"));
    ASSERT_TRUE(spy.wait(kWaitMs));
    EXPECT_EQ(spy.count(), 2);
}

TEST_F(RemovableDeviceWatcherTest, ABurstOfChangesIsReportedOnce) {
    ASSERT_TRUE(m_base.mkpath("media"));
    RemovableDeviceWatcher watcher({path("media")});
    QSignalSpy spy(&watcher, &RemovableDeviceWatcher::devicesChanged);

    ASSERT_TRUE(m_base.mkdir("media/A"));
    ASSERT_TRUE(m_base.mkdir("media/B"));
    ASSERT_TRUE(m_base.mkdir("media/C"));
    ASSERT_TRUE(spy.wait(kWaitMs));
    // Nothing more once it has settled.
    EXPECT_FALSE(spy.wait(RemovableDeviceWatcher::kSettleMs * 2));
    EXPECT_EQ(spy.count(), 1);
}

// /run/media/$USER does not exist until something is first mounted there.
TEST_F(RemovableDeviceWatcherTest, ARootThatAppearsLaterIsWatched) {
    ASSERT_TRUE(m_base.mkpath("run/media"));
    RemovableDeviceWatcher watcher({path("run/media/user")});
    QSignalSpy spy(&watcher, &RemovableDeviceWatcher::devicesChanged);

    ASSERT_TRUE(m_base.mkpath("run/media/user"));
    ASSERT_TRUE(spy.wait(kWaitMs));
    const int afterRoot = spy.count();

    ASSERT_TRUE(m_base.mkdir("run/media/user/USB-5E6F"));
    ASSERT_TRUE(spy.wait(kWaitMs));
    EXPECT_EQ(spy.count(), afterRoot + 1);
}

TEST_F(RemovableDeviceWatcherTest, ChangesInsideADeviceAreNotReported) {
    ASSERT_TRUE(m_base.mkpath("media/MUSIC/Tracks"));
    RemovableDeviceWatcher watcher({path("media")});
    QSignalSpy spy(&watcher, &RemovableDeviceWatcher::devicesChanged);

    ASSERT_TRUE(m_base.mkdir("media/MUSIC/Tracks/New"));
    EXPECT_FALSE(spy.wait(RemovableDeviceWatcher::kSettleMs * 2));
}

TEST_F(RemovableDeviceWatcherTest, NoRootsMeansNothingToDo) {
    RemovableDeviceWatcher watcher({});
    QSignalSpy spy(&watcher, &RemovableDeviceWatcher::devicesChanged);
    EXPECT_FALSE(spy.wait(RemovableDeviceWatcher::kSettleMs * 2));
}

} // namespace
