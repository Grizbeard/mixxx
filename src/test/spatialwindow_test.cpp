#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include <QPixmap>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>

#include "control/controlobject.h"
#include "engine/effects/groupfeaturestate.h"
#include "engine/spatial/enginespatialoutput.h"
#include "preferences/dialog/dlgprefspatial.h"
#include "spatial/spatialpanels.h"
#include "spatial/spatialpresets.h"
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
    set("[Channel1_Stem3]", "spatial_azimuth", -100);
    set("[Channel1_Stem3]", "spatial_spread", 0.3);
    set("[Channel2_Stem3]", "spatial_azimuth", 160);
    set("[Channel2_Stem3]", "spatial_width", 0.8);
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

    // One render per tab (auto-pan A, B, Spatial FX on delay, then reverb),
    // then the Basic layout.
    // (5: Basic with the auto-pan column on unit B.)
    for (int tab = 0; tab < 6; ++tab) {
        config()->setValue(ConfigKey("[SpatialWindow]", "tab"), std::min(tab, 2));
        config()->setValue(ConfigKey("[SpatialWindow]", "fx_unit"), tab == 3 ? 1 : 0);
        config()->setValue(ConfigKey("[SpatialWindow]", "layout"), tab >= 4 ? 1 : 0);
        set("[Spatial]", "autopan_focus", tab == 5 ? 1 : 0);
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

TEST_F(SpatialWindowTest, RendersATwelveSpeakerRing) {
    const AudioOutput output(
            AudioPath::AudioPathType::Spatial, 0, mixxx::audio::ChannelCount(13));
    m_pOutput->onOutputConnected(output);
    set("[Spatial]", "speaker_count", 12);
    set("[Spatial]", "lf_enabled", 1);
    config()->setValue(ConfigKey("[SpatialWindow]", "layout"), 0);
    SpatialWindow window(config(), nullptr, nullptr);
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1920, 1080);
    window.show();
    for (int i = 0; i < 10; ++i) {
        QTest::qWait(20);
    }
    const QImage image = window.grab().toImage();
    EXPECT_EQ(image.width(), 1920);
    DlgPrefSpatial page(nullptr, config());
    page.setAttribute(Qt::WA_DontShowOnScreen);
    page.resize(900, 1600);
    page.show();
    QCoreApplication::processEvents();
    QString path = qEnvironmentVariable("SPATIAL_SCREENSHOT");
    if (!path.isEmpty()) {
        EXPECT_TRUE(image.save(path.replace(QStringLiteral(".png"), QStringLiteral("_ring12.png"))));
        path = qEnvironmentVariable("SPATIAL_SCREENSHOT");
        EXPECT_TRUE(page.grab().toImage().save(
                path.replace(QStringLiteral(".png"), QStringLiteral("_prefs12.png"))));
    }
    m_pOutput->onOutputDisconnected(output);
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

TEST_F(SpatialWindowTest, PresetKeysAreRealControls) {
    for (const QString& key : spatialpresets::autopanKeys()) {
        EXPECT_NE(ControlObject::getControl(
                          ConfigKey(QStringLiteral("[SpatialAutopan1]"), key),
                          ControlFlag::AllowMissingOrInvalid),
                nullptr)
                << key.toStdString();
    }
    for (const QString& key : spatialpresets::fxKeys()) {
        const int split = key.indexOf(QStringLiteral("],"));
        ASSERT_GT(split, 0) << key.toStdString();
        EXPECT_NE(ControlObject::getControl(ConfigKey(key.left(split + 1), key.mid(split + 2)),
                          ControlFlag::AllowMissingOrInvalid),
                nullptr)
                << key.toStdString();
    }
}

TEST_F(SpatialWindowTest, FxPresetsCoverDelayAndReverbTogether) {
    QTemporaryDir settings;
    const ConfigKey delayOn(QStringLiteral("[SpatialDelay]"), QStringLiteral("enabled"));
    const ConfigKey reverbOn(QStringLiteral("[SpatialReverb]"), QStringLiteral("enabled"));
    {
        SpatialPresetBank bank(QStringLiteral("fx"),
                spatialpresets::fxKeys(),
                spatialpresets::fxFactory(),
                settings.path());
        // A reverb-only preset turns the delay off, and the other way round.
        bank.apply(bank.indexOf(QStringLiteral("VOCAL SPACE")), QString());
        EXPECT_EQ(ControlObject::get(delayOn), 0);
        EXPECT_EQ(ControlObject::get(reverbOn), 1);
        bank.apply(bank.indexOf(QStringLiteral("TRANCE GATE")), QString());
        EXPECT_EQ(ControlObject::get(delayOn), 1);
        EXPECT_EQ(ControlObject::get(reverbOn), 0);
        EXPECT_EQ(ControlObject::get(ConfigKey("[SpatialDelay]", "gate")), 1);
        // Together: echoes only into the wash.
        const int throwIndex = bank.indexOf(QStringLiteral("WASH THROW"));
        bank.apply(throwIndex, QString());
        EXPECT_EQ(ControlObject::get(delayOn), 1);
        EXPECT_EQ(ControlObject::get(reverbOn), 1);
        EXPECT_EQ(ControlObject::get(ConfigKey("[SpatialReverb]", "direct")), 0);
        EXPECT_TRUE(bank.matches(throwIndex, QString()));

        set("[SpatialDelay]", "feedback", 0.3);
        set("[SpatialReverb]", "decay", 20);
        EXPECT_FALSE(bank.matches(throwIndex, QString()));
        bank.saveCurrent(QStringLiteral("MY THROW"), QString());
        // Factory presets can't be overwritten.
        bank.saveCurrent(QStringLiteral("WASH THROW"), QString());
        bank.apply(throwIndex, QString());
        EXPECT_EQ(ControlObject::get(ConfigKey("[SpatialDelay]", "feedback")), 0.6);
    }
    {
        // A new session finds the user preset in the settings folder.
        SpatialPresetBank bank(QStringLiteral("fx"),
                spatialpresets::fxKeys(),
                spatialpresets::fxFactory(),
                settings.path());
        const int mine = bank.indexOf(QStringLiteral("MY THROW"));
        ASSERT_GE(mine, 0);
        bank.apply(mine, QString());
        EXPECT_EQ(ControlObject::get(ConfigKey("[SpatialDelay]", "feedback")), 0.3);
        EXPECT_EQ(ControlObject::get(ConfigKey("[SpatialReverb]", "decay")), 20);
        EXPECT_TRUE(bank.remove(QStringLiteral("MY THROW")));
        EXPECT_FALSE(bank.remove(QStringLiteral("WASH THROW")));
    }
    SpatialPresetBank bank(QStringLiteral("fx"),
            spatialpresets::fxKeys(),
            spatialpresets::fxFactory(),
            settings.path());
    EXPECT_LT(bank.indexOf(QStringLiteral("MY THROW")), 0);
}

TEST_F(SpatialWindowTest, SavingBakesTheMacrosIn) {
    set("[SpatialReverb]", "decay", 9);
    set("[SpatialDelay]", "feedback", 0.5);
    set("[SpatialFX]", "space", 0.75);
    set("[SpatialFX]", "echo", 0.25);
    spatialpresets::bakeFxMacros();
    EXPECT_NEAR(ControlObject::get(ConfigKey("[SpatialReverb]", "decay")),
            9 * std::sqrt(30.0 / 9),
            1e-6);
    EXPECT_NEAR(ControlObject::get(ConfigKey("[SpatialDelay]", "feedback")), 0.25, 1e-9);
    EXPECT_EQ(ControlObject::get(ConfigKey("[SpatialFX]", "space")), 0.5);
    EXPECT_EQ(ControlObject::get(ConfigKey("[SpatialFX]", "echo")), 0.5);
}

TEST_F(SpatialWindowTest, ThePresetPopupListsTheBankAndLoadsOnTap) {
    QTemporaryDir settings;
    SpatialPresetBank bank(QStringLiteral("fx"),
            spatialpresets::fxKeys(),
            spatialpresets::fxFactory(),
            settings.path());
    QWidget anchor;
    anchor.setAttribute(Qt::WA_DontShowOnScreen);
    anchor.resize(120, 48);
    anchor.show();
    SpatialPresetPopup::open(&bank, QString(), nullptr, QColor(0x02, 0x83, 0x92), &anchor);
    QWidget* pPopup = nullptr;
    for (QWidget* pWidget : QApplication::topLevelWidgets()) {
        if (qobject_cast<SpatialPresetPopup*>(pWidget) && pWidget->isVisible()) {
            pPopup = pWidget;
        }
    }
    ASSERT_NE(pPopup, nullptr);
    EXPECT_EQ(pPopup->findChildren<SpatialActionButton*>().size(), bank.presets().size());
    QString path = qEnvironmentVariable("SPATIAL_SCREENSHOT");
    if (!path.isEmpty()) {
        path.replace(QStringLiteral(".png"), QStringLiteral("_popup.png"));
        pPopup->grab().save(path);
    }
    // The tap on the PRESETS button that closes the pop-up (Qt closes a
    // pop-up on a press outside it) must not open it again.
    pPopup->close();
    SpatialPresetPopup::open(&bank, QString(), nullptr, QColor(0x02, 0x83, 0x92), &anchor);
    int open = 0;
    for (QWidget* pWidget : QApplication::topLevelWidgets()) {
        open += qobject_cast<SpatialPresetPopup*>(pWidget) && pWidget->isVisible() ? 1 : 0;
    }
    EXPECT_EQ(open, 0);
}

TEST_F(SpatialWindowTest, OlderDelayAndReverbPresetsMoveIntoTheFxList) {
    QTemporaryDir settings;
    QDir(settings.path()).mkpath(QStringLiteral("spatial_presets"));
    QFile old(settings.filePath(QStringLiteral("spatial_presets/delay.json")));
    ASSERT_TRUE(old.open(QIODevice::WriteOnly));
    old.write(R"({"presets":[{"name":"OLD ECHO","values":{"feedback":0.25,"beats":1}}]})");
    old.close();
    SpatialPresetBank bank(QStringLiteral("fx"),
            spatialpresets::fxKeys(),
            spatialpresets::fxFactory(),
            settings.path());
    bank.importLegacy(QStringLiteral("delay"), QStringLiteral("[SpatialDelay]"));
    const int index = bank.indexOf(QStringLiteral("OLD ECHO"));
    ASSERT_GE(index, 0);
    bank.apply(index, QString());
    EXPECT_EQ(ControlObject::get(ConfigKey("[SpatialDelay]", "feedback")), 0.25);
}

} // namespace
