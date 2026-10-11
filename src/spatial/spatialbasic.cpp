#include "spatial/spatialbasic.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <cmath>

#include "engine/spatial/enginespatialoutput.h"
#include "engine/spatial/spatialmath.h"
#include "moc_spatialbasic.cpp"
#include "spatial/spatialpresets.h"
#include "spatial/spatialstyle.h"
#include "spatial/spatialwidgets.h"

using namespace spatialstyle;
using spatialpanels::beatLabel;
using spatialpanels::percent;

namespace {

QString degrees(double value) {
    return QStringLiteral("%1°").arg(static_cast<int>(std::lround(value)));
}

/// A macro's offset from its neutral middle, as -100..+100.
QString macro(double value) {
    const int offset = static_cast<int>(std::lround((value - 0.5) * 200));
    return offset == 0 ? QStringLiteral("0") : QStringLiteral("%1%2").arg(offset > 0 ? "+" : "").arg(offset);
}

SpatialButton* toggle(const QString& group, const char* key, const QString& text, const QColor& accent) {
    auto* pButton = new SpatialButton(group, key, SpatialButton::Mode::Toggle, text);
    pButton->setAccent(accent);
    pButton->setMinimumHeight(48);
    return pButton;
}

SpatialSlider* fader(const QString& group, const char* key, const QString& label, const QColor& accent) {
    auto* pFader = new SpatialSlider(group, key, 0, 1, label, Qt::Vertical);
    pFader->setAccent(accent);
    pFader->setFormatter(percent);
    pFader->setMinimumHeight(180);
    return pFader;
}

QString autopanGroup(int unit) {
    return QStringLiteral("[SpatialAutopan%1]").arg(unit + 1);
}

} // namespace

// ---------------------------------------------------------------------------

SpatialBasicStem::SpatialBasicStem(int deck, int slot, QWidget* pParent)
        : SpatialPanel(pParent),
          m_slot(slot) {
    const QString group = EngineSpatialOutput::sourceGroup(deck, slot);
    const QColor accent = deckColor(deck + 1);
    m_pStemCount = std::make_unique<PollingControlProxy>(QStringLiteral("[Channel%1]").arg(deck + 1),
            "stem_count",
            ControlFlag::AllowMissingOrInvalid);
    m_pColor = std::make_unique<PollingControlProxy>(
            colorGroupForSource(deck, slot), "color", ControlFlag::AllowMissingOrInvalid);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(4, 4, 4, 4);
    pLayout->setSpacing(2);
    m_pName = new SpatialLabel(QString(), this);
    pLayout->addWidget(m_pName);

    auto* pPan = new SpatialKnob(group, "spatial_azimuth", -180, 180, tr("PAN"), this);
    pPan->setBipolar(true);
    pPan->setFormatter(degrees);
    auto* pWidth = new SpatialKnob(group, "spatial_width", 0, 1, tr("WIDTH"), this);
    pWidth->setFormatter([](double v) { return degrees(v * 180); });
    auto* pSend = new SpatialKnob(group, "spatial_fx_send", 0, 1, tr("SEND"), this);
    pSend->setFormatter(percent);
    pSend->setAccent(kTeal);
    for (SpatialKnob* pKnob : {pPan, pWidth, pSend}) {
        if (pKnob != pSend) {
            pKnob->setAccent(accent);
        }
        pLayout->addWidget(pKnob);
        m_knobs.append(pKnob);
    }

    auto* pFaderRow = new QHBoxLayout();
    pFaderRow->setSpacing(2);
    m_pMeter = new SpatialMeter(group, "spatial_level", this);
    pFaderRow->addWidget(m_pMeter);
    auto* pVolume = fader(group, "volume", tr("VOL"), accent);
    pFaderRow->addWidget(pVolume, 1);
    pLayout->addLayout(pFaderRow, 1);
    m_sliders.append(pVolume);

    auto* pAuto = toggle(group, "spatial_autopan", tr("AUTO"), kUnitA);
    m_pAuto = pAuto;
    m_pUnit = std::make_unique<PollingControlProxy>(
            group, "spatial_autopan_unit", ControlFlag::AllowMissingOrInvalid);
    auto* pFront = toggle(group, "spatial_front", tr("FRONT"), kHotOrange);
    // Touch only: which auto-pan unit the stem follows.
    auto* pUnits = new QHBoxLayout();
    pUnits->setSpacing(2);
    for (int unit = 1; unit <= 2; ++unit) {
        auto* pUnit = new SpatialButton(group,
                "spatial_autopan_unit",
                SpatialButton::Mode::Select,
                unit == 1 ? QStringLiteral("A") : QStringLiteral("B"));
        pUnit->setValue(unit);
        pUnit->setAccent(unitColor(unit));
        pUnit->setMinimumHeight(36);
        pUnits->addWidget(pUnit);
        m_buttons.append(pUnit);
    }
    pLayout->addWidget(pAuto);
    pLayout->addLayout(pUnits);
    pLayout->addWidget(pFront);
    m_buttons.append(pAuto);
    m_buttons.append(pFront);
    setMinimumWidth(100);
    poll();
}

void SpatialBasicStem::poll() {
    m_pMeter->poll();
    const int unit = m_pUnit->get() > 1.5 ? 2 : 1;
    if (unit != m_unit) {
        m_unit = unit;
        m_pAuto->setAccent(unitColor(unit));
    }
    const bool stems = m_pStemCount->get() > 0;
    const int available = stems ? 2 : (m_slot == 1 ? 1 : 0);
    const double color = m_pColor->get();
    if (available == m_available && color == m_color) {
        return;
    }
    m_available = available;
    m_color = color;
    static const char* const kNames[] = {"DRUMS", "INST", "VOCAL"};
    static const QColor kColors[] = {kDrums, kInstruments, kVocals};
    m_pName->setText(available == 1 ? tr("TRACK") : QString::fromLatin1(kNames[m_slot]));
    m_pName->setColor(available == 0
                    ? kDisabled
                    : (stems && color > 0 ? QColor::fromRgb(static_cast<QRgb>(color))
                                          : kColors[m_slot]));
    for (SpatialKnob* pKnob : std::as_const(m_knobs)) {
        pKnob->setControlEnabled(available > 0);
    }
    m_sliders.first()->setControlEnabled(available == 2);
    for (SpatialButton* pButton : std::as_const(m_buttons)) {
        pButton->setControlEnabled(available > 0);
    }
}

// ---------------------------------------------------------------------------

SpatialBasicAutopan::SpatialBasicAutopan(SpatialPresetBank* pBank, QWidget* pParent)
        : SpatialPanel(pParent) {
    m_pFocus = std::make_unique<PollingControlProxy>(
            QStringLiteral("[Spatial]"), "autopan_focus", ControlFlag::AllowMissingOrInvalid);
    const QString group = autopanGroup(0);
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(4, 4, 4, 4);
    pLayout->setSpacing(2);
    m_pTitle = new SpatialLabel(tr("AUTO-PAN A"), this);
    m_pTitle->setColor(kGreen);
    pLayout->addWidget(m_pTitle);

    m_pShape = new SpatialKnob(group, "shape", 0, mixxx::spatial::kShapeCount - 1, tr("SHAPE"), this);
    QList<double> shapes;
    for (int i = 0; i < mixxx::spatial::kShapeCount; ++i) {
        shapes.append(i);
    }
    m_pShape->setSteps(shapes);
    m_pShape->setValuePainter([](QPainter& p, const QRectF& r, const QColor& c, double v) {
        const double side = r.height();
        spatialicons::paintShape(p,
                QRectF(r.center().x() - side / 2, r.top(), side, side),
                c,
                static_cast<int>(std::lround(v)));
    });
    m_pWave = new SpatialKnob(group, "wave", 0, mixxx::spatial::kWaveCount - 1, tr("WAVE"), this);
    QList<double> waves;
    for (int i = 0; i < mixxx::spatial::kWaveCount; ++i) {
        waves.append(i);
    }
    m_pWave->setSteps(waves);
    m_pWave->setValuePainter([](QPainter& p, const QRectF& r, const QColor& c, double v) {
        spatialicons::paintWave(p, r, c, static_cast<int>(std::lround(v)));
    });
    m_pBeats = new SpatialKnob(group, "beats", 1.0 / 16, 16, tr("BEATS"), this);
    QList<double> beats;
    for (double b : mixxx::spatial::kBeatDivisions) {
        beats.append(b);
    }
    m_pBeats->setSteps(beats);
    m_pBeats->setFormatter(beatLabel);
    for (SpatialKnob* pKnob : {m_pShape, m_pWave, m_pBeats}) {
        pKnob->setAccent(kGreen);
        pLayout->addWidget(pKnob);
    }

    m_pSize = fader(group, "size", tr("SIZE"), kGreen);
    pLayout->addWidget(m_pSize, 1);

    m_pReverse = toggle(group, "direction", tr("REVERSE"), kGreen);
    pLayout->addWidget(m_pReverse);
    // The controller's last button: play unit A or B with this column.
    m_pFocusButton = toggle(QStringLiteral("[Spatial]"), "autopan_focus", tr("A / B"), kGreen);
    pLayout->addWidget(m_pFocusButton);
    // Touch only: choose a preset for the unit the column is playing.
    auto* pPresets = new SpatialActionButton(tr("PRESETS"), nullptr, this);
    pPresets->setAction([this, pBank, pPresets]() {
        SpatialPresetPopup::open(pBank, autopanGroup(m_unit), nullptr, unitColor(m_unit + 1), pPresets);
    });
    pPresets->setMinimumHeight(48);
    pLayout->addWidget(pPresets);
    setMinimumWidth(110);
    m_unit = -1;
    poll();
}

void SpatialBasicAutopan::poll() {
    const int unit = m_pFocus->get() > 0 ? 1 : 0;
    if (unit == m_unit) {
        return;
    }
    m_unit = unit;
    const QString group = autopanGroup(unit);
    for (SpatialKnob* pKnob : {m_pShape, m_pWave, m_pBeats}) {
        pKnob->rebind(group);
    }
    m_pSize->rebind(group);
    m_pReverse->rebind(group);
    m_pTitle->setText(unit == 0 ? tr("AUTO-PAN A") : tr("AUTO-PAN B"));
    m_pFocusButton->setText(unit == 0 ? tr("A / B") : tr("B / A"));
    // Green for A, orange for B: which unit the column plays at a glance.
    const QColor accent = unitColor(unit + 1);
    m_pTitle->setColor(accent);
    for (SpatialKnob* pKnob : {m_pShape, m_pWave, m_pBeats}) {
        pKnob->setAccent(accent);
    }
    m_pSize->setAccent(accent);
    m_pReverse->setAccent(accent);
    m_pFocusButton->setAccent(accent);
}

// ---------------------------------------------------------------------------

SpatialBasicFx::SpatialBasicFx(SpatialPresetBank* pBank, QWidget* pParent)
        : SpatialPanel(pParent) {
    const QString group = QStringLiteral("[SpatialFX]");
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(4, 4, 4, 4);
    pLayout->setSpacing(2);
    auto* pTitle = new SpatialLabel(tr("SPATIAL FX"), this);
    pTitle->setColor(kTeal);
    pLayout->addWidget(pTitle);
    const std::pair<const char*, QString> macros[] = {
            {"space", tr("SPACE")}, {"echo", tr("ECHO")}, {"tone", tr("TONE")}};
    for (const auto& [key, label] : macros) {
        auto* pKnob = new SpatialKnob(group, key, 0, 1, label, this);
        pKnob->setBipolar(true);
        pKnob->setFormatter(macro);
        pKnob->setAccent(kTeal);
        pLayout->addWidget(pKnob);
    }
    pLayout->addWidget(fader(group, "level", tr("LEVEL"), kTeal), 1);
    pLayout->addWidget(toggle(QStringLiteral("[SpatialDelay]"), "enabled", tr("DELAY"), kGreen));
    pLayout->addWidget(toggle(QStringLiteral("[SpatialReverb]"), "enabled", tr("REVERB"), kGreen));
    auto* pPresets = new SpatialActionButton(tr("PRESETS"), nullptr, this);
    pPresets->setAction([pBank, pPresets]() {
        SpatialPresetPopup::open(
                pBank, QString(), spatialpresets::centreFxMacros, kTeal, pPresets);
    });
    pPresets->setMinimumHeight(48);
    pLayout->addWidget(pPresets);
    setMinimumWidth(110);
}

// ---------------------------------------------------------------------------

SpatialBasicSurface::SpatialBasicSurface(
        SpatialPresetBank* pAutopanBank, SpatialPresetBank* pFxBank, QWidget* pParent)
        : QWidget(pParent) {
    auto* pLayout = new QHBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(4);
    for (int deck = 0; deck < 2; ++deck) {
        for (int slot = 0; slot < EngineSpatialOutput::kSlots; ++slot) {
            auto* pStem = new SpatialBasicStem(deck, slot, this);
            m_stems.append(pStem);
            pLayout->addWidget(pStem);
        }
        pLayout->addSpacing(6);
    }
    m_pAutopan = new SpatialBasicAutopan(pAutopanBank, this);
    pLayout->addWidget(m_pAutopan);
    pLayout->addWidget(new SpatialBasicFx(pFxBank, this));
}

void SpatialBasicSurface::poll() {
    for (SpatialBasicStem* pStem : std::as_const(m_stems)) {
        pStem->poll();
    }
    m_pAutopan->poll();
}
