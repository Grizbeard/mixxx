#include "library/stemconverter/stemconverter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <functional>
#include <optional>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include "control/controlproxy.h"
#include "library/stemconverter/stemtrackimporter.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/trackset/crate/crate.h"
#include "library/trackset/crate/cratefeaturehelper.h"
#include "mixer/playermanager.h"
#include "moc_stemconverter.cpp"
#include "track/track.h"
#include "track/trackref.h"
#include "util/logger.h"

namespace mixxx::stemconverter {

namespace {

const Logger kLogger("StemConverter");

const QString kConfigGroup = QStringLiteral("[StemConverter]");
const ConfigKey kExecutableKey(kConfigGroup, QStringLiteral("Executable"));
const ConfigKey kOutputRootKey(kConfigGroup, QStringLiteral("OutputRoot"));
const ConfigKey kPresetKey(kConfigGroup, QStringLiteral("Preset"));
const ConfigKey kSkipUnchangedKey(kConfigGroup, QStringLiteral("SkipUnchanged"));
const ConfigKey kPauseWhilePlayingKey(kConfigGroup, QStringLiteral("PauseWhilePlaying"));

const QString kMirrorSuffix = QStringLiteral(" (Stems)");
const QString kStemExtension = QStringLiteral(".stem.mp4");
constexpr int kDeckPollIntervalMs = 1000;

QString defaultOutputRoot() {
    // The library drive in the setup this was built for; elsewhere, Music.
    if (QDir(QStringLiteral("D:/")).exists()) {
        return QStringLiteral("D:/Stems");
    }
    return QDir(QStandardPaths::writableLocation(QStandardPaths::MusicLocation))
            .filePath(QStringLiteral("Stems"));
}

QString stemforgeDir(const UserSettingsPointer& pConfig) {
    return QDir(pConfig->getSettingsPath()).filePath(QStringLiteral("stemforge"));
}

/// A name that is safe as a Windows file or folder name.
QString sanitizeFileName(const QString& name) {
    static const QRegularExpression kForbidden(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])"));
    QString clean = name;
    clean.replace(kForbidden, QStringLiteral("_"));
    // Windows drops trailing dots and spaces silently, which would make two
    // different names collide.
    while (clean.endsWith(QChar('.')) || clean.endsWith(QChar(' '))) {
        clean.chop(1);
    }
    clean = clean.trimmed();
    return clean.isEmpty() ? QStringLiteral("_") : clean;
}

QString stateLabel(TrackState state) {
    switch (state) {
    case TrackState::Queued:
        return QObject::tr("Queued");
    case TrackState::Running:
        return QObject::tr("Converting");
    case TrackState::Done:
        return QObject::tr("Done");
    case TrackState::Skipped:
        return QObject::tr("Up to date");
    case TrackState::Failed:
        return QObject::tr("Failed");
    case TrackState::Cancelled:
        return QObject::tr("Cancelled");
    }
    return {};
}

} // namespace

// -- Settings ---------------------------------------------------------------

Settings Settings::load(const UserSettingsPointer& pConfig) {
    Settings s;
    s.executable = pConfig->getValue(kExecutableKey, QString());
    if (s.executable.isEmpty() || !QFileInfo::exists(s.executable)) {
        s.executable = defaultExecutable();
    }
    s.outputRoot = pConfig->getValue(kOutputRootKey, defaultOutputRoot());
    s.presetPath = pConfig->getValue(kPresetKey, QString());
    const QStringList presets = presetFiles(s.executable, pConfig);
    if (!presets.contains(s.presetPath)) {
        s.presetPath.clear();
        for (const QString& path : presets) {
            if (QFileInfo(path).baseName() == QStringLiteral("quality-4gb")) {
                s.presetPath = path;
            }
        }
        if (s.presetPath.isEmpty() && !presets.isEmpty()) {
            s.presetPath = presets.first();
        }
    }
    s.skipUnchanged = pConfig->getValue(kSkipUnchangedKey, true);
    s.pauseWhilePlaying = pConfig->getValue(kPauseWhilePlayingKey, true);
    return s;
}

void Settings::save(const UserSettingsPointer& pConfig) const {
    pConfig->setValue(kExecutableKey, executable);
    pConfig->setValue(kOutputRootKey, outputRoot);
    pConfig->setValue(kPresetKey, presetPath);
    pConfig->setValue(kSkipUnchangedKey, skipUnchanged);
    pConfig->setValue(kPauseWhilePlayingKey, pauseWhilePlaying);
}

QString defaultExecutable() {
    // <project>/mixxx/build/<preset>/mixxx.exe -> <project>/stemforge/...
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString bundled = QDir::cleanPath(appDir.filePath(
            QStringLiteral("../../../stemforge/.venv/Scripts/stemforge.exe")));
    if (QFileInfo::exists(bundled)) {
        return bundled;
    }
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("stemforge"));
    return onPath.isEmpty() ? bundled : onPath;
}

QStringList presetFiles(const QString& executable, const UserSettingsPointer& pConfig) {
    QStringList files;
    // stemforge.exe sits in <stemforge>/.venv/Scripts; presets in <stemforge>/presets.
    const QDir builtin(QDir::cleanPath(QFileInfo(executable).absolutePath() +
            QStringLiteral("/../../presets")));
    const QDir user(QDir(stemforgeDir(pConfig)).filePath(QStringLiteral("presets")));
    for (const QDir& dir : {builtin, user}) {
        const QFileInfoList found = dir.entryInfoList(
                {QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QFileInfo& info : found) {
            files.append(info.absoluteFilePath());
        }
    }
    return files;
}

QString presetDisplayName(const QString& presetPath) {
    QFile file(presetPath);
    if (file.open(QIODevice::ReadOnly)) {
        const QString name = QJsonDocument::fromJson(file.readAll())
                                     .object()
                                     .value(QStringLiteral("name"))
                                     .toString();
        if (!name.isEmpty()) {
            return name;
        }
    }
    return QFileInfo(presetPath).baseName();
}

// -- StemConverter ----------------------------------------------------------

StemConverter::StemConverter(TrackCollectionManager* pTrackCollectionManager,
        UserSettingsPointer pConfig,
        QObject* pParent)
        : QObject(pParent),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_pConfig(std::move(pConfig)) {
    m_deckPollTimer.setInterval(kDeckPollIntervalMs);
    connect(&m_deckPollTimer, &QTimer::timeout, this, &StemConverter::slotPollDecks);
}

StemConverter::~StemConverter() {
    if (m_pProcess) {
        m_pProcess->disconnect(this);
        m_pProcess->kill();
        m_pProcess->waitForFinished(3000);
    }
}

bool StemConverter::isRunning() const {
    return m_pProcess != nullptr;
}

QList<StemConverter::CrateWalkItem> StemConverter::walkCrates(
        CrateId crateId, bool includeSubcrates) const {
    const CrateStorage& crates = m_pTrackCollectionManager->internalCollection()->crates();
    QList<CrateWalkItem> items;
    Crate crate;
    if (!crates.readCrateById(crateId, &crate)) {
        return items;
    }
    // Pre-order, so a parent always precedes its children. The storage layer
    // refuses parent cycles, so the recursion terminates.
    std::function<void(CrateId, int, const QStringList&)> visit =
            [&](CrateId id, int parentIndex, const QStringList& path) {
                const int index = static_cast<int>(items.size());
                items.append({id, parentIndex, path});
                if (!includeSubcrates) {
                    return;
                }
                QList<Crate> children;
                CrateSelectResult childCrates = crates.selectChildCrates(id);
                Crate child;
                while (childCrates.populateNext(&child)) {
                    children.append(child);
                }
                for (const Crate& c : std::as_const(children)) {
                    visit(c.getId(), index, path + QStringList{c.getName()});
                }
            };
    visit(crateId, -1, {crate.getName()});
    return items;
}

StemConverter::Plan StemConverter::planCrate(CrateId crateId, bool includeSubcrates) const {
    Plan plan;
    const auto items = walkCrates(crateId, includeSubcrates);
    plan.crates = static_cast<int>(items.size());
    const TrackCollectionManager* pManager = m_pTrackCollectionManager;
    QSet<TrackId> seen;
    for (const auto& item : items) {
        CrateTrackSelectResult tracks(
                pManager->internalCollection()->crates().selectCrateTracksSorted(item.crateId));
        while (tracks.next()) {
            const TrackId id = tracks.trackId();
            if (seen.contains(id)) {
                continue;
            }
            seen.insert(id);
            const TrackPointer pTrack = pManager->getTrackById(id);
            if (pTrack && pTrack->getType().startsWith(QStringLiteral("stem"))) {
                ++plan.alreadyStems;
            } else if (pTrack) {
                ++plan.tracks;
            }
        }
    }
    return plan;
}

bool StemConverter::isInStemsTree(CrateId crateId) const {
    Crate root;
    const CrateStorage& crates = m_pTrackCollectionManager->internalCollection()->crates();
    if (!crates.readCrateByName(QString::fromLatin1(kStemsRootCrateName), &root) ||
            root.getParentId().isValid()) {
        return false;
    }
    return crateId == root.getId() || crates.isAncestorOf(root.getId(), crateId);
}

CrateId StemConverter::stemsRootCrate(bool create) {
    TrackCollection* pCollection = m_pTrackCollectionManager->internalCollection();
    Crate root;
    if (pCollection->crates().readCrateByName(QString::fromLatin1(kStemsRootCrateName), &root) &&
            !root.getParentId().isValid()) {
        return root.getId();
    }
    if (!create) {
        return CrateId();
    }
    Crate crate;
    crate.setName(CrateFeatureHelper(pCollection, m_pConfig)
                    .proposeNameForNewCrate(QString::fromLatin1(kStemsRootCrateName)));
    CrateId id;
    pCollection->insertCrate(crate, &id);
    return id;
}

CrateId StemConverter::mirrorCrate(CrateId sourceCrateId, CrateId mirrorParentId) {
    TrackCollection* pCollection = m_pTrackCollectionManager->internalCollection();
    Crate source;
    if (!pCollection->crates().readCrateById(sourceCrateId, &source)) {
        return CrateId();
    }
    // Crate names are unique across the whole collection, so a mirror cannot
    // reuse its source's name; it carries a suffix instead. An existing
    // mirror under the same parent is reused, so converting again fills the
    // same tree.
    const QString wanted = source.getName() + kMirrorSuffix;
    CrateSelectResult children = pCollection->crates().selectChildCrates(mirrorParentId);
    Crate child;
    while (children.populateNext(&child)) {
        if (child.getName() == wanted ||
                child.getName().startsWith(wanted + QChar(' '))) {
            return child.getId();
        }
    }
    Crate mirror;
    mirror.setName(CrateFeatureHelper(pCollection, m_pConfig).proposeNameForNewCrate(wanted));
    mirror.setParentId(mirrorParentId);
    CrateId id;
    pCollection->insertCrate(mirror, &id);
    return id;
}

int StemConverter::enqueueCrate(CrateId crateId, bool includeSubcrates) {
    if (isInStemsTree(crateId)) {
        emit message(tr("Crates under \"%1\" hold converted tracks already.")
                        .arg(QString::fromLatin1(kStemsRootCrateName)));
        return 0;
    }
    const Settings settings = Settings::load(m_pConfig);
    const auto items = walkCrates(crateId, includeSubcrates);
    if (items.isEmpty()) {
        return 0;
    }

    const CrateId rootId = stemsRootCrate(true);
    QList<CrateId> mirrorIds;
    mirrorIds.reserve(items.size());
    for (const auto& item : items) {
        const CrateId parent = item.parentIndex < 0 ? rootId : mirrorIds.at(item.parentIndex);
        mirrorIds.append(mirrorCrate(item.crateId, parent));
    }

    const TrackCollectionManager* pManager = m_pTrackCollectionManager;
    const auto outputKey = [](const QString& path) {
        return QDir::toNativeSeparators(QDir::cleanPath(path)).toLower();
    };
    // Tracks still waiting in an earlier batch gain the new crates instead
    // of being queued twice. Output paths already handed out stay with their
    // track, so converting again finds the same file and skips it.
    QHash<TrackId, int> entryByTrack;
    QHash<QString, TrackId> outputOwner;
    for (int index = 0; index < m_entries.size(); ++index) {
        const TrackEntry& entry = m_entries.at(index);
        outputOwner.insert(outputKey(entry.outputPath), entry.sourceId);
        if (entry.state == TrackState::Queued || entry.state == TrackState::Running) {
            entryByTrack.insert(entry.sourceId, index);
        }
    }
    const int first = static_cast<int>(m_entries.size());
    for (int i = 0; i < items.size(); ++i) {
        CrateTrackSelectResult tracks(
                pManager->internalCollection()->crates().selectCrateTracksSorted(
                        items.at(i).crateId));
        while (tracks.next()) {
            const TrackId id = tracks.trackId();
            if (const auto it = entryByTrack.constFind(id); it != entryByTrack.constEnd()) {
                // Same track in another subcrate: one conversion, many crates.
                m_entries[*it].targetCrates.append(mirrorIds.at(i));
                continue;
            }
            const TrackPointer pTrack = pManager->getTrackById(id);
            if (!pTrack || pTrack->getType().startsWith(QStringLiteral("stem"))) {
                continue;
            }
            TrackEntry entry;
            entry.sourceId = id;
            entry.sourcePath = pTrack->getLocation();
            const QString artist = pTrack->getArtist();
            const QString title = pTrack->getTitle();
            entry.displayName = artist.isEmpty() || title.isEmpty()
                    ? QFileInfo(entry.sourcePath).fileName()
                    : artist + QStringLiteral(" - ") + title;

            QDir dir(settings.outputRoot);
            for (const QString& segment : items.at(i).pathSegments) {
                dir.setPath(dir.filePath(sanitizeFileName(segment)));
            }
            const QString base = sanitizeFileName(QFileInfo(entry.sourcePath).completeBaseName());
            // Two sources with the same file name in one crate get "name (2)".
            QString output = dir.filePath(base + kStemExtension);
            for (int n = 2; outputOwner.contains(outputKey(output)) &&
                    outputOwner.value(outputKey(output)) != id;
                    ++n) {
                output = dir.filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(kStemExtension));
            }
            outputOwner.insert(outputKey(output), id);
            entry.outputPath = QDir::toNativeSeparators(output);
            entry.targetCrates.append(mirrorIds.at(i));

            entryByTrack.insert(id, static_cast<int>(m_entries.size()));
            m_pending.append(static_cast<int>(m_entries.size()));
            m_entries.append(entry);
        }
    }
    const int added = static_cast<int>(m_entries.size()) - first;
    kLogger.info() << "Queued" << added << "tracks from" << items.size() << "crates";
    if (added > 0) {
        emit entriesAdded(first, added);
        startNextBatch();
    }
    return added;
}

void StemConverter::startNextBatch() {
    if (m_pProcess || m_pending.isEmpty()) {
        return;
    }
    const Settings settings = Settings::load(m_pConfig);
    if (!QFileInfo::exists(settings.executable)) {
        emit message(tr("stemforge was not found at %1.").arg(settings.executable));
        for (int index : std::as_const(m_pending)) {
            setState(index, TrackState::Failed, tr("stemforge not found"));
        }
        m_pending.clear();
        return;
    }
    if (settings.presetPath.isEmpty()) {
        emit message(tr("No stemforge preset found."));
        return;
    }

    const QDir dir(stemforgeDir(m_pConfig));
    dir.mkpath(QStringLiteral("jobs"));
    dir.mkpath(QStringLiteral("logs"));
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));

    QJsonArray tracks;
    m_batch = m_pending;
    m_pending.clear();
    for (int index : std::as_const(m_batch)) {
        const TrackEntry& entry = m_entries.at(index);
        tracks.append(QJsonObject{
                {QStringLiteral("id"), index},
                {QStringLiteral("source"), entry.sourcePath},
                {QStringLiteral("output"), entry.outputPath},
        });
    }
    const QJsonObject job{
            {QStringLiteral("preset"), settings.presetPath},
            {QStringLiteral("skip_unchanged"), settings.skipUnchanged},
            {QStringLiteral("tracks"), tracks},
    };
    const QString jobPath = dir.filePath(QStringLiteral("jobs/job-%1.json").arg(stamp));
    QFile jobFile(jobPath);
    if (!jobFile.open(QIODevice::WriteOnly) ||
            jobFile.write(QJsonDocument(job).toJson()) < 0) {
        emit message(tr("Cannot write the job file %1.").arg(jobPath));
        return;
    }
    jobFile.close();

    m_log.setFileName(dir.filePath(QStringLiteral("logs/job-%1.log").arg(stamp)));
    m_log.open(QIODevice::WriteOnly | QIODevice::Text);

    m_pProcess = new QProcess(this);
#ifdef Q_OS_WIN
    m_pProcess->setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* pArgs) {
                pArgs->flags |= BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW;
            });
#endif
    connect(m_pProcess, &QProcess::readyReadStandardOutput, this, &StemConverter::slotReadStdout);
    connect(m_pProcess, &QProcess::readyReadStandardError, this, &StemConverter::slotReadStderr);
    connect(m_pProcess, &QProcess::finished, this, &StemConverter::slotFinished);
    m_stdoutBuffer.clear();
    m_runningTrack = -1;
    m_autoPaused = false;
    kLogger.info() << "Starting" << settings.executable << "for" << m_batch.size() << "tracks";
    m_pProcess->start(settings.executable, {QStringLiteral("run"), jobPath, QStringLiteral("--control")});
    if (m_userPaused) {
        sendCommand("pause");
    }
    m_deckPollTimer.start();
    emit stateChanged();
}

void StemConverter::slotReadStdout() {
    m_stdoutBuffer += m_pProcess->readAllStandardOutput();
    qsizetype newline;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }
        if (m_log.isOpen()) {
            m_log.write(line + '\n');
        }
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
        if (doc.isObject()) {
            handleEvent(doc.object());
        } else {
            kLogger.warning() << "Unparsable stemforge output:" << line;
        }
    }
}

void StemConverter::slotReadStderr() {
    const QByteArray text = m_pProcess->readAllStandardError();
    if (m_log.isOpen()) {
        m_log.write(text);
        m_log.flush();
    }
}

void StemConverter::handleEvent(const QJsonObject& event) {
    const QString type = event.value(QStringLiteral("ev")).toString();
    const auto entryIndex = [&]() -> int {
        const int index = event.value(QStringLiteral("id")).toInt(-1);
        return index >= 0 && index < m_entries.size() ? index : -1;
    };

    if (type == QStringLiteral("track_start")) {
        m_runningTrack = entryIndex();
        if (m_runningTrack >= 0) {
            setState(m_runningTrack, TrackState::Running, tr("Starting"));
        }
    } else if (type == QStringLiteral("stage")) {
        if (m_runningTrack >= 0) {
            setState(m_runningTrack,
                    TrackState::Running,
                    tr("Separating %1 (%2/%3)")
                            .arg(event.value(QStringLiteral("take"))
                                            .toVariant()
                                            .toStringList()
                                            .join(QStringLiteral(", ")))
                            .arg(event.value(QStringLiteral("stage")).toInt() + 1)
                            .arg(event.value(QStringLiteral("stages")).toInt()));
        }
    } else if (type == QStringLiteral("fallback")) {
        if (m_runningTrack >= 0) {
            setState(m_runningTrack,
                    TrackState::Running,
                    tr("Out of GPU memory, retrying: %1")
                            .arg(event.value(QStringLiteral("to")).toString()));
        }
    } else if (type == QStringLiteral("track_done") || type == QStringLiteral("track_skipped")) {
        const int index = entryIndex();
        if (index >= 0) {
            importConverted(index, event);
        }
        m_runningTrack = -1;
    } else if (type == QStringLiteral("track_error")) {
        const int index = entryIndex();
        if (index >= 0) {
            setState(index, TrackState::Failed, event.value(QStringLiteral("message")).toString());
        }
        m_runningTrack = -1;
    } else if (type == QStringLiteral("paused") || type == QStringLiteral("resumed")) {
        emit stateChanged();
    }
}

void StemConverter::importConverted(int entryIndex, const QJsonObject& event) {
    TrackEntry& entry = m_entries[entryIndex];
    const bool skipped = event.value(QStringLiteral("ev")).toString() ==
            QStringLiteral("track_skipped");
    TrackCollectionManager* pManager = m_pTrackCollectionManager;

    bool alreadyInLibrary = false;
    const TrackPointer pStem = pManager->getOrAddTrack(
            TrackRef::fromFilePath(entry.outputPath), &alreadyInLibrary);
    if (!pStem || !pStem->getId().isValid()) {
        setState(entryIndex, TrackState::Failed, tr("Converted, but could not be added to the library"));
        return;
    }

    QStringList notes;
    const TrackPointer pSource = pManager->getTrackById(entry.sourceId);
    const QJsonObject qa = event.value(QStringLiteral("qa")).toObject();
    if (pSource) {
        const auto offset = measureOffset(pSource, pStem);
        if (offset && *offset != 0) {
            notes << tr("offset %1 frames").arg(*offset);
        } else if (!offset) {
            notes << tr("offset not measured");
        }
        // A skipped track carries no QA, so its headroom gain is unknown.
        std::optional<double> headroomDb;
        if (qa.contains(QStringLiteral("headroom_gain_db"))) {
            headroomDb = qa.value(QStringLiteral("headroom_gain_db")).toDouble();
        }
        const CarryOverResult carried = carryOverMetadata(
                *pSource, pStem.get(), offset.value_or(0), headroomDb);
        if (carried.cues > 0) {
            notes << tr("%n cue(s)", "", carried.cues);
        }
        if (carried.beats) {
            notes << tr("beat grid");
        }
    }

    TrackCollection* pCollection = pManager->internalCollection();
    for (const CrateId& crateId : std::as_const(entry.targetCrates)) {
        pCollection->addCrateTracks(crateId, {pStem->getId()});
    }

    if (qa.contains(QStringLiteral("sum_error_peak_dbfs"))) {
        notes.prepend(tr("stems sum to within %1 dB")
                        .arg(qa.value(QStringLiteral("sum_error_peak_dbfs")).toDouble()));
    }
    setState(entryIndex,
            skipped ? TrackState::Skipped : TrackState::Done,
            notes.join(QStringLiteral(" · ")));
}

void StemConverter::slotFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    slotReadStdout();
    for (int index : std::as_const(m_batch)) {
        const TrackState state = m_entries.at(index).state;
        if (state == TrackState::Queued || state == TrackState::Running) {
            setState(index,
                    TrackState::Failed,
                    exitStatus == QProcess::CrashExit
                            ? tr("stemforge stopped unexpectedly")
                            : tr("stemforge exited (code %1)").arg(exitCode));
        }
    }
    kLogger.info() << "stemforge finished, exit code" << exitCode;
    m_batch.clear();
    m_runningTrack = -1;
    m_log.close();
    m_pProcess->deleteLater();
    m_pProcess = nullptr;
    m_deckPollTimer.stop();
    m_autoPaused = false;
    emit stateChanged();
    startNextBatch();
}

void StemConverter::sendCommand(const QByteArray& command) {
    if (m_pProcess && m_pProcess->state() != QProcess::NotRunning) {
        m_pProcess->write(command + '\n');
    }
}

void StemConverter::pause() {
    if (!m_userPaused) {
        m_userPaused = true;
        if (!m_autoPaused) {
            sendCommand("pause");
        }
        emit stateChanged();
    }
}

void StemConverter::resume() {
    if (m_userPaused) {
        m_userPaused = false;
        if (!m_autoPaused) {
            sendCommand("resume");
        }
        emit stateChanged();
    }
}

void StemConverter::cancel() {
    for (int index : std::as_const(m_pending)) {
        setState(index, TrackState::Cancelled);
    }
    m_pending.clear();
    if (m_pProcess) {
        for (int index : std::as_const(m_batch)) {
            const TrackState state = m_entries.at(index).state;
            if (state == TrackState::Queued || state == TrackState::Running) {
                setState(index, TrackState::Cancelled);
            }
        }
        // The running track would otherwise finish first, which can take
        // minutes; its partial output is written to a temp folder only.
        m_pProcess->kill();
    }
}

void StemConverter::slotPollDecks() {
    if (!Settings::load(m_pConfig).pauseWhilePlaying) {
        if (m_autoPaused) {
            m_autoPaused = false;
            if (!m_userPaused) {
                sendCommand("resume");
            }
            emit stateChanged();
        }
        return;
    }
    if (!m_pNumDecks) {
        m_pNumDecks = std::make_unique<ControlProxy>(
                QStringLiteral("[App]"), QStringLiteral("num_decks"), this);
    }
    const int numDecks = static_cast<int>(m_pNumDecks->get());
    while (static_cast<int>(m_playControls.size()) < numDecks) {
        m_playControls.push_back(std::make_unique<ControlProxy>(
                PlayerManager::groupForDeck(static_cast<int>(m_playControls.size())),
                QStringLiteral("play"),
                this));
    }
    bool playing = false;
    for (int i = 0; i < numDecks; ++i) {
        playing = playing || m_playControls[i]->toBool();
    }
    if (playing && !m_autoPaused) {
        m_autoPaused = true;
        if (!m_userPaused) {
            sendCommand("pause");
        }
        emit message(tr("Stem conversion pauses after the current track while a deck is playing."));
        emit stateChanged();
    } else if (!playing && m_autoPaused) {
        m_autoPaused = false;
        if (!m_userPaused) {
            sendCommand("resume");
        }
        emit stateChanged();
    }
}

void StemConverter::setState(int entryIndex, TrackState state, const QString& detail) {
    TrackEntry& entry = m_entries[entryIndex];
    entry.state = state;
    entry.detail = detail.isEmpty() ? stateLabel(state) : detail;
    emit entryChanged(entryIndex);
}

QString trackStateLabel(TrackState state) {
    return stateLabel(state);
}

} // namespace mixxx::stemconverter
