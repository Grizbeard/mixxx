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
            // Benchmarked on trance/techno: as good as the RoFormer presets
            // there, at a ninth of the time (docs/02-stem-converter.md).
            if (QFileInfo(path).baseName() == QStringLiteral("electronic")) {
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

QList<CrateId> StemConverter::walkCrates(CrateId crateId, bool includeSubcrates) const {
    const CrateStorage& crates = m_pTrackCollectionManager->internalCollection()->crates();
    QList<CrateId> ids;
    if (!crates.readCrateById(crateId)) {
        return ids;
    }
    // Pre-order, so a parent always precedes its children. The storage layer
    // refuses parent cycles, so the recursion terminates.
    std::function<void(CrateId)> visit = [&](CrateId id) {
        ids.append(id);
        if (!includeSubcrates) {
            return;
        }
        QList<CrateId> children;
        CrateSelectResult childCrates = crates.selectChildCrates(id);
        Crate child;
        while (childCrates.populateNext(&child)) {
            children.append(child.getId());
        }
        for (const CrateId& childId : std::as_const(children)) {
            visit(childId);
        }
    };
    visit(crateId);
    return ids;
}

QList<CrateId> StemConverter::ancestry(CrateId crateId) const {
    const CrateStorage& crates = m_pTrackCollectionManager->internalCollection()->crates();
    QList<CrateId> chain;
    Crate crate;
    // The depth limit only guards against a cycle that slipped past the
    // storage layer's checks.
    for (CrateId id = crateId; id.isValid() && chain.size() < 64 &&
            crates.readCrateById(id, &crate);
            id = crate.getParentId()) {
        chain.prepend(id);
    }
    return chain;
}

QStringList StemConverter::crateNamePath(CrateId crateId) const {
    const CrateStorage& crates = m_pTrackCollectionManager->internalCollection()->crates();
    QStringList names;
    Crate crate;
    for (const CrateId& id : ancestry(crateId)) {
        if (crates.readCrateById(id, &crate)) {
            names.append(crate.getName());
        }
    }
    return names;
}

QList<CrateId> StemConverter::sourceCratesOf(TrackId trackId) const {
    QList<CrateId> ids;
    CrateTrackSelectResult crates(
            m_pTrackCollectionManager->internalCollection()->crates().selectTrackCratesSorted(
                    trackId));
    while (crates.next()) {
        if (!isInStemsTree(crates.crateId())) {
            ids.append(crates.crateId());
        }
    }
    return ids;
}

CrateId StemConverter::folderCrateOf(TrackId trackId) const {
    // The deepest crate holding the track is the most specific place for its
    // file; ties go to the alphabetically first path. This depends only on
    // the track, so converting it alone or as part of any crate writes the
    // same file, and the next conversion finds it up to date.
    CrateId best;
    int bestDepth = -1;
    QString bestPath;
    for (const CrateId& id : sourceCratesOf(trackId)) {
        const QStringList path = crateNamePath(id);
        const QString joined = path.join(QChar('/')).toLower();
        const int depth = static_cast<int>(path.size());
        if (depth > bestDepth || (depth == bestDepth && joined < bestPath)) {
            best = id;
            bestDepth = depth;
            bestPath = joined;
        }
    }
    return best;
}

QList<StemConverter::Request> StemConverter::requestsForCrate(
        CrateId crateId, bool includeSubcrates) const {
    QList<Request> requests;
    QHash<TrackId, int> byTrack;
    const CrateStorage& crates = m_pTrackCollectionManager->internalCollection()->crates();
    for (const CrateId& id : walkCrates(crateId, includeSubcrates)) {
        CrateTrackSelectResult tracks(crates.selectCrateTracksSorted(id));
        while (tracks.next()) {
            const TrackId trackId = tracks.trackId();
            if (const auto it = byTrack.constFind(trackId); it != byTrack.constEnd()) {
                requests[*it].crates.append(id);
            } else {
                byTrack.insert(trackId, static_cast<int>(requests.size()));
                requests.append({trackId, {id}});
            }
        }
    }
    return requests;
}

QList<StemConverter::Request> StemConverter::requestsForTracks(
        const QList<TrackId>& trackIds) const {
    QList<Request> requests;
    QSet<TrackId> seen;
    for (const TrackId& id : trackIds) {
        if (id.isValid() && !seen.contains(id)) {
            seen.insert(id);
            requests.append({id, sourceCratesOf(id)});
        }
    }
    return requests;
}

StemConverter::Plan StemConverter::plan(const QList<Request>& requests) const {
    Plan result;
    QSet<CrateId> crates;
    for (const Request& request : requests) {
        const TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(request.trackId);
        if (!pTrack) {
            continue;
        }
        if (pTrack->getType().startsWith(QStringLiteral("stem"))) {
            ++result.alreadyStems;
            continue;
        }
        ++result.tracks;
        for (const CrateId& id : request.crates) {
            crates.insert(id);
        }
        if (request.crates.isEmpty()) {
            ++result.withoutCrate;
        }
    }
    result.crates = static_cast<int>(crates.size());
    return result;
}

StemConverter::Plan StemConverter::planCrate(CrateId crateId, bool includeSubcrates) const {
    Plan result = plan(requestsForCrate(crateId, includeSubcrates));
    // Count the crates walked, including empty ones that only hold subcrates.
    result.crates = static_cast<int>(walkCrates(crateId, includeSubcrates).size());
    return result;
}

StemConverter::Plan StemConverter::planTracks(const QList<TrackId>& trackIds) const {
    return plan(requestsForTracks(trackIds));
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

CrateId StemConverter::mirrorPath(
        CrateId crateId, CrateId rootId, QHash<CrateId, CrateId>* pCache) {
    // Mirror the whole chain from the top level down, so a subcrate lands
    // under its parent's mirror however it was reached.
    CrateId parent = rootId;
    for (const CrateId& id : ancestry(crateId)) {
        if (const auto it = pCache->constFind(id); it != pCache->constEnd()) {
            parent = *it;
            continue;
        }
        parent = mirrorCrate(id, parent);
        pCache->insert(id, parent);
    }
    return parent;
}

int StemConverter::enqueueCrate(CrateId crateId, bool includeSubcrates) {
    if (isInStemsTree(crateId)) {
        emit message(tr("Crates under \"%1\" hold converted tracks already.")
                        .arg(QString::fromLatin1(kStemsRootCrateName)));
        return 0;
    }
    // Empty subcrates are mirrored too, so the tree matches the source.
    const CrateId rootId = stemsRootCrate(true);
    QHash<CrateId, CrateId> mirrors;
    for (const CrateId& id : walkCrates(crateId, includeSubcrates)) {
        mirrorPath(id, rootId, &mirrors);
    }
    return enqueue(requestsForCrate(crateId, includeSubcrates), &mirrors);
}

int StemConverter::enqueueTracks(const QList<TrackId>& trackIds) {
    QHash<CrateId, CrateId> mirrors;
    return enqueue(requestsForTracks(trackIds), &mirrors);
}

int StemConverter::enqueue(const QList<Request>& requests, QHash<CrateId, CrateId>* pMirrors) {
    if (requests.isEmpty()) {
        return 0;
    }
    const Settings settings = Settings::load(m_pConfig);
    const CrateId rootId = stemsRootCrate(true);
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
    for (const Request& request : requests) {
        // Tracks in no crate go straight into the "Stems" crate.
        QList<CrateId> targets;
        for (const CrateId& id : request.crates) {
            targets.append(mirrorPath(id, rootId, pMirrors));
        }
        if (targets.isEmpty()) {
            targets.append(rootId);
        }

        if (const auto it = entryByTrack.constFind(request.trackId);
                it != entryByTrack.constEnd()) {
            for (const CrateId& target : std::as_const(targets)) {
                if (!m_entries[*it].targetCrates.contains(target)) {
                    m_entries[*it].targetCrates.append(target);
                }
            }
            continue;
        }
        const TrackPointer pTrack = pManager->getTrackById(request.trackId);
        if (!pTrack || pTrack->getType().startsWith(QStringLiteral("stem"))) {
            continue;
        }
        TrackEntry entry;
        entry.sourceId = request.trackId;
        entry.sourcePath = pTrack->getLocation();
        entry.durationSeconds = pTrack->getDuration();
        const QString artist = pTrack->getArtist();
        const QString title = pTrack->getTitle();
        entry.displayName = artist.isEmpty() || title.isEmpty()
                ? QFileInfo(entry.sourcePath).fileName()
                : artist + QStringLiteral(" - ") + title;

        // Folders mirror the crate path of the track's most specific crate.
        QDir dir(settings.outputRoot);
        for (const QString& segment : crateNamePath(folderCrateOf(request.trackId))) {
            dir.setPath(dir.filePath(sanitizeFileName(segment)));
        }
        const QString base = sanitizeFileName(QFileInfo(entry.sourcePath).completeBaseName());
        // Two sources with the same file name in one folder get "name (2)".
        QString output = dir.filePath(base + kStemExtension);
        for (int n = 2; outputOwner.contains(outputKey(output)) &&
                outputOwner.value(outputKey(output)) != request.trackId;
                ++n) {
            output = dir.filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(kStemExtension));
        }
        outputOwner.insert(outputKey(output), request.trackId);
        entry.outputPath = QDir::toNativeSeparators(output);
        entry.targetCrates = targets;

        entryByTrack.insert(request.trackId, static_cast<int>(m_entries.size()));
        m_pending.append(static_cast<int>(m_entries.size()));
        m_entries.append(entry);
    }
    const int added = static_cast<int>(m_entries.size()) - first;
    kLogger.info() << "Queued" << added << "of" << requests.size() << "tracks";
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
    if (m_runStartedMs == 0) {
        m_runStartedMs = QDateTime::currentMSecsSinceEpoch();
        m_runFirstEntry = m_batch.first();
    }
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
    // Events name the track by its position in the job ("track"); the job's
    // "id" is our entry index. Map through the batch, since progress events
    // carry only the position. With one track finalising while the next
    // separates, events for two tracks interleave.
    const auto entryIndex = [&]() -> int {
        const int position = event.value(QStringLiteral("track")).toInt(-1);
        if (position >= 0 && position < m_batch.size()) {
            return m_batch.at(position);
        }
        const int index = event.value(QStringLiteral("id")).toInt(-1);
        return index >= 0 && index < m_entries.size() ? index : -1;
    };
    const int index = entryIndex();

    if (type == QStringLiteral("progress")) {
        if (index >= 0 && m_entries.at(index).state == TrackState::Running) {
            TrackEntry& entry = m_entries[index];
            entry.progress = event.value(QStringLiteral("fraction")).toDouble();
            entry.etaSeconds = event.value(QStringLiteral("eta_s")).toDouble(-1);
            emit entryChanged(index);
        }
    } else if (type == QStringLiteral("track_start")) {
        if (index >= 0) {
            m_entries[index].progress = 0;
            m_entries[index].startedMs = QDateTime::currentMSecsSinceEpoch();
            setState(index, TrackState::Running, tr("Starting"));
        }
    } else if (type == QStringLiteral("stage")) {
        if (index >= 0) {
            setState(index,
                    TrackState::Running,
                    tr("Separating %1 (%2/%3)")
                            .arg(event.value(QStringLiteral("take"))
                                            .toVariant()
                                            .toStringList()
                                            .join(QStringLiteral(", ")))
                            .arg(event.value(QStringLiteral("stage")).toInt() + 1)
                            .arg(event.value(QStringLiteral("stages")).toInt()));
        }
    } else if (type == QStringLiteral("finalize")) {
        if (index >= 0) {
            setState(index, TrackState::Running, tr("Writing the stem file"));
        }
    } else if (type == QStringLiteral("copy_retry")) {
        if (index >= 0) {
            setState(index,
                    TrackState::Running,
                    tr("Output drive not responding, retrying (%1)")
                            .arg(event.value(QStringLiteral("attempt")).toInt()));
        }
    } else if (type == QStringLiteral("fallback")) {
        if (index >= 0) {
            setState(index,
                    TrackState::Running,
                    tr("Out of GPU memory, retrying: %1")
                            .arg(event.value(QStringLiteral("to")).toString()));
        }
    } else if (type == QStringLiteral("track_done") || type == QStringLiteral("track_skipped")) {
        if (index >= 0) {
            m_entries[index].progress = 1;
            m_entries[index].seconds = event.value(QStringLiteral("seconds")).toDouble();
            importConverted(index, event);
        }
    } else if (type == QStringLiteral("track_error")) {
        if (index >= 0) {
            setState(index, TrackState::Failed, event.value(QStringLiteral("message")).toString());
        }
    } else if (type == QStringLiteral("paused") || type == QStringLiteral("resumed")) {
        emit stateChanged();
    }
}

StemConverter::Progress StemConverter::progress() const {
    Progress result;
    double totalAudio = 0;
    double doneAudio = 0;
    double remainingAudio = 0;
    // Wall-clock throughput of the converted (not skipped) tracks so far.
    double workedAudio = 0;
    double knownDuration = 0;
    int knownCount = 0;
    for (const TrackEntry& entry : m_entries) {
        if (entry.durationSeconds > 0) {
            knownDuration += entry.durationSeconds;
            ++knownCount;
        }
    }
    const double fallbackDuration = knownCount > 0 ? knownDuration / knownCount : 300.0;
    for (int i = 0; i < m_entries.size(); ++i) {
        const TrackEntry& entry = m_entries.at(i);
        const bool thisRun = i >= m_runFirstEntry;
        const double duration = entry.durationSeconds > 0 ? entry.durationSeconds : fallbackDuration;
        double completion = 0;
        switch (entry.state) {
        case TrackState::Queued:
            break;
        case TrackState::Running:
            completion = entry.progress;
            workedAudio += thisRun ? completion * duration : 0;
            break;
        case TrackState::Done:
            completion = 1;
            workedAudio += thisRun ? duration : 0;
            ++result.finished;
            break;
        case TrackState::Skipped:
            completion = 1;
            ++result.finished;
            break;
        case TrackState::Failed:
        case TrackState::Cancelled:
            completion = 1;
            ++result.notConverted;
            break;
        }
        totalAudio += duration;
        doneAudio += completion * duration;
        if (entry.state == TrackState::Queued || entry.state == TrackState::Running) {
            remainingAudio += (1 - completion) * duration;
        }
    }
    result.total = static_cast<int>(m_entries.size());
    result.fraction = totalAudio > 0 ? doneAudio / totalAudio : 0;

    // Estimate from what this run has actually achieved, once there is
    // enough of it; before that, from stemforge's per-track estimate.
    if (m_runStartedMs > 0 && remainingAudio > 0) {
        const double elapsed = (QDateTime::currentMSecsSinceEpoch() - m_runStartedMs) / 1000.0;
        if (workedAudio >= 0.25 * fallbackDuration && elapsed > 0) {
            result.etaSeconds = remainingAudio * elapsed / workedAudio;
        } else {
            // The running track's own estimate, and the same pace for the
            // tracks after it.
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            for (const TrackEntry& entry : m_entries) {
                if (entry.state != TrackState::Running || entry.etaSeconds < 0 ||
                        entry.startedMs <= 0) {
                    continue;
                }
                const double duration = entry.durationSeconds > 0
                        ? entry.durationSeconds
                        : fallbackDuration;
                const double trackSeconds = (now - entry.startedMs) / 1000.0 + entry.etaSeconds;
                const double after = remainingAudio - (1 - entry.progress) * duration;
                result.etaSeconds = entry.etaSeconds + std::max(0.0, after) * trackSeconds / duration;
                break;
            }
        }
    }
    return result;
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
    m_log.close();
    m_pProcess->deleteLater();
    m_pProcess = nullptr;
    m_deckPollTimer.stop();
    m_autoPaused = false;
    if (m_pending.isEmpty()) {
        m_runStartedMs = 0;
    }
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
