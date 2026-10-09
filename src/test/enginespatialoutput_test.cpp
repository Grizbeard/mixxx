#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "control/controlobject.h"
#include "engine/spatial/enginespatialoutput.h"
#include "soundio/sounddevice.h"
#include "test/mixxxtest.h"

namespace {

constexpr int kFrames = 512;
const auto kRate = mixxx::audio::SampleRate(48000);

double rmsOf(const std::vector<double>& x) {
    double sum = 0;
    for (double v : x) {
        sum += v * v;
    }
    return x.empty() ? 0 : std::sqrt(sum / x.size());
}

class EngineSpatialOutputTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pOutput = std::make_unique<EngineSpatialOutput>();
        m_pOutput->onOutputConnected(m_out);
        set("speaker_count", 6);
        set("lf_enabled", 1);
        set("test_level_db", -20);
    }

    static void set(const char* key, double value) {
        ControlObject::set(ConfigKey("[Spatial]", key), value);
    }

    /// Run `buffers` callbacks; collect each channel of the last `keep`.
    std::vector<std::vector<double>> run(int buffers, int keep, const CSAMPLE* pMain = nullptr) {
        std::vector<std::vector<double>> channels(7);
        for (int b = 0; b < buffers; ++b) {
            m_pOutput->process(pMain, kFrames, kRate);
            if (b >= buffers - keep) {
                const auto buffer = m_pOutput->buffer(m_out);
                for (int f = 0; f < kFrames; ++f) {
                    for (int c = 0; c < 7; ++c) {
                        channels[c].push_back(buffer[f * 7 + c]);
                    }
                }
            }
        }
        return channels;
    }

    std::unique_ptr<EngineSpatialOutput> m_pOutput;
    // Six speakers + LF on sound-card channels 1-7.
    const AudioOutput m_out{AudioPath::AudioPathType::Spatial, 0, mixxx::audio::ChannelCount(7)};
};

TEST_F(EngineSpatialOutputTest, ReportsTheConnectedGroup) {
    EXPECT_EQ(ControlObject::get(ConfigKey("[Spatial]", "channel_count")), 7);
    m_pOutput->onOutputDisconnected(m_out);
    EXPECT_EQ(ControlObject::get(ConfigKey("[Spatial]", "channel_count")), 0);
    EXPECT_FALSE(m_pOutput->isConnected());
}

TEST_F(EngineSpatialOutputTest, SilentByDefault) {
    for (const auto& channel : run(4, 4)) {
        EXPECT_EQ(rmsOf(channel), 0);
    }
}

TEST_F(EngineSpatialOutputTest, TestSignalOnlyOnTheChosenChannelAtItsLevel) {
    set("test_channel", 3);
    // A callback to start, one to fade in, then measure a steady stretch.
    const auto channels = run(40, 30);
    for (int c = 0; c < 7; ++c) {
        if (c == 3) {
            const double db = 20 * std::log10(rmsOf(channels[c]));
            EXPECT_NEAR(db, -20, 1.5) << "pink noise RMS";
        } else {
            EXPECT_EQ(rmsOf(channels[c]), 0) << "channel " << c;
        }
    }
}

TEST_F(EngineSpatialOutputTest, MovingTheTestFadesOutBeforeFadingIn) {
    set("test_channel", 0);
    run(10, 1);
    set("test_channel", 1);
    // While moving, the two channels never sound together.
    for (int b = 0; b < 4; ++b) {
        const auto channels = run(1, 1);
        EXPECT_FALSE(rmsOf(channels[0]) > 0 && rmsOf(channels[1]) > 0) << "callback " << b;
    }
    const auto settled = run(10, 5);
    EXPECT_EQ(rmsOf(settled[0]), 0);
    EXPECT_GT(rmsOf(settled[1]), 0);
}

TEST_F(EngineSpatialOutputTest, LfTestIsBandLimited) {
    set("test_channel", 6); // the LF channel: after six speakers
    const auto lf = run(80, 60)[6];
    // A 100 Hz LR4 low-pass leaves little sample-to-sample change compared
    // to full-band pink noise, whose high end changes sign constantly.
    double diff = 0;
    for (std::size_t i = 1; i < lf.size(); ++i) {
        diff += (lf[i] - lf[i - 1]) * (lf[i] - lf[i - 1]);
    }
    const double roughness = std::sqrt(diff / lf.size()) / rmsOf(lf);
    EXPECT_LT(roughness, 0.05);
}

TEST_F(EngineSpatialOutputTest, PassthroughFeedsTheFrontPairAndLf) {
    set("main_passthrough", 1);
    // Left: 1 kHz, right: 50 Hz, so the LF should carry mostly the 50 Hz.
    std::vector<CSAMPLE> main(2 * kFrames);
    double phaseL = 0, phaseR = 0;
    std::vector<std::vector<double>> captured(7);
    for (int b = 0; b < 40; ++b) {
        for (int f = 0; f < kFrames; ++f) {
            main[2 * f] = static_cast<CSAMPLE>(0.5 * std::sin(phaseL));
            main[2 * f + 1] = static_cast<CSAMPLE>(0.5 * std::sin(phaseR));
            phaseL += 2 * M_PI * 1000 / kRate.toDouble();
            phaseR += 2 * M_PI * 50 / kRate.toDouble();
        }
        m_pOutput->process(main.data(), kFrames, kRate);
        if (b >= 20) {
            const auto buffer = m_pOutput->buffer(m_out);
            for (int f = 0; f < kFrames; ++f) {
                for (int c = 0; c < 7; ++c) {
                    captured[c].push_back(buffer[f * 7 + c]);
                }
            }
        }
    }
    EXPECT_NEAR(rmsOf(captured[0]), 0.5 / M_SQRT2, 0.01); // left as is
    EXPECT_NEAR(rmsOf(captured[1]), 0.5 / M_SQRT2, 0.01); // right as is
    for (int c = 2; c < 6; ++c) {
        EXPECT_EQ(rmsOf(captured[c]), 0) << "speaker " << c + 1;
    }
    // LF = half the mono sum, low-passed: the 50 Hz half survives (0.25 peak),
    // the 1 kHz half is ~84 dB down.
    EXPECT_NEAR(rmsOf(captured[6]), 0.25 / M_SQRT2, 0.02);
}

/// A sound device that only composes buffers.
class FakeDevice : public SoundDevice {
  public:
    FakeDevice()
            : SoundDevice(UserSettingsPointer(), nullptr) {
        m_numOutputChannels = mixxx::audio::ChannelCount(18);
    }
    SoundDeviceStatus open(bool, int) override {
        return SoundDeviceStatus::Ok;
    }
    bool isOpen() const override {
        return true;
    }
    SoundDeviceStatus close() override {
        return SoundDeviceStatus::Ok;
    }
    void readProcess(SINT) override {
    }
    void writeProcess(SINT) override {
    }
    QString getError() const override {
        return {};
    }
    mixxx::audio::SampleRate getDefaultSampleRate() const override {
        return kRate;
    }
    using SoundDevice::composeOutputBuffer;
};

TEST_F(EngineSpatialOutputTest, DeviceComposesAMultichannelGroupAtAnOffset) {
    // Seven channels starting at device channel 3, read from frame 5 on, as a
    // device fed through its FIFO does.
    constexpr int kChannels = 7;
    constexpr int kBase = 2;
    constexpr int kOffset = 5;
    constexpr int kCompose = 16;
    std::vector<CSAMPLE> source((kOffset + kCompose) * kChannels);
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<CSAMPLE>(i) / 1000.0f;
    }
    FakeDevice device;
    ASSERT_EQ(device.addOutput(AudioOutputBuffer(
                      AudioOutput(AudioPath::AudioPathType::Spatial,
                              kBase,
                              mixxx::audio::ChannelCount(kChannels)),
                      source.data())),
            SoundDeviceStatus::Ok);
    std::vector<CSAMPLE> out(kCompose * 18, -1.0f);
    device.composeOutputBuffer(out.data(), kCompose, kOffset, 18);
    for (int f = 0; f < kCompose; ++f) {
        for (int c = 0; c < 18; ++c) {
            const CSAMPLE expected = (c >= kBase && c < kBase + kChannels)
                    ? source[(kOffset + f) * kChannels + (c - kBase)]
                    : 0.0f;
            ASSERT_EQ(out[f * 18 + c], expected) << "frame " << f << " channel " << c;
        }
    }
}

} // namespace
