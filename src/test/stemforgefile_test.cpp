#include <gtest/gtest.h>

#include <QProcessEnvironment>
#include <cmath>
#include <iostream>

#include "library/stemconverter/stemtrackimporter.h"
#include "sources/soundsourceproxy.h"
#include "sources/soundsourcestem.h"
#include "test/mixxxtest.h"
#include "track/track.h"
#include "util/samplebuffer.h"

using namespace mixxx;

// Checks a .stem.mp4 written by stemforge (the offline separator next to this
// fork) the way a deck opens it: 8 channels, 4 named stems, and the stereo
// mix Mixxx derives for samplers and previews equal to the sum of the stems.
//
// Skipped unless STEMFORGE_TEST_FILE names such a file, e.g.
//   set STEMFORGE_TEST_FILE=D:\Stems\_stemforge-test\a.stem.mp4
//   mixxx-test --gtest_filter=StemforgeFileTest.*

namespace {

QString testFile() {
    return QProcessEnvironment::systemEnvironment().value(
            QStringLiteral("STEMFORGE_TEST_FILE"));
}

class StemforgeFileTest : public MixxxTest {
  protected:
    void SetUp() override {
        if (testFile().isEmpty()) {
            GTEST_SKIP() << "STEMFORGE_TEST_FILE not set";
        }
        ASSERT_TRUE(SoundSourceProxy::isFileTypeSupported("stem.mp4") ||
                SoundSourceProxy::registerProviders());
    }
};

TEST_F(StemforgeFileTest, OpensAsFourNamedStems) {
    TrackPointer pTrack(Track::newTemporary(testFile()));
    AudioSource::OpenParams config;
    config.setChannelCount(audio::ChannelCount::stereo());
    ASSERT_NE(SoundSourceProxy(pTrack).openAudioSource(config), nullptr);

    const auto stemInfo = pTrack->getStemInfo();
    ASSERT_EQ(stemInfo.size(), 4);
    EXPECT_EQ(stemInfo.at(0).getLabel(), QStringLiteral("Drums"));
    EXPECT_EQ(stemInfo.at(1).getLabel(), QStringLiteral("Bass"));
    EXPECT_EQ(stemInfo.at(2).getLabel(), QStringLiteral("Other"));
    EXPECT_EQ(stemInfo.at(3).getLabel(), QStringLiteral("Vocals"));
}

TEST_F(StemforgeFileTest, StereoMixIsSumOfStems) {
    const QUrl url = QUrl::fromLocalFile(testFile());
    SoundSourceSTEM stems(url);
    SoundSourceSTEM stereo(url);

    AudioSource::OpenParams stemConfig; // no request: deck (stem) mode
    ASSERT_EQ(stems.open(AudioSource::OpenMode::Strict, stemConfig),
            AudioSource::OpenResult::Succeeded);
    ASSERT_EQ(stems.getSignalInfo().getChannelCount(), audio::ChannelCount::stem());

    AudioSource::OpenParams stereoConfig;
    stereoConfig.setChannelCount(audio::ChannelCount::stereo());
    ASSERT_EQ(stereo.open(AudioSource::OpenMode::Strict, stereoConfig),
            AudioSource::OpenResult::Succeeded);
    ASSERT_EQ(stems.getSignalInfo().getSampleRate(), stereo.getSignalInfo().getSampleRate());

    // One second from 30 s in (or from the start for short files).
    const SINT frames = stems.getSignalInfo().getSampleRate();
    const SINT first = std::min<SINT>(30 * frames, stems.frameIndexRange().end() - frames);
    ASSERT_GE(first, 0);
    const auto range = IndexRange::forward(first, frames);

    SampleBuffer stemBuffer(frames * 8);
    SampleBuffer stereoBuffer(frames * 2);
    ASSERT_EQ(stems.readSampleFrames(WritableSampleFrames(range,
                                             SampleBuffer::WritableSlice(stemBuffer)))
                      .frameIndexRange()
                      .length(),
            frames);
    ASSERT_EQ(stereo.readSampleFrames(WritableSampleFrames(range,
                                              SampleBuffer::WritableSlice(stereoBuffer)))
                      .frameIndexRange()
                      .length(),
            frames);

    double peakError = 0;
    double peakSignal = 0;
    for (SINT f = 0; f < frames; ++f) {
        for (int ch = 0; ch < 2; ++ch) {
            double sum = 0;
            for (int stem = 0; stem < 4; ++stem) {
                sum += stemBuffer[f * 8 + stem * 2 + ch];
            }
            peakError = std::max(peakError, std::abs(sum - stereoBuffer[f * 2 + ch]));
            peakSignal = std::max(peakSignal, std::abs(sum));
        }
    }
    EXPECT_GT(peakSignal, 0.01) << "test window is silent";
    EXPECT_LT(peakError, 1e-5);
}

// With STEMFORGE_TEST_SOURCE also set to the file it was made from: the
// offset that cues and the beat grid are shifted by on import must be
// measurable. It is printed, since a non-zero value means Mixxx's decoder
// and stemforge's (ffmpeg) disagree on the source's start, e.g. MP3 delay.
TEST_F(StemforgeFileTest, OffsetAgainstSourceIsMeasurable) {
    const QString sourcePath = QProcessEnvironment::systemEnvironment().value(
            QStringLiteral("STEMFORGE_TEST_SOURCE"));
    if (sourcePath.isEmpty()) {
        GTEST_SKIP() << "STEMFORGE_TEST_SOURCE not set";
    }
    const TrackPointer pSource(Track::newTemporary(sourcePath));
    const TrackPointer pStem(Track::newTemporary(testFile()));
    const auto offset = stemconverter::measureOffset(pSource, pStem);
    ASSERT_TRUE(offset.has_value());
    std::cout << "[ OFFSET   ] " << *offset << " frames" << std::endl;
}

} // namespace
