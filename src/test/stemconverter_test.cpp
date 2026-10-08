#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <algorithm>

#include "library/stemconverter/stemconverter.h"
#include "library/trackset/crate/crate.h"
#include "test/librarytest.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"

using namespace mixxx;
using namespace mixxx::stemconverter;

// End-to-end: convert a nested crate with the real stemforge and check the
// mirrored "Stems" tree, the files, and the metadata carried across.
//
// Needs a working stemforge, so it is skipped unless STEMFORGE_EXE names the
// executable, e.g.
//   set STEMFORGE_EXE=C:\...\stemforge\.venv\Scripts\stemforge.exe
//   mixxx-test --gtest_filter=StemConverterTest.*
// Uses the "fast" preset on a 12.8 s clip; about half a minute on a GPU.

namespace {

constexpr int kTimeoutMs = 10 * 60 * 1000;

QString stemforgeExe() {
    return QProcessEnvironment::systemEnvironment().value(QStringLiteral("STEMFORGE_EXE"));
}

class StemConverterTest : public LibraryTest {
  protected:
    void SetUp() override {
        if (stemforgeExe().isEmpty()) {
            GTEST_SKIP() << "STEMFORGE_EXE not set";
        }
        ASSERT_TRUE(m_tempDir.isValid());
    }

    QString copyFixture(const QString& name) {
        const QString target = QDir(m_tempDir.path()).filePath(QStringLiteral("source/") + name);
        QDir().mkpath(QFileInfo(target).absolutePath());
        EXPECT_TRUE(QFile::copy(
                getTestDir().filePath(QStringLiteral("stems/stem02/trance_mainmix.wav")), target));
        return target;
    }

    CrateId makeCrate(const QString& name, CrateId parent, const TrackPointer& pTrack) {
        Crate crate;
        crate.setName(name);
        crate.setParentId(parent);
        CrateId id;
        EXPECT_TRUE(internalCollection()->insertCrate(crate, &id));
        EXPECT_TRUE(internalCollection()->addCrateTracks(id, {pTrack->getId()}));
        return id;
    }

    CrateId childNamed(CrateId parent, const QString& name) const {
        CrateSelectResult children = internalCollection()->crates().selectChildCrates(parent);
        Crate child;
        while (children.populateNext(&child)) {
            if (child.getName() == name) {
                return child.getId();
            }
        }
        return CrateId();
    }

    QList<TrackId> tracksOf(CrateId crateId) const {
        QList<TrackId> ids;
        CrateTrackSelectResult tracks(internalCollection()->crates().selectCrateTracksSorted(crateId));
        while (tracks.next()) {
            ids.append(tracks.trackId());
        }
        return ids;
    }

    void runToCompletion(StemConverter* pConverter) {
        QElapsedTimer timer;
        timer.start();
        while (pConverter->isRunning() && timer.elapsed() < kTimeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        }
        ASSERT_FALSE(pConverter->isRunning()) << "stemforge did not finish in time";
    }

    void configure() {
        Settings settings = Settings::load(config());
        settings.executable = stemforgeExe();
        settings.outputRoot = QDir(m_tempDir.path()).filePath(QStringLiteral("Stems"));
        for (const QString& preset : presetFiles(settings.executable, config())) {
            if (QFileInfo(preset).baseName() == QStringLiteral("fast")) {
                settings.presetPath = preset;
            }
        }
        ASSERT_FALSE(settings.presetPath.isEmpty()) << "fast preset not found";
        settings.pauseWhilePlaying = false;
        settings.save(config());
    }

    QTemporaryDir m_tempDir;
};

TEST_F(StemConverterTest, ConvertsNestedCrateIntoMirroredStemsTree) {
    configure();

    const TrackPointer pA = getOrAddTrackByLocation(copyFixture(QStringLiteral("a.wav")));
    const TrackPointer pB = getOrAddTrackByLocation(copyFixture(QStringLiteral("b.wav")));
    ASSERT_TRUE(pA && pB);
    pA->setKeyText(QStringLiteral("Am"));
    pA->setRating(4);
    pA->createAndAddCue(CueType::HotCue,
            2,
            audio::FramePos(44100),
            audio::FramePos(),
            RgbColor(0xff0000));
    const bool withBeats = pA->getSampleRate().isValid();
    if (withBeats) {
        ASSERT_TRUE(pA->trySetBeats(Beats::fromConstTempo(
                pA->getSampleRate(), audio::FramePos(100), Bpm(128))));
    }

    const CrateId house = makeCrate(QStringLiteral("House"), CrateId(), pA);
    makeCrate(QStringLiteral("4am"), house, pB);

    StemConverter converter(trackCollectionManager(), config());
    const auto plan = converter.planCrate(house, true);
    EXPECT_EQ(plan.tracks, 2);
    EXPECT_EQ(plan.crates, 2);

    // Progress must move through the middle of a track, never backwards.
    QList<double> progressSeen;
    QObject::connect(&converter, &StemConverter::entryChanged, [&](int index) {
        const TrackEntry& entry = converter.entries().at(index);
        if (index == 0 && entry.state == TrackState::Running) {
            progressSeen.append(entry.progress);
        }
    });
    bool etaSeen = false;
    QObject::connect(&converter, &StemConverter::entryChanged, [&](int) {
        etaSeen = etaSeen || converter.progress().etaSeconds.has_value();
    });

    ASSERT_EQ(converter.enqueueCrate(house, true), 2);
    runToCompletion(&converter);
    for (const TrackEntry& entry : converter.entries()) {
        EXPECT_EQ(entry.state, TrackState::Done) << entry.displayName.toStdString() << ": "
                                                 << entry.detail.toStdString();
        EXPECT_TRUE(QFileInfo::exists(entry.outputPath)) << entry.outputPath.toStdString();
    }
    ASSERT_FALSE(progressSeen.isEmpty());
    EXPECT_TRUE(std::is_sorted(progressSeen.begin(), progressSeen.end()));
    EXPECT_TRUE(std::any_of(progressSeen.begin(), progressSeen.end(), [](double p) {
        return p > 0.05 && p < 0.95;
    }));
    EXPECT_TRUE(etaSeen);
    const auto overall = converter.progress();
    EXPECT_EQ(overall.finished, 2);
    EXPECT_DOUBLE_EQ(overall.fraction, 1.0);
    EXPECT_FALSE(overall.etaSeconds.has_value());

    // Stems ▸ House (Stems) ▸ 4am (Stems), one converted track in each.
    Crate root;
    ASSERT_TRUE(internalCollection()->crates().readCrateByName(QStringLiteral("Stems"), &root));
    EXPECT_FALSE(root.getParentId().isValid());
    const CrateId houseMirror = childNamed(root.getId(), QStringLiteral("House (Stems)"));
    ASSERT_TRUE(houseMirror.isValid());
    const CrateId fourAmMirror = childNamed(houseMirror, QStringLiteral("4am (Stems)"));
    ASSERT_TRUE(fourAmMirror.isValid());
    ASSERT_EQ(tracksOf(houseMirror).size(), 1);
    ASSERT_EQ(tracksOf(fourAmMirror).size(), 1);
    EXPECT_TRUE(converter.isInStemsTree(fourAmMirror));
    EXPECT_FALSE(converter.isInStemsTree(house));

    // Folders mirror the crates too.
    EXPECT_TRUE(converter.entries().at(1).outputPath.contains(
            QDir::toNativeSeparators(QStringLiteral("Stems/House/4am/b.stem.mp4"))));

    // What was prepared on the source came across. The source is a WAV, so
    // there is no decoder delay and the offset must be zero.
    const TrackPointer pStem = trackCollectionManager()->getTrackById(tracksOf(houseMirror).first());
    ASSERT_TRUE(pStem);
    EXPECT_EQ(pStem->getKeyText(), QStringLiteral("Am"));
    EXPECT_EQ(pStem->getRating(), 4);
    const CuePointer pHotcue = pStem->findHotcueByIndex(2);
    ASSERT_TRUE(pHotcue);
    EXPECT_EQ(pHotcue->getPosition(), audio::FramePos(44100));
    if (withBeats) {
        ASSERT_TRUE(pStem->getBeats());
        EXPECT_DOUBLE_EQ(pStem->getBpm(), 128.0);
    }

    // Converting again reuses the files and the mirrored crates.
    ASSERT_EQ(converter.enqueueCrate(house, true), 2);
    runToCompletion(&converter);
    for (int i = 2; i < 4; ++i) {
        EXPECT_EQ(converter.entries().at(i).state, TrackState::Skipped)
                << converter.entries().at(i).detail.toStdString();
    }
    EXPECT_FALSE(childNamed(root.getId(), QStringLiteral("House (Stems) 2")).isValid());
    EXPECT_EQ(tracksOf(houseMirror).size(), 1);
}

TEST_F(StemConverterTest, ConvertsTracksIntoTheirCratesMirrors) {
    configure();

    const TrackPointer pNested = getOrAddTrackByLocation(copyFixture(QStringLiteral("n.wav")));
    const TrackPointer pLoose = getOrAddTrackByLocation(copyFixture(QStringLiteral("l.wav")));
    const TrackPointer pSub = getOrAddTrackByLocation(copyFixture(QStringLiteral("s.wav")));
    ASSERT_TRUE(pNested && pLoose && pSub);
    // House > 4am holds pNested; Deep (top level) holds pSub, and so does
    // House > 4am > Late, which is deeper and therefore names its folder.
    const CrateId house = makeCrate(QStringLiteral("House"), CrateId(), pSub);
    const CrateId fourAm = makeCrate(QStringLiteral("4am"), house, pNested);
    const CrateId late = makeCrate(QStringLiteral("Late"), fourAm, pSub);
    const CrateId deep = makeCrate(QStringLiteral("Deep"), CrateId(), pSub);
    ASSERT_TRUE(internalCollection()->removeCrateTracks(house, {pSub->getId()}));

    StemConverter converter(trackCollectionManager(), config());
    const auto plan = converter.planTracks({pNested->getId(), pLoose->getId(), pSub->getId()});
    EXPECT_EQ(plan.tracks, 3);
    EXPECT_EQ(plan.withoutCrate, 1);

    ASSERT_EQ(converter.enqueueTracks({pNested->getId(), pLoose->getId(), pSub->getId()}), 3);
    runToCompletion(&converter);
    for (const TrackEntry& entry : converter.entries()) {
        EXPECT_EQ(entry.state, TrackState::Done) << entry.detail.toStdString();
    }

    Crate root;
    ASSERT_TRUE(internalCollection()->crates().readCrateByName(QStringLiteral("Stems"), &root));
    // The nested track brings its crate's parent along: Stems > House > 4am.
    const CrateId houseMirror = childNamed(root.getId(), QStringLiteral("House (Stems)"));
    const CrateId fourAmMirror = childNamed(houseMirror, QStringLiteral("4am (Stems)"));
    const CrateId lateMirror = childNamed(fourAmMirror, QStringLiteral("Late (Stems)"));
    const CrateId deepMirror = childNamed(root.getId(), QStringLiteral("Deep (Stems)"));
    ASSERT_TRUE(fourAmMirror.isValid() && lateMirror.isValid() && deepMirror.isValid());
    EXPECT_TRUE(tracksOf(houseMirror).isEmpty());
    EXPECT_EQ(tracksOf(fourAmMirror).size(), 1);
    // pSub is in both of its crates' mirrors, from one conversion.
    EXPECT_EQ(tracksOf(lateMirror), tracksOf(deepMirror));
    // The loose track sits in the Stems crate itself.
    EXPECT_EQ(tracksOf(root.getId()).size(), 1);

    const auto& entries = converter.entries();
    EXPECT_TRUE(entries.at(0).outputPath.endsWith(
            QDir::toNativeSeparators(QStringLiteral("Stems/House/4am/n.stem.mp4"))));
    EXPECT_TRUE(entries.at(1).outputPath.endsWith(
            QDir::toNativeSeparators(QStringLiteral("Stems/l.stem.mp4"))));
    EXPECT_TRUE(entries.at(2).outputPath.endsWith(
            QDir::toNativeSeparators(QStringLiteral("Stems/House/4am/Late/s.stem.mp4"))));

    // Converting the subcrate on its own reuses the same mirrors and files.
    ASSERT_EQ(converter.enqueueCrate(fourAm, true), 2);
    runToCompletion(&converter);
    for (int i = 3; i < 5; ++i) {
        EXPECT_EQ(entries.at(i).state, TrackState::Skipped) << entries.at(i).detail.toStdString();
    }
    EXPECT_FALSE(childNamed(root.getId(), QStringLiteral("4am (Stems)")).isValid());
    EXPECT_EQ(tracksOf(fourAmMirror).size(), 1);
}

} // namespace
