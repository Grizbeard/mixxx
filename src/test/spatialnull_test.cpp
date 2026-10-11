#include <gtest/gtest.h>

#include <QTest>
#include <QtDebug>
#include <cmath>
#include <functional>
#include <numeric>

#include "control/controlobject.h"
#include "effects/backends/builtin/bessel8lvmixeqeffect.h"
#include "effects/backends/effectsbackendmanager.h"
#include "effects/chains/equalizereffectchain.h"
#include "effects/effectslot.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "engine/spatial/enginespatialoutput.h"
#include "test/signalpathtest.h"

#ifdef __STEM__
namespace {

/// Mixer equivalence: with every stem in its default place (the original
/// stereo image on the front pair), the spatial output's speakers 1 and 2
/// must equal the stereo main, sample for sample, through pregain, stem
/// volumes, channel fader, crossfader and master volume, including their
/// ramps. The spatial path processes each stem separately, so this checks
/// the per-stem gain staging against the stereo sum.
class SpatialNullTest : public BaseSignalPathTest {
  protected:
    void SetUp() override {
        BaseSignalPathTest::SetUp();
        const ChannelHandleAndGroup deck = m_pEngineMixer->registerChannelGroup(m_sGroup1);
        // The deck's EQ and QuickEffect chains, as PlayerManager sets up.
        m_pEffectsManager->addDeck(deck);
        for (int i = 1; i <= 4; i++) {
            ChannelHandleAndGroup stem = m_pEngineMixer->registerChannelGroup(
                    QStringLiteral("[Channel1_Stem%1]").arg(i));
            m_pChannel1->addStemHandle(stem);
            m_pEffectsManager->addStem(stem);
        }
        for (int i = 1; i <= EngineDeck::kSpatialStems; i++) {
            ChannelHandleAndGroup spatial = m_pEngineMixer->registerChannelGroup(
                    QStringLiteral("[Channel1_Spatial%1]").arg(i));
            m_pChannel1->addSpatialHandle(spatial);
            m_pEffectsManager->addSpatialInput(deck, spatial);
        }

        m_pOutput = m_pEngineMixer->getSpatialOutput();
        m_pOutput->onOutputConnected(m_out);
        ControlObject::set(ConfigKey("[Spatial]", "speaker_count"), 6);
        ControlObject::set(ConfigKey("[Spatial]", "lf_enabled"), 0);
        ControlObject::set(ConfigKey("[Spatial]", "limiter_enabled"), 0);
        ControlObject::set(ConfigKey("[SpatialDelay]", "enabled"), 0);
        ControlObject::set(ConfigKey("[SpatialReverb]", "enabled"), 0);

        TrackPointer pTrack(Track::newTemporary(
                getTestDir().filePath("stems/stem01/sin_ALAC_24bit.stem.mp4")));
        loadTrackAndWait(pTrack);
    }

    void loadTrackAndWait(TrackPointer pTrack) {
        TrackPointer pLoaded;
        QMetaObject::Connection connection = QObject::connect(m_pMixerDeck1.get(),
                &BaseTrackPlayerImpl::newTrackLoaded,
                [&pLoaded](TrackPointer pNew) { pLoaded = pNew; }); // clazy:exclude=lambda-in-connect
        loadTrack(m_pMixerDeck1.get(), pTrack);
        for (int i = 0; i < 10000 && pLoaded != pTrack; ++i) {
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 1);
        }
        QObject::disconnect(connection);
        ASSERT_EQ(pLoaded, pTrack);
    }

    EngineSpatialOutput* m_pOutput = nullptr;
    const AudioOutput m_out{AudioPath::AudioPathType::Spatial, 0, mixxx::audio::ChannelCount(6)};
};

struct NullResult {
    double peak = 0;       // of the stereo main
    double worst = 0;      // largest |front pair - main|
    std::size_t worstAt = 0;
    double others = 0;     // largest sample on speakers 3-6
    double tail = 0;       // front pair peak over the last 5 buffers
    std::vector<double> mainRms; // per buffer
};

/// Plays deck 1 for `buffers` callbacks, calling `change(b)` before each,
/// and compares the spatial front pair with the stereo main.
class SpatialNullRunner : public SpatialNullTest {
  protected:
    NullResult run(int buffers, const std::function<void(int)>& change) {
        for (int i = 1; i <= 4; i++) {
            const QString stem = QStringLiteral("[Channel1_Stem%1]").arg(i);
            ControlObject::set(ConfigKey(stem, "volume"), 1.0);
            ControlObject::set(
                    ConfigKey(QStringLiteral("[QuickEffectRack1_%1]").arg(stem), "enabled"), 0);
        }
        m_pChannel1->getEngineBuffer()->queueNewPlaypos(
                mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
        // The reader thread fills the deck's cache asynchronously: let it,
        // and pace the run, so the deck never plays a cache miss (silence).
        for (int i = 0; i < 20; ++i) {
            ProcessBuffer();
            QTest::qSleep(10);
        }
        ControlObject::set(ConfigKey(m_sGroup1, "play"), 1);

        const double rate = ControlObject::get(ConfigKey("[App]", "samplerate"));
        const int lookahead = static_cast<int>(std::lround(0.0015 * rate));
        const int frames = kProcessBufferSize / 2;
        std::vector<double> main[2];
        std::vector<double> front[2];
        NullResult result;
        for (int b = 0; b < buffers; ++b) {
            change(b);
            ProcessBuffer();
            QTest::qSleep(2);
            const auto mainBuffer = m_pEngineMixer->getMainBuffer();
            const auto spatial = m_pOutput->buffer(m_out);
            double sum = 0;
            for (int f = 0; f < frames; ++f) {
                sum += mainBuffer[2 * f] * mainBuffer[2 * f];
            }
            result.mainRms.push_back(std::sqrt(sum / frames));
            for (int f = 0; f < frames; ++f) {
                main[0].push_back(mainBuffer[2 * f]);
                main[1].push_back(mainBuffer[2 * f + 1]);
                front[0].push_back(spatial[f * 6]);
                front[1].push_back(spatial[f * 6 + 1]);
                for (int c = 2; c < 6; ++c) {
                    result.others = std::max(result.others, std::abs(static_cast<double>(spatial[f * 6 + c])));
                }
                if (b >= buffers - 5) {
                    result.tail = std::max(result.tail, std::abs(static_cast<double>(spatial[f * 6])));
                }
            }
        }
        for (std::size_t n = 0; n + lookahead < front[0].size(); ++n) {
            result.peak = std::max(result.peak, std::abs(main[0][n]));
            for (int side = 0; side < 2; ++side) {
                const double d = std::abs(front[side][n + lookahead] - main[side][n]);
                if (d > result.worst) {
                    result.worst = d;
                    result.worstAt = n / frames;
                }
            }
        }
        return result;
    }
};

TEST_F(SpatialNullRunner, FrontPairNullsAgainstTheStereoMain) {
    ASSERT_EQ(ControlObject::get(ConfigKey(m_sGroup1, "stem_count")), 4);
    const NullResult result = run(140, [this](int b) {
        switch (b) {
        case 20:
            ControlObject::set(ConfigKey(m_sGroup1, "volume"), 0.5);
            break;
        case 40:
            ControlObject::set(ConfigKey("[Master]", "crossfader"), -0.6);
            ControlObject::set(ConfigKey(m_sGroup1, "orientation"), 0); // left
            break;
        case 60:
            ControlObject::set(ConfigKey(m_sGroup1, "pregain"), 1.6);
            break;
        case 80:
            ControlObject::set(ConfigKey("[Master]", "gain"), 0.7);
            break;
        case 100:
            ControlObject::set(ConfigKey("[Channel1_Stem3]", "volume"), 0.3);
            ControlObject::set(ConfigKey("[Channel1_Stem4]", "mute"), 1);
            break;
        case 120:
            // The crossfader at the far right silences the left deck.
            ControlObject::set(ConfigKey("[Master]", "crossfader"), 1.0);
            break;
        }
    });
    EXPECT_GT(result.peak, 0.05) << "the track plays";
    EXPECT_LT(result.worst, 1e-4) << "in buffer " << result.worstAt;
    EXPECT_EQ(result.others, 0.0);
    EXPECT_LT(result.tail, 1e-6);
}

TEST_F(SpatialNullRunner, InstCarriesTheBass) {
    // Bass is folded into Other (INST): with Drums, INST and Vocals muted,
    // nothing plays, though the Bass stem itself is not muted.
    const NullResult result = run(40, [this](int b) {
        if (b == 0) {
            for (int stem : {1, 3, 4}) {
                ControlObject::set(
                        ConfigKey(QStringLiteral("[Channel1_Stem%1]").arg(stem), "mute"), 1);
            }
        }
    });
    for (std::size_t b = 5; b < result.mainRms.size(); ++b) {
        EXPECT_EQ(result.mainRms[b], 0) << "buffer " << b;
    }
    EXPECT_LT(result.worst, 1e-6);
}

TEST_F(SpatialNullRunner, DeckEqAppliesToEveryStem) {
    // The deck EQ runs once on the stereo sum and once per spatial stem
    // (mirrored inputs); being linear, the sums must match.
    const auto pManifest = m_pEffectsManager->getBackendManager()->getManifest(
            Bessel8LVMixEQEffect::getId(), EffectBackendType::BuiltIn);
    ASSERT_TRUE(pManifest);
    auto pEq = m_pEffectsManager->getEqualizerEffectChain(m_sGroup1);
    ASSERT_TRUE(pEq);
    pEq->getEffectSlot(0)->loadEffectWithDefaults(pManifest);
    const NullResult result = run(140, [this](int b) {
        switch (b) {
        case 20:
            ControlObject::set(ConfigKey(m_sGroup1, "filterLow"), 0.0);
            break;
        case 50:
            ControlObject::set(ConfigKey(m_sGroup1, "filterHigh"), 3.0);
            break;
        case 80:
            ControlObject::set(ConfigKey(m_sGroup1, "filterMid"), 0.2);
            ControlObject::set(ConfigKey(m_sGroup1, "filterLow"), 1.5);
            break;
        case 110:
            ControlObject::set(ConfigKey(m_sGroup1, "filterHighKill"), 1);
            break;
        }
    });
    EXPECT_GT(result.peak, 0.02) << "the track plays";
    EXPECT_LT(result.worst, 1e-4) << "in buffer " << result.worstAt;
    // And the EQ really acted: killing the lows changes the level.
    const auto average = [&](int from, int to) {
        return std::accumulate(result.mainRms.begin() + from, result.mainRms.begin() + to, 0.0) /
                (to - from);
    };
    EXPECT_LT(average(30, 48), average(5, 19) * 0.9);
}

} // namespace
#endif // __STEM__
