#include "preferences/dialog/dlgprefspatial.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cmath>

#include "control/controlproxy.h"
#include "engine/spatial/spatialmath.h"
#include "moc_dlgprefspatial.cpp"

namespace {

const QString kGroup = QStringLiteral("[Spatial]");
constexpr int kWalkIntervalMs = 1500;

constexpr int kDefaultSpeakers = 4;
constexpr int kDefaultCrossoverHz = 100;
constexpr int kDefaultFrequencyHz = 1000;
constexpr double kDefaultLevelDb = -30;

} // namespace

DlgPrefSpatial::DlgPrefSpatial(QWidget* pParent, UserSettingsPointer pConfig)
        : DlgPreferencePage(pParent),
          m_pConfig(std::move(pConfig)) {
    const auto proxy = [this](const char* key) {
        return std::make_unique<ControlProxy>(kGroup, QString::fromLatin1(key), this);
    };
    m_pChannelCount = proxy("channel_count");
    m_pChannelBase = proxy("channel_base");
    m_pSpeakerCount = proxy("speaker_count");
    m_pLfEnabled = proxy("lf_enabled");
    m_pLfCrossover = proxy("lf_crossover_hz");
    m_pTestChannel = proxy("test_channel");
    m_pTestSignal = proxy("test_signal");
    m_pTestFrequency = proxy("test_frequency");
    m_pTestLevelDb = proxy("test_level_db");
    m_pMainPassthrough = proxy("main_passthrough");

    // Status: is a Spatial output assigned at all?
    m_pStatus = new QLabel;
    m_pStatus->setWordWrap(true);

    // The ring.
    m_pSpeakers = new QSpinBox;
    m_pSpeakers->setRange(4, 6);
    m_pLf = new QCheckBox(tr("The channel after the speakers carries the LF (sub) feed"));
    m_pCrossover = new QSpinBox;
    m_pCrossover->setRange(40, 200);
    m_pCrossover->setSuffix(tr(" Hz"));
    m_pCrossover->setToolTip(tr("Low-pass corner of the LF feed (Linkwitz-Riley, 24 dB/octave)."));
    auto* pRing = new QGroupBox(tr("Speaker ring"));
    auto* pRingForm = new QFormLayout(pRing);
    pRingForm->addRow(tr("Speakers:"), m_pSpeakers);
    pRingForm->addRow(QString(), m_pLf);
    pRingForm->addRow(tr("LF crossover:"), m_pCrossover);

    // One row per channel of the output, with a test button.
    auto* pChannels = new QGroupBox(tr("Channels"));
    auto* pChannelsLayout = new QVBoxLayout(pChannels);
    m_pChannelGrid = new QGridLayout;
    pChannelsLayout->addLayout(m_pChannelGrid);
    m_pTestButtons = new QButtonGroup(this);
    m_pTestButtons->setExclusive(false); // so a running test can be clicked off
    m_pWalk = new QPushButton(tr("Walk the ring"));
    m_pWalk->setCheckable(true);
    m_pWalk->setToolTip(tr("Play the test signal on each speaker in turn, then the LF."));
    auto* pWalkRow = new QHBoxLayout;
    pWalkRow->addWidget(m_pWalk);
    pWalkRow->addStretch(1);
    pChannelsLayout->addLayout(pWalkRow);

    // The test signal.
    m_pSignal = new QComboBox;
    m_pSignal->addItem(tr("Pink noise"));
    m_pSignal->addItem(tr("Sine"));
    m_pFrequency = new QSpinBox;
    m_pFrequency->setRange(20, 20000);
    m_pFrequency->setSuffix(tr(" Hz"));
    m_pLevel = new QDoubleSpinBox;
    m_pLevel->setRange(-60, -6);
    m_pLevel->setDecimals(0);
    m_pLevel->setSuffix(tr(" dBFS"));
    auto* pWarning = new QLabel(tr("Start low: %1 dBFS of pink noise is already loud on a "
                                   "club system. The LF channel gets the noise low-passed "
                                   "at the crossover, so it reads quieter.")
                                        .arg(kDefaultLevelDb));
    pWarning->setWordWrap(true);
    auto* pTest = new QGroupBox(tr("Test signal"));
    auto* pTestForm = new QFormLayout(pTest);
    pTestForm->addRow(tr("Signal:"), m_pSignal);
    pTestForm->addRow(tr("Sine frequency:"), m_pFrequency);
    pTestForm->addRow(tr("Level (RMS):"), m_pLevel);
    pTestForm->addRow(QString(), pWarning);

    // A fallback: the plain stereo mix through the room.
    m_pPassthrough = new QCheckBox(tr("Play the stereo main mix instead of the spatial mix"));
    m_pPassthrough->setToolTip(tr("For troubleshooting: the stereo main mix on the front pair "
                                  "and its mono sum low-passed on LF, bypassing the spatial "
                                  "renderer, FX, trims and limiter."));
    auto* pPass = new QGroupBox(tr("Stereo passthrough"));
    auto* pPassLayout = new QVBoxLayout(pPass);
    pPassLayout->addWidget(m_pPassthrough);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->addWidget(m_pStatus);
    pLayout->addWidget(pRing);
    pLayout->addWidget(createAlignmentGroup());
    pLayout->addWidget(pChannels);
    pLayout->addWidget(pTest);
    pLayout->addWidget(pPass);
    pLayout->addStretch(1);

    // Widgets drive the controls directly; there is nothing to apply later.
    connect(m_pSpeakers, &QSpinBox::valueChanged, this, [this](int value) {
        m_pSpeakerCount->set(value);
    });
    connect(m_pLf, &QCheckBox::toggled, this, [this](bool checked) {
        m_pLfEnabled->set(checked ? 1 : 0);
    });
    connect(m_pCrossover, &QSpinBox::valueChanged, this, [this](int value) {
        m_pLfCrossover->set(value);
    });
    connect(m_pSignal, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_pTestSignal->set(index);
        m_pFrequency->setEnabled(index == 1);
    });
    connect(m_pFrequency, &QSpinBox::valueChanged, this, [this](int value) {
        m_pTestFrequency->set(value);
    });
    connect(m_pLevel, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        m_pTestLevelDb->set(value);
    });
    connect(m_pPassthrough, &QCheckBox::toggled, this, [this](bool checked) {
        m_pMainPassthrough->set(checked ? 1 : 0);
    });
    connect(m_pTestButtons, &QButtonGroup::idToggled, this, &DlgPrefSpatial::slotTestToggled);
    connect(m_pWalk, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            m_walkChannel = -1;
            slotWalkStep();
            m_walkTimer.start();
        } else {
            stopTest();
        }
    });
    m_walkTimer.setInterval(kWalkIntervalMs);
    connect(&m_walkTimer, &QTimer::timeout, this, &DlgPrefSpatial::slotWalkStep);

    // The channel list follows the output assignment and the ring layout.
    m_pChannelCount->connectValueChanged(this, &DlgPrefSpatial::slotRebuildChannels);
    m_pChannelBase->connectValueChanged(this, &DlgPrefSpatial::slotRebuildChannels);
    m_pSpeakerCount->connectValueChanged(this, &DlgPrefSpatial::slotRebuildChannels);
    m_pLfEnabled->connectValueChanged(this, &DlgPrefSpatial::slotRebuildChannels);

    slotUpdate();
}

QDoubleSpinBox* DlgPrefSpatial::bindSpinBox(const QString& key,
        double min,
        double max,
        double step,
        int decimals,
        const QString& suffix) {
    auto* pSpinBox = new QDoubleSpinBox;
    pSpinBox->setRange(min, max);
    pSpinBox->setSingleStep(step);
    pSpinBox->setDecimals(decimals);
    pSpinBox->setSuffix(suffix);
    auto* pProxy = new ControlProxy(kGroup, key, this);
    pSpinBox->setValue(pProxy->get());
    connect(pSpinBox, &QDoubleSpinBox::valueChanged, pProxy, [pProxy](double value) {
        pProxy->set(value);
    });
    pProxy->connectValueChanged(pSpinBox, [pSpinBox](double value) {
        const QSignalBlocker blocker(pSpinBox);
        pSpinBox->setValue(value);
    });
    m_alignmentRefresh.append([pSpinBox, pProxy]() {
        const QSignalBlocker blocker(pSpinBox);
        pSpinBox->setValue(pProxy->get());
    });
    return pSpinBox;
}

QCheckBox* DlgPrefSpatial::bindCheckBox(const QString& key, const QString& text) {
    auto* pCheckBox = new QCheckBox(text);
    auto* pProxy = new ControlProxy(kGroup, key, this);
    pCheckBox->setChecked(pProxy->toBool());
    connect(pCheckBox, &QCheckBox::toggled, pProxy, [pProxy](bool checked) {
        pProxy->set(checked ? 1 : 0);
    });
    pProxy->connectValueChanged(pCheckBox, [pCheckBox](double value) {
        const QSignalBlocker blocker(pCheckBox);
        pCheckBox->setChecked(value > 0);
    });
    m_alignmentRefresh.append([pCheckBox, pProxy]() {
        const QSignalBlocker blocker(pCheckBox);
        pCheckBox->setChecked(pProxy->toBool());
    });
    return pCheckBox;
}

QGroupBox* DlgPrefSpatial::createAlignmentGroup() {
    auto* pGroup = new QGroupBox(tr("Room alignment"));
    auto* pLayout = new QVBoxLayout(pGroup);
    auto* pHelp = new QLabel(tr("Angles are as seen from the DJ: 0\u00b0 is front centre, 90\u00b0 "
                                "right, 180\u00b0 behind. Delay the nearer speakers so that every "
                                "speaker's sound arrives together at the centre of the floor "
                                "(about 2.9 ms per metre of difference)."));
    pHelp->setWordWrap(true);
    pLayout->addWidget(pHelp);

    m_pLayoutCustom = std::make_unique<ControlProxy>(kGroup, QStringLiteral("layout_custom"), this);
    auto* pGrid = new QGridLayout;
    pGrid->addWidget(new QLabel(tr("Angle")), 0, 1);
    pGrid->addWidget(new QLabel(tr("Trim")), 0, 2);
    pGrid->addWidget(new QLabel(tr("Delay")), 0, 3);
    for (int i = 0; i < mixxx::spatial::kMaxSpeakers; ++i) {
        const QString prefix = QStringLiteral("speaker_%1_").arg(i + 1);
        auto* pName = new QLabel(tr("Speaker %1").arg(i + 1));
        auto* pAngle = bindSpinBox(prefix + "azimuth", -180, 180, 1, 0, QStringLiteral("\u00b0"));
        auto* pTrim = bindSpinBox(prefix + "gain_db", -24, 6, 0.5, 1, tr(" dB"));
        auto* pDelay = bindSpinBox(prefix + "delay_ms", 0, 30, 0.1, 1, tr(" ms"));
        // Editing an angle switches the ring to the custom layout.
        connect(pAngle, &QDoubleSpinBox::valueChanged, this, [this]() {
            m_pLayoutCustom->set(1);
        });
        pGrid->addWidget(pName, i + 1, 0);
        pGrid->addWidget(pAngle, i + 1, 1);
        pGrid->addWidget(pTrim, i + 1, 2);
        pGrid->addWidget(pDelay, i + 1, 3);
        m_speakerRows.append({pName, pAngle, pTrim, pDelay});
    }
    const int lfRow = mixxx::spatial::kMaxSpeakers + 1;
    pGrid->addWidget(new QLabel(tr("LF (sub)")), lfRow, 0);
    pGrid->addWidget(bindCheckBox(QStringLiteral("lf_invert"), tr("Invert polarity")), lfRow, 1);
    pGrid->addWidget(bindSpinBox(QStringLiteral("lf_gain_db"), -24, 12, 0.5, 1, tr(" dB")),
            lfRow,
            2);
    pGrid->addWidget(bindSpinBox(QStringLiteral("lf_delay_ms"), 0, 30, 0.1, 1, tr(" ms")),
            lfRow,
            3);
    pGrid->setColumnStretch(4, 1);
    pLayout->addLayout(pGrid);

    auto* pResetAngles = new QPushButton(tr("Default angles"));
    pResetAngles->setToolTip(tr("Evenly spaced, with speakers 1 and 2 as the front pair."));
    connect(pResetAngles, &QPushButton::clicked, this, [this]() {
        const int count = static_cast<int>(std::lround(m_pSpeakerCount->get()));
        const auto defaults = mixxx::spatial::SpeakerLayout::defaultAzimuths(count);
        for (int i = 0; i < count; ++i) {
            ControlProxy(kGroup, QStringLiteral("speaker_%1_azimuth").arg(i + 1)).set(defaults[i]);
        }
        m_pLayoutCustom->set(0);
        refreshAlignment();
    });
    auto* pButtons = new QHBoxLayout;
    pButtons->addWidget(pResetAngles);
    pButtons->addStretch(1);
    pLayout->addLayout(pButtons);

    pLayout->addWidget(bindCheckBox(QStringLiteral("mains_highpass"),
            tr("High-pass the speakers at the LF crossover (when the LF feed is on)")));
    auto* pLimiterRow = new QHBoxLayout;
    pLimiterRow->addWidget(bindCheckBox(QStringLiteral("limiter_enabled"),
            tr("Output limiter, ceiling:")));
    pLimiterRow->addWidget(bindSpinBox(
            QStringLiteral("limiter_ceiling_db"), -12, 0, 0.1, 1, tr(" dBFS")));
    pLimiterRow->addStretch(1);
    pLayout->addLayout(pLimiterRow);
    return pGroup;
}

void DlgPrefSpatial::refreshAlignment() {
    for (const auto& refresh : std::as_const(m_alignmentRefresh)) {
        refresh();
    }
    // With the default layout, show the angles the engine uses.
    const int count = static_cast<int>(std::lround(m_pSpeakerCount->get()));
    const bool custom = m_pLayoutCustom->toBool();
    const auto defaults = mixxx::spatial::SpeakerLayout::defaultAzimuths(count);
    for (int i = 0; i < m_speakerRows.size(); ++i) {
        for (QWidget* pWidget : std::as_const(m_speakerRows[i])) {
            pWidget->setVisible(i < count);
        }
        if (!custom && i < count) {
            auto* pAngle = static_cast<QDoubleSpinBox*>(m_speakerRows[i][1]);
            const QSignalBlocker blocker(pAngle);
            pAngle->setValue(defaults[i]);
        }
    }
}

DlgPrefSpatial::~DlgPrefSpatial() {
    stopTest();
}

QString DlgPrefSpatial::roleOfChannel(int channel) const {
    const int channels = static_cast<int>(m_pChannelCount->get());
    const int speakers = std::min(static_cast<int>(std::lround(m_pSpeakerCount->get())), channels);
    if (channel < speakers) {
        if (channel == 0) {
            return tr("Speaker 1 (front left)");
        }
        if (channel == 1) {
            return tr("Speaker 2 (front right)");
        }
        return tr("Speaker %1").arg(channel + 1);
    }
    if (channel == speakers && m_pLfEnabled->toBool()) {
        return tr("LF (sub)");
    }
    return tr("unused");
}

void DlgPrefSpatial::slotRebuildChannels() {
    stopTest();
    for (QWidget* pWidget : std::as_const(m_channelWidgets)) {
        delete pWidget;
    }
    m_channelWidgets.clear();
    const auto buttons = m_pTestButtons->buttons();
    for (QAbstractButton* pButton : buttons) {
        m_pTestButtons->removeButton(pButton);
    }

    const int channels = static_cast<int>(m_pChannelCount->get());
    const int base = static_cast<int>(m_pChannelBase->get());
    if (channels <= 0) {
        m_pStatus->setText(tr("<b>No sound-card channels are assigned to the Spatial output.</b> "
                              "Under Sound Hardware, choose a device and channels for "
                              "\"Spatial\" in the Output list (for example Channels 1 - 7 "
                              "for six speakers and LF)."));
    } else {
        m_pStatus->setText(tr("Spatial output: %1 channels, sound-card channels %2 to %3.")
                                   .arg(channels)
                                   .arg(base + 1)
                                   .arg(base + channels));
    }
    m_pWalk->setEnabled(channels > 0);

    refreshAlignment();

    for (int channel = 0; channel < channels; ++channel) {
        auto* pChannelLabel = new QLabel(tr("Output %1").arg(base + channel + 1));
        auto* pRoleLabel = new QLabel(roleOfChannel(channel));
        auto* pTestButton = new QPushButton(tr("Test"));
        pTestButton->setCheckable(true);
        m_pChannelGrid->addWidget(pChannelLabel, channel, 0);
        m_pChannelGrid->addWidget(pRoleLabel, channel, 1);
        m_pChannelGrid->addWidget(pTestButton, channel, 2);
        m_pTestButtons->addButton(pTestButton, channel);
        m_channelWidgets << pChannelLabel << pRoleLabel << pTestButton;
    }
}

void DlgPrefSpatial::slotTestToggled(int channel, bool checked) {
    if (checked) {
        // One channel at a time: uncheck the rest without re-entering here.
        const auto buttons = m_pTestButtons->buttons();
        for (QAbstractButton* pButton : buttons) {
            if (m_pTestButtons->id(pButton) != channel && pButton->isChecked()) {
                pButton->blockSignals(true);
                pButton->setChecked(false);
                pButton->blockSignals(false);
            }
        }
        m_pTestChannel->set(channel);
    } else if (static_cast<int>(m_pTestChannel->get()) == channel) {
        m_pTestChannel->set(-1);
    }
}

void DlgPrefSpatial::slotWalkStep() {
    const int channels = static_cast<int>(m_pChannelCount->get());
    // Walk the speakers, then the LF; unused channels are skipped.
    int used = std::min(static_cast<int>(std::lround(m_pSpeakerCount->get())), channels);
    if (m_pLfEnabled->toBool() && used < channels) {
        ++used;
    }
    if (used <= 0) {
        stopTest();
        return;
    }
    m_walkChannel = (m_walkChannel + 1) % used;
    if (QAbstractButton* pButton = m_pTestButtons->button(m_walkChannel)) {
        pButton->setChecked(true); // also sets test_channel
    }
}

void DlgPrefSpatial::stopTest() {
    m_walkTimer.stop();
    if (m_pWalk && m_pWalk->isChecked()) {
        m_pWalk->blockSignals(true);
        m_pWalk->setChecked(false);
        m_pWalk->blockSignals(false);
    }
    if (m_pTestButtons) {
        const auto buttons = m_pTestButtons->buttons();
        for (QAbstractButton* pButton : buttons) {
            pButton->blockSignals(true);
            pButton->setChecked(false);
            pButton->blockSignals(false);
        }
    }
    if (m_pTestChannel) {
        m_pTestChannel->set(-1);
    }
}

void DlgPrefSpatial::hideEvent(QHideEvent* pEvent) {
    // Never leave a test signal running in the room behind a closed dialog.
    stopTest();
    DlgPreferencePage::hideEvent(pEvent);
}

void DlgPrefSpatial::slotUpdate() {
    const QList<QWidget*> widgets{m_pSpeakers, m_pLf, m_pCrossover, m_pSignal,
            m_pFrequency, m_pLevel, m_pPassthrough};
    for (QWidget* pWidget : widgets) {
        pWidget->blockSignals(true);
    }
    m_pSpeakers->setValue(static_cast<int>(std::lround(m_pSpeakerCount->get())));
    m_pLf->setChecked(m_pLfEnabled->toBool());
    m_pCrossover->setValue(static_cast<int>(std::lround(m_pLfCrossover->get())));
    m_pSignal->setCurrentIndex(m_pTestSignal->toBool() ? 1 : 0);
    m_pFrequency->setValue(static_cast<int>(std::lround(m_pTestFrequency->get())));
    m_pFrequency->setEnabled(m_pTestSignal->toBool());
    m_pLevel->setValue(m_pTestLevelDb->get());
    m_pPassthrough->setChecked(m_pMainPassthrough->toBool());
    for (QWidget* pWidget : widgets) {
        pWidget->blockSignals(false);
    }
    slotRebuildChannels();
}

void DlgPrefSpatial::slotApply() {
    // Settings took effect as they were changed.
}

void DlgPrefSpatial::slotResetToDefaults() {
    m_pSpeakers->setValue(kDefaultSpeakers);
    m_pLf->setChecked(true);
    m_pCrossover->setValue(kDefaultCrossoverHz);
    m_pSignal->setCurrentIndex(0);
    m_pFrequency->setValue(kDefaultFrequencyHz);
    m_pLevel->setValue(kDefaultLevelDb);
    m_pPassthrough->setChecked(false);
}
