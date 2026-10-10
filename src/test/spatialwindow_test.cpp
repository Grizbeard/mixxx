#include <gtest/gtest.h>

#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include <QPixmap>
#include <QTest>
#include <cmath>

#include "control/controlobject.h"
#include "engine/effects/groupfeaturestate.h"
#include "engine/spatial/enginespatialoutput.h"
#include "preferences/dialog/dlgprefspatial.h"
#include "spatial/spatialwindow.h"
#include "test/mixxxtest.h"

namespace {

/// Builds the spatial window over a live set of spatial controls and renders
/// it offscreen. Set SPATIAL_SCREENSHOT to a .png path to keep the render.
class SpatialWindowTest : public MixxxTest {
  protected:
    void SetUp() override {
        const QDir fonts(getTestDir().filePath(QStringLiteral("../../res/fonts")));
        for (const QString& file : fonts.entryList({QStringLiteral("*.ttf")})) {
            QFontDatabase::addApplicationFont(fonts.filePath(file));
        }
        m_pOutput = std::make_unique<EngineSpatialOutput>();
        m_pOutput->onOutputConnected(AudioOutput(
                AudioPath::AudioPathType::Spatial, 0, mixxx::audio::ChannelCount(7)));
        m_stemCount.push_back(std::make_unique<ControlObject>(ConfigKey("[Channel1]", "stem_count")));
        m_stemCount.push_back(std::make_unique<ControlObject>(ConfigKey("[Channel2]", "stem_count")));
        for (auto& pCount : m_stemCount) {
            pCount->set(4);
        }
    }

    static void set(const QString& group, const char* key, double value) {
        ControlObject::set(ConfigKey(group, key), value);
    }

    std::unique_ptr<EngineSpatialOutput> m_pOutput;
    std::vector<std::unique_ptr<ControlObject>> m_stemCount;
};

TEST_F(SpatialWindowTest, RendersAPerformanceScene) {
    set("[Spatial]", "speaker_count", 6);
    set("[Spatial]", "lf_enabled", 1);
    // A scene: deck 1 drums wide at the front, its vocals orbiting on unit
    // A; deck 2 instruments spread at the back. Positions are what the
    // engine publishes while playing; drive the published values by
    // rendering a few callbacks with some signal.
    set("[Channel1_Stem1]", "spatial_width", 0.6);
    set("[Channel1_Stem4]", "spatial_autopan", 1);
    set("[Channel1_Stem4]", "spatial_fx_send", 0.6);
    set("[Channel1_Stem2]", "spatial_azimuth", -100);
    set("[Channel1_Stem2]", "spatial_spread", 0.3);
    set("[Channel2_Stem2]", "spatial_azimuth", 160);
    set("[Channel2_Stem2]", "spatial_width", 0.8);
    set("[Channel2_Stem1]", "spatial_front", 1);
    set("[Channel2_Stem4]", "spatial_azimuth", 75);
    set("[Channel2_Stem4]", "spatial_autopan", 1);
    set("[Channel2_Stem4]", "spatial_autopan_unit", 2);
    set("[SpatialAutopan1]", "shape", 4);
    set("[SpatialAutopan1]", "wave", 2);
    set("[SpatialAutopan2]", "shape", 3);
    set("[SpatialAutopan2]", "size", 0.7);
    std::vector<CSAMPLE> stereo(2 * 480);
    for (std::size_t i = 0; i < stereo.size(); ++i) {
        stereo[i] = static_cast<CSAMPLE>(0.4 * std::sin(i * 0.05));
    }
    GroupFeatureState tempo;
    tempo.beat_length = GroupFeatureBeatLength{0.5, 1};
    for (int c = 1; c <= 60; ++c) {
        const double beats = c * 480 / 24000.0;
        tempo.beat_fraction_buffer_end = beats - std::floor(beats);
        m_pOutput->beginCallback(480, mixxx::audio::SampleRate(48000), &tempo, 0);
        for (int deck = 0; deck < 2; ++deck) {
            for (int slot = 0; slot < 3; ++slot) {
                m_pOutput->addSource(deck, slot, stereo.data());
            }
        }
        m_pOutput->process(nullptr, 480, mixxx::audio::SampleRate(48000));
    }

    // One render per tab (auto-pan A, B, delay, reverb).
    for (int tab = 0; tab < 4; ++tab) {
        config()->setValue(ConfigKey("[SpatialWindow]", "tab"), tab);
        SpatialWindow window(config(), nullptr, nullptr);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1920, 1080);
        window.show();
        QCoreApplication::processEvents();
        // Let the poll timer run a few times.
        for (int i = 0; i < 10; ++i) {
            QTest::qWait(20);
        }
        const QImage image = window.grab().toImage();
        EXPECT_EQ(image.width(), 1920);
        EXPECT_EQ(image.height(), 1080);
        QString path = qEnvironmentVariable("SPATIAL_SCREENSHOT");
        if (!path.isEmpty()) {
            path.replace(QStringLiteral(".png"), QStringLiteral("_%1.png").arg(tab));
            EXPECT_TRUE(image.save(path));
        }
    }
}

TEST_F(SpatialWindowTest, RendersThePreferencesPage) {
    set("[Spatial]", "speaker_count", 6);
    DlgPrefSpatial page(nullptr, config());
    page.setAttribute(Qt::WA_DontShowOnScreen);
    page.resize(900, 1400);
    page.show();
    QCoreApplication::processEvents();
    const QImage image = page.grab().toImage();
    EXPECT_GT(image.height(), 0);
    QString path = qEnvironmentVariable("SPATIAL_SCREENSHOT");
    if (!path.isEmpty()) {
        path.replace(QStringLiteral(".png"), QStringLiteral("_prefs.png"));
        EXPECT_TRUE(image.save(path));
    }
}

} // namespace
