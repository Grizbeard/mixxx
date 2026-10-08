#pragma once

#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <memory>
#include <vector>

#include "library/trackset/crate/crateid.h"
#include "preferences/usersettings.h"
#include "track/trackid.h"

class ControlProxy;
class TrackCollectionManager;

namespace mixxx::stemconverter {

/// Where and how to convert. Persisted under [StemConverter] in mixxx.cfg.
struct Settings {
    QString executable;    // stemforge.exe
    QString outputRoot;    // the crate tree is mirrored as folders below it
    QString presetPath;    // a preset JSON file
    bool skipUnchanged = true;
    bool pauseWhilePlaying = true;

    static Settings load(const UserSettingsPointer& pConfig);
    void save(const UserSettingsPointer& pConfig) const;
};

/// The stemforge executable bundled next to this build in the dev layout
/// (<project>/stemforge/.venv/Scripts/stemforge.exe), or "stemforge" from PATH.
QString defaultExecutable();
/// Built-in presets beside the executable, then user presets in
/// <settings>/stemforge/presets. Absolute paths.
QStringList presetFiles(const QString& executable, const UserSettingsPointer& pConfig);
/// The "name" inside a preset file, or its file name.
QString presetDisplayName(const QString& presetPath);

enum class TrackState {
    Queued,
    Running,
    Done,
    Skipped, // already converted and unchanged
    Failed,
    Cancelled,
};

QString trackStateLabel(TrackState state);

struct TrackEntry {
    TrackId sourceId;
    QString sourcePath;
    QString outputPath;
    QString displayName;
    /// The mirrored crates the converted track goes into.
    QList<CrateId> targetCrates;
    TrackState state = TrackState::Queued;
    QString detail;
};

/// Converts crates of tracks into .stem.mp4 files with stemforge, one
/// background process at a time, and files the results in the library under
/// a "Stems" crate that mirrors the source crate tree.
///
/// stemforge talks JSON lines on stdout (stemforge/src/stemforge/protocol.py)
/// and takes pause/resume/cancel on stdin between tracks. While any deck is
/// playing the converter pauses it (when enabled), and the process always
/// runs at below-normal priority, so a set is never competing with it.
class StemConverter : public QObject {
    Q_OBJECT
  public:
    StemConverter(TrackCollectionManager* pTrackCollectionManager,
            UserSettingsPointer pConfig,
            QObject* pParent = nullptr);
    ~StemConverter() override;

    struct Plan {
        int tracks = 0;
        int crates = 0;
        int alreadyStems = 0; // tracks that are stem files themselves
        int withoutCrate = 0; // go straight into the "Stems" crate
    };
    Plan planCrate(CrateId crateId, bool includeSubcrates) const;
    Plan planTracks(const QList<TrackId>& trackIds) const;

    /// Queue the crate's tracks (and its subcrates' tracks), creating the
    /// mirrored crates right away. Returns the number of tracks queued.
    int enqueueCrate(CrateId crateId, bool includeSubcrates);

    /// Queue individual tracks. Each is filed under the mirror of every crate
    /// it is in, with the crates' full parent chain mirrored too; a track in
    /// no crate goes into the "Stems" crate itself.
    int enqueueTracks(const QList<TrackId>& trackIds);

    /// True for the "Stems" crate and anything below it, which are outputs.
    bool isInStemsTree(CrateId crateId) const;

    const QList<TrackEntry>& entries() const {
        return m_entries;
    }
    bool isRunning() const;
    bool isPaused() const {
        return m_userPaused || m_autoPaused;
    }
    /// Paused from the status window, as opposed to by a playing deck.
    bool isUserPaused() const {
        return m_userPaused;
    }

    static constexpr const char* kStemsRootCrateName = "Stems";

  public slots:
    void pause();
    void resume();
    void cancel();

  signals:
    void entriesAdded(int first, int count);
    void entryChanged(int index);
    void stateChanged();
    void message(const QString& text);

  private slots:
    void slotReadStdout();
    void slotReadStderr();
    void slotFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void slotPollDecks();

  private:
    /// One track to convert, and the source crates to file the result under.
    struct Request {
        TrackId trackId;
        QList<CrateId> crates;
    };
    QList<Request> requestsForCrate(CrateId crateId, bool includeSubcrates) const;
    QList<Request> requestsForTracks(const QList<TrackId>& trackIds) const;
    Plan plan(const QList<Request>& requests) const;
    int enqueue(const QList<Request>& requests, QHash<CrateId, CrateId>* pMirrors);

    /// The crate and its subcrates, parents before children.
    QList<CrateId> walkCrates(CrateId crateId, bool includeSubcrates) const;
    /// Top-level crate first, crateId last.
    QList<CrateId> ancestry(CrateId crateId) const;
    QStringList crateNamePath(CrateId crateId) const;
    /// Crates holding the track, outside the "Stems" tree.
    QList<CrateId> sourceCratesOf(TrackId trackId) const;
    /// The crate whose path names the track's output folder.
    CrateId folderCrateOf(TrackId trackId) const;

    CrateId stemsRootCrate(bool create);
    CrateId mirrorCrate(CrateId sourceCrateId, CrateId mirrorParentId);
    /// The mirror of crateId, creating mirrors for its whole parent chain.
    CrateId mirrorPath(CrateId crateId, CrateId rootId, QHash<CrateId, CrateId>* pCache);

    void startNextBatch();
    void handleEvent(const QJsonObject& event);
    void importConverted(int entryIndex, const QJsonObject& event);
    void setState(int entryIndex, TrackState state, const QString& detail = QString());
    void sendCommand(const QByteArray& command);

    TrackCollectionManager* const m_pTrackCollectionManager;
    const UserSettingsPointer m_pConfig;

    QList<TrackEntry> m_entries;
    QList<int> m_pending; // entry indices not yet handed to a process
    QList<int> m_batch;   // entry index for each track of the running job
    int m_runningTrack = -1;

    QProcess* m_pProcess = nullptr;
    QByteArray m_stdoutBuffer;
    QFile m_log;

    bool m_userPaused = false;
    bool m_autoPaused = false;
    QTimer m_deckPollTimer;
    std::unique_ptr<ControlProxy> m_pNumDecks;
    std::vector<std::unique_ptr<ControlProxy>> m_playControls;
};

} // namespace mixxx::stemconverter
