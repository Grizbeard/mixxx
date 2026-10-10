#include "spatial/spatialpanels.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QVBoxLayout>
#include <cmath>
#include <numeric>

#include "control/controlproxy.h"
#include "engine/spatial/enginespatialoutput.h"
#include "engine/spatial/spatialmath.h"
#include "moc_spatialpanels.cpp"
#include "spatial/spatialstyle.h"

using namespace spatialstyle;

namespace spatialpanels {

QString beatLabel(double beats) {
    if (beats >= 1) {
        return QString::number(static_cast<int>(std::lround(beats)));
    }
    int numerator = static_cast<int>(std::lround(beats * 32));
    int denominator = 32;
    const int divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    return QStringLiteral("%1/%2").arg(numerator).arg(denominator);
}

QString percent(double value) {
    return QStringLiteral("%1%").arg(static_cast<int>(std::lround(value * 100)));
}

QWidget* row(const QString& label, const QList<QWidget*>& widgets, QWidget* pParent) {
    auto* pRow = new QWidget(pParent);
    auto* pLayout = new QHBoxLayout(pRow);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(4);
    auto* pLabel = new SpatialLabel(label, pRow);
    pLabel->setFixedWidth(64);
    pLayout->addWidget(pLabel);
    for (QWidget* pWidget : widgets) {
        pWidget->setParent(pRow);
        pLayout->addWidget(pWidget);
    }
    return pRow;
}

} // namespace spatialpanels

using namespace spatialpanels;

namespace {

QString degrees(double value) {
    return QStringLiteral("%1°").arg(static_cast<int>(std::lround(value)));
}

QString seconds(double value) {
    return value < 10 ? QStringLiteral("%1 s").arg(value, 0, 'f', 2)
                      : QStringLiteral("%1 s").arg(value, 0, 'f', 1);
}

QString milliseconds(double value) {
    return QStringLiteral("%1 ms").arg(static_cast<int>(std::lround(value)));
}

QString hertz(double value) {
    return value < 1000 ? QStringLiteral("%1 Hz").arg(static_cast<int>(std::lround(value)))
                        : QStringLiteral("%1 kHz").arg(value / 1000, 0, 'f', 1);
}

SpatialSlider* slider(const QString& group,
        const char* key,
        double min,
        double max,
        const QString& label,
        const QColor& accent,
        std::function<QString(double)> formatter = percent) {
    auto* pSlider = new SpatialSlider(group, key, min, max, label, Qt::Horizontal);
    pSlider->setAccent(accent);
    pSlider->setFormatter(std::move(formatter));
    return pSlider;
}

SpatialButton* toggle(const QString& group, const char* key, const QString& text, const QColor& accent) {
    auto* pButton = new SpatialButton(group, key, SpatialButton::Mode::Toggle, text);
    pButton->setAccent(accent);
    return pButton;
}

SpatialButton* select(const QString& group, const char* key, double value, const QString& text,
        const QColor& accent) {
    auto* pButton = new SpatialButton(group, key, SpatialButton::Mode::Select, text);
    pButton->setValue(value);
    pButton->setAccent(accent);
    return pButton;
}

QList<QWidget*> shapeButtons(const QString& group, const QColor& accent) {
    QList<QWidget*> buttons;
    for (int shape = 0; shape < mixxx::spatial::kShapeCount; ++shape) {
        auto* pButton = select(group, "shape", shape, QString(), accent);
        pButton->setIcon([shape](QPainter& p, const QRectF& r, const QColor& c) {
            spatialicons::paintShape(p, r, c, shape);
        });
        pButton->setMinimumSize(52, 48);
        buttons.append(pButton);
    }
    return buttons;
}

QList<QWidget*> waveButtons(const QString& group, const QColor& accent) {
    QList<QWidget*> buttons;
    for (int wave = 0; wave < mixxx::spatial::kWaveCount; ++wave) {
        auto* pButton = select(group, "wave", wave, QString(), accent);
        pButton->setIcon([wave](QPainter& p, const QRectF& r, const QColor& c) {
            spatialicons::paintWave(p, r, c, wave);
        });
        pButton->setMinimumSize(52, 48);
        buttons.append(pButton);
    }
    return buttons;
}

} // namespace

// ---------------------------------------------------------------------------

SpatialPanel::SpatialPanel(QWidget* pParent, bool dim)
        : QWidget(pParent),
          m_dim(dim) {
}

void SpatialPanel::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    paintContainer(p, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), m_dim ? kDim : kContainer);
}

SpatialLabel::SpatialLabel(const QString& text, QWidget* pParent)
        : QWidget(pParent),
          m_text(text),
          m_color(kLabel) {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumHeight(22);
}

void SpatialLabel::setText(const QString& text) {
    m_text = text;
    update();
}

void SpatialLabel::setColor(const QColor& color) {
    m_color = color;
    update();
}

QSize SpatialLabel::sizeHint() const {
    return {64, 22};
}

void SpatialLabel::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter p(this);
    p.setPen(m_color);
    p.setFont(styleFont(12, true));
    p.drawText(rect(), Qt::AlignCenter, m_text);
}

SpatialActionButton::SpatialActionButton(
        const QString& text, std::function<void()> action, QWidget* pParent)
        : SpatialTouchWidget(pParent),
          m_text(text),
          m_action(std::move(action)),
          m_accent(kHotOrange) {
    setMinimumSize(64, 40);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void SpatialActionButton::setActive(bool active) {
    if (m_active != active) {
        m_active = active;
        update();
    }
}

void SpatialActionButton::setAccent(const QColor& accent) {
    m_accent = accent;
    update();
}

void SpatialActionButton::pointerPressed(QPointF position) {
    Q_UNUSED(position);
    update();
    if (m_action) {
        m_action();
    }
}

void SpatialActionButton::pointerReleased(QPointF position) {
    Q_UNUSED(position);
    update();
}

void SpatialActionButton::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    paintButton(p, rect().adjusted(1, 1, -1, -1), m_active, m_pressed, m_accent);
    p.setPen(m_active ? QColor(Qt::black) : kLabel);
    p.setFont(styleFont(13, true));
    p.drawText(rect(), Qt::AlignCenter, m_text);
}

// ---------------------------------------------------------------------------

SpatialStemStrip::SpatialStemStrip(int deck, int slot, QWidget* pParent)
        : SpatialPanel(pParent),
          m_deck(deck),
          m_slot(slot) {
    const QString group = EngineSpatialOutput::sourceGroup(deck, slot);
    const QString deckGroup = QStringLiteral("[Channel%1]").arg(deck + 1);
    const QColor accent = deckColor(deck + 1);
    m_pStemCount = std::make_unique<PollingControlProxy>(
            deckGroup, "stem_count", ControlFlag::AllowMissingOrInvalid);
    m_pColor = std::make_unique<PollingControlProxy>(
            group, "color", ControlFlag::AllowMissingOrInvalid);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(4, 4, 4, 4);
    pLayout->setSpacing(3);

    m_pName = new SpatialLabel(QString(), this);
    pLayout->addWidget(m_pName);

    auto* pFaderRow = new QHBoxLayout();
    pFaderRow->setSpacing(2);
    m_pMeter = new SpatialMeter(group, "spatial_level", this);
    pFaderRow->addWidget(m_pMeter);
    auto* pVolume = new SpatialSlider(group, "volume", 0, 1, tr("VOL"), Qt::Vertical, this);
    pVolume->setAccent(accent);
    pVolume->setFormatter(percent);
    if (slot == 1) {
        // Instruments: bass and other together.
        pVolume->addMirror(QStringLiteral("[Channel%1_Stem3]").arg(deck + 1), "volume");
    }
    pFaderRow->addWidget(pVolume, 1);
    pLayout->addLayout(pFaderRow, 1);
    m_sliders.append(pVolume);

    auto* pPan = slider(group, "spatial_azimuth", -180, 180, tr("PAN"), accent, degrees);
    pPan->setBipolar(true);
    auto* pWidth = slider(group, "spatial_width", 0, 1, tr("WIDTH"), accent, [](double v) {
        return degrees(v * 180);
    });
    auto* pSpread = slider(group, "spatial_spread", 0, 1, tr("SPREAD"), accent);
    auto* pSend = slider(group, "spatial_fx_send", 0, 1, tr("FX SEND"), kTeal);
    for (SpatialSlider* pSlider : {pPan, pWidth, pSpread, pSend}) {
        pLayout->addWidget(pSlider);
        m_sliders.append(pSlider);
    }

    auto* pAutoRow = new QHBoxLayout();
    pAutoRow->setSpacing(2);
    auto* pAuto = toggle(group, "spatial_autopan", tr("AUTO"), kGreen);
    auto* pUnitA = select(group, "spatial_autopan_unit", 1, QStringLiteral("A"), kGreen);
    auto* pUnitB = select(group, "spatial_autopan_unit", 2, QStringLiteral("B"), kGreen);
    pAuto->setMinimumWidth(48);
    pUnitA->setMinimumWidth(26);
    pUnitB->setMinimumWidth(26);
    pAutoRow->addWidget(pAuto, 2);
    pAutoRow->addWidget(pUnitA, 1);
    pAutoRow->addWidget(pUnitB, 1);
    pLayout->addLayout(pAutoRow);
    auto* pOffset = slider(group, "spatial_autopan_offset", 0, 1, tr("OFFSET"), kGreen);
    pLayout->addWidget(pOffset);
    m_sliders.append(pOffset);
    auto* pFront = toggle(group, "spatial_front", tr("FRONT"), kHotOrange);
    pLayout->addWidget(pFront);
    m_buttons = {pAuto, pUnitA, pUnitB, pFront};

    setMinimumWidth(104);
    poll();
}

void SpatialStemStrip::poll() {
    m_pMeter->poll();
    const bool stems = m_pStemCount->get() > 0;
    const int available = stems ? 2 : (m_slot == 1 ? 1 : 0);
    const double color = m_pColor->get();
    if (available == m_available && color == m_color) {
        return;
    }
    m_available = available;
    m_color = color;
    static const char* const kNames[] = {"DRUMS", "INSTR", "VOCALS"};
    static const QColor kColors[] = {kDrums, kInstruments, kVocals};
    // A stereo track plays as the Instruments source.
    m_pName->setText(available == 1 ? tr("TRACK") : QString::fromLatin1(kNames[m_slot]));
    m_pName->setColor(available == 0
                    ? kDisabled
                    : (stems && color > 0 ? QColor::fromRgb(static_cast<QRgb>(color))
                                           : kColors[m_slot]));
    for (SpatialSlider* pSlider : std::as_const(m_sliders)) {
        pSlider->setControlEnabled(available > 0);
    }
    // The stem volume does nothing for a stereo track.
    m_sliders.first()->setControlEnabled(available == 2);
    for (SpatialButton* pButton : std::as_const(m_buttons)) {
        pButton->setControlEnabled(available > 0);
    }
}

// ---------------------------------------------------------------------------

SpatialDeckPanel::SpatialDeckPanel(int deck, QWidget* pParent)
        : QWidget(pParent) {
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(4);

    auto* pHeader = new QHBoxLayout();
    auto* pTitle = new SpatialLabel(tr("DECK %1").arg(deck + 1), this);
    pTitle->setColor(deckColor(deck + 1));
    pHeader->addWidget(pTitle, 1);
    // All stems of the deck to Front (or all back) at once.
    QList<ControlProxy*> fronts;
    for (int slot = 0; slot < EngineSpatialOutput::kSlots; ++slot) {
        fronts.append(new ControlProxy(EngineSpatialOutput::sourceGroup(deck, slot),
                "spatial_front",
                this,
                ControlFlag::NoAssertIfMissing));
    }
    auto* pAllFront = new SpatialActionButton(tr("ALL FRONT"), [fronts]() {
        bool all = true;
        for (ControlProxy* pFront : fronts) {
            all = all && pFront->get() > 0;
        }
        for (ControlProxy* pFront : fronts) {
            pFront->set(all ? 0 : 1);
        }
    }, this);
    pAllFront->setMaximumWidth(120);
    pHeader->addWidget(pAllFront);
    pLayout->addLayout(pHeader);

    auto* pStrips = new QHBoxLayout();
    pStrips->setSpacing(4);
    for (int slot = 0; slot < EngineSpatialOutput::kSlots; ++slot) {
        auto* pStrip = new SpatialStemStrip(deck, slot, this);
        pStrips->addWidget(pStrip);
        m_strips.append(pStrip);
    }
    pLayout->addLayout(pStrips, 1);
}

void SpatialDeckPanel::poll() {
    for (SpatialStemStrip* pStrip : std::as_const(m_strips)) {
        pStrip->poll();
    }
}

// ---------------------------------------------------------------------------

SpatialAutopanPanel::SpatialAutopanPanel(int unit, QWidget* pParent)
        : SpatialPanel(pParent),
          m_group(QStringLiteral("[SpatialAutopan%1]").arg(unit)) {
    m_pSync = std::make_unique<PollingControlProxy>(
            m_group, "sync", ControlFlag::AllowMissingOrInvalid);
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(8, 8, 8, 8);
    pLayout->setSpacing(6);

    pLayout->addWidget(row(tr("SHAPE"), shapeButtons(m_group, kGreen), this));
    pLayout->addWidget(row(tr("WAVE"), waveButtons(m_group, kGreen), this));

    QList<QWidget*> rate;
    rate.append(toggle(m_group, "sync", tr("SYNC"), kHotOrange));
    for (double beats : mixxx::spatial::kBeatDivisions) {
        auto* pButton = select(m_group, "beats", beats, beatLabel(beats), kGreen);
        m_beatButtons.append(pButton);
        rate.append(pButton);
    }
    rate.append(toggle(m_group, "triplet", QStringLiteral("T"), kGreen));
    pLayout->addWidget(row(tr("BEATS"), rate, this));

    m_pSeconds = slider(m_group, "seconds", 0.05, 60, tr("SECONDS PER POINT"), kGreen, seconds);
    m_pSeconds->setCurve(SpatialSlider::Curve::Log);
    auto* pSize = slider(m_group, "size", 0, 1, tr("SIZE"), kGreen);
    auto* pReverse = toggle(m_group, "direction", tr("REVERSE"), kGreen);
    auto* pRetrigger = new SpatialButton(
            m_group, "retrigger", SpatialButton::Mode::Momentary, tr("RETRIG"));
    pRetrigger->setAccent(kHotOrange);
    pReverse->setMaximumWidth(110);
    pRetrigger->setMaximumWidth(110);
    pLayout->addWidget(row(tr("MOTION"), {m_pSeconds, pSize, pReverse, pRetrigger}, this));
    pLayout->addStretch(1);
    poll();
}

void SpatialAutopanPanel::poll() {
    const bool sync = m_pSync->get() > 0;
    for (SpatialButton* pButton : std::as_const(m_beatButtons)) {
        pButton->setControlEnabled(sync);
    }
    m_pSeconds->setControlEnabled(!sync);
}

// ---------------------------------------------------------------------------

SpatialDelayPanel::SpatialDelayPanel(QWidget* pParent)
        : SpatialPanel(pParent) {
    const QString group = QStringLiteral("[SpatialDelay]");
    m_pSync = std::make_unique<PollingControlProxy>(
            group, "sync", ControlFlag::AllowMissingOrInvalid);
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(8, 8, 8, 8);
    pLayout->setSpacing(6);

    QList<QWidget*> time;
    time.append(toggle(group, "sync", tr("SYNC"), kHotOrange));
    for (double beats : {1.0 / 16, 1.0 / 8, 3.0 / 16, 1.0 / 4, 3.0 / 8, 1.0 / 2, 3.0 / 4, 1.0, 2.0, 4.0}) {
        auto* pButton = select(group, "beats", beats, beatLabel(beats), kTeal);
        m_beatButtons.append(pButton);
        time.append(pButton);
    }
    time.append(toggle(group, "triplet", QStringLiteral("T"), kTeal));
    m_pTimeMs = slider(group, "time_ms", 10, 4000, tr("TIME"), kTeal, milliseconds);
    m_pTimeMs->setCurve(SpatialSlider::Curve::Log);
    m_pTimeMs->setMinimumWidth(150);
    time.append(m_pTimeMs);
    pLayout->addWidget(row(tr("ECHO"), time, this));

    // Each echo sits on a point of the shape: no wave between them.
    QList<QWidget*> place = shapeButtons(group, kTeal);
    place.append(toggle(group, "direction", tr("REVERSE"), kTeal));
    auto* pSize = slider(group, "size", 0, 1, tr("SIZE"), kTeal);
    pSize->setMinimumWidth(110);
    place.append(pSize);
    pLayout->addWidget(row(tr("PLACE"), place, this));

    QList<QWidget*> rhythm;
    rhythm.append(toggle(group, "poly", tr("POLY"), kHotOrange));
    for (double beats : {1.0 / 4, 1.0 / 2, 1.0, 2.0, 4.0}) {
        rhythm.append(select(group, "poly_beats", beats, beatLabel(beats), kTeal));
    }
    rhythm.append(toggle(group, "poly_triplet", QStringLiteral("T"), kTeal));
    rhythm.append(toggle(group, "gate", tr("GATE"), kHotOrange));
    for (double beats : {1.0 / 32, 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2}) {
        rhythm.append(select(group, "gate_beats", beats, beatLabel(beats), kTeal));
    }
    auto* pDuty = slider(group, "gate_duty", 0.05, 0.95, tr("DUTY"), kTeal);
    auto* pSmooth = slider(group, "gate_smooth", 0, 1, tr("SMOOTH"), kTeal);
    pDuty->setMinimumWidth(100);
    pSmooth->setMinimumWidth(100);
    rhythm.append(pDuty);
    rhythm.append(pSmooth);
    pLayout->addWidget(row(tr("RHYTHM"), rhythm, this));

    QList<QWidget*> tone;
    tone.append(toggle(group, "enabled", tr("ON"), kTeal));
    tone.append(slider(group, "level", 0, 1, tr("LEVEL"), kTeal));
    tone.append(slider(group, "feedback", 0, 0.97, tr("FEEDBACK"), kTeal));
    tone.append(slider(group, "tone", 0, 1, tr("TONE"), kTeal, [](double v) {
        return hertz(1000.0 * std::pow(20.0, v));
    }));
    auto* pLowCut = slider(group, "lowcut", 20, 1000, tr("LOW CUT"), kTeal, hertz);
    pLowCut->setCurve(SpatialSlider::Curve::Log);
    tone.append(pLowCut);
    tone.append(slider(group, "to_reverb", 0, 1, tr("TO REVERB"), kTeal));
    pLayout->addWidget(row(tr("TONE"), tone, this));
    pLayout->addStretch(1);
    poll();
}

void SpatialDelayPanel::poll() {
    const bool sync = m_pSync->get() > 0;
    for (SpatialButton* pButton : std::as_const(m_beatButtons)) {
        pButton->setControlEnabled(sync);
    }
    m_pTimeMs->setControlEnabled(!sync);
}

// ---------------------------------------------------------------------------

namespace {

struct ReverbPreset {
    const char* name;
    double decay, size, predelay, damping, lowcut, modulation, diffusion, shimmer, duck;
};

// Starting points for trance vocals in a spatial mix.
constexpr ReverbPreset kReverbPresets[] = {
        {"VOCAL SPACE", 9, 0.8, 60, 0.45, 250, 0.6, 0.75, 0, 0.3},
        {"SHIMMER", 14, 0.9, 40, 0.35, 300, 0.7, 0.8, 0.5, 0.2},
        {"INFINITE", 30, 1.0, 20, 0.3, 300, 0.8, 0.85, 0.2, 0},
        {"PLATE", 3, 0.4, 15, 0.5, 200, 0.3, 0.9, 0, 0},
};

} // namespace

SpatialReverbPanel::SpatialReverbPanel(QWidget* pParent)
        : SpatialPanel(pParent) {
    const QString group = QStringLiteral("[SpatialReverb]");
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(8, 8, 8, 8);
    pLayout->setSpacing(6);

    QList<QWidget*> presets;
    presets.append(toggle(group, "enabled", tr("ON"), kTeal));
    auto* pFreeze = toggle(group, "freeze", tr("FREEZE"), kHotOrange);
    presets.append(pFreeze);
    static const char* const kKeys[] = {"decay", "size", "predelay_ms", "damping", "lowcut",
            "modulation", "diffusion", "shimmer", "duck"};
    for (const ReverbPreset& preset : kReverbPresets) {
        QList<ControlProxy*> proxies;
        for (const char* key : kKeys) {
            proxies.append(new ControlProxy(group, key, this, ControlFlag::NoAssertIfMissing));
        }
        const ReverbPreset copy = preset;
        presets.append(new SpatialActionButton(QString::fromLatin1(preset.name), [proxies, copy]() {
            const double values[] = {copy.decay, copy.size, copy.predelay, copy.damping,
                    copy.lowcut, copy.modulation, copy.diffusion, copy.shimmer, copy.duck};
            for (int i = 0; i < proxies.size(); ++i) {
                proxies[i]->set(values[i]);
            }
        }));
    }
    pLayout->addWidget(row(tr("REVERB"), presets, this));

    auto* pDecay = slider(group, "decay", 0.5, 30, tr("DECAY"), kTeal, seconds);
    pDecay->setCurve(SpatialSlider::Curve::Log);
    auto* pPredelay = slider(group, "predelay_ms", 0, 500, tr("PRE-DELAY"), kTeal, milliseconds);
    pLayout->addWidget(row(tr("SPACE"),
            {slider(group, "level", 0, 1, tr("LEVEL"), kTeal),
                    pDecay,
                    slider(group, "size", 0, 1, tr("SIZE"), kTeal),
                    pPredelay,
                    slider(group, "diffusion", 0, 1, tr("DIFFUSION"), kTeal)},
            this));
    auto* pLowCut = slider(group, "lowcut", 20, 1000, tr("LOW CUT"), kTeal, hertz);
    pLowCut->setCurve(SpatialSlider::Curve::Log);
    pLayout->addWidget(row(tr("COLOUR"),
            {slider(group, "damping", 0, 1, tr("DAMPING"), kTeal),
                    pLowCut,
                    slider(group, "modulation", 0, 1, tr("MODULATION"), kTeal),
                    slider(group, "shimmer", 0, 1, tr("SHIMMER"), kTeal),
                    slider(group, "duck", 0, 1, tr("DUCK"), kTeal)},
            this));
    pLayout->addStretch(1);
}
