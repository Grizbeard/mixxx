#pragma once

#include <QList>
#include <memory>

#include "control/pollingcontrolproxy.h"
#include "spatial/spatialpanels.h"

class SpatialKnob;
class SpatialPresetBank;

/// One stem column.
class SpatialBasicStem : public SpatialPanel {
    Q_OBJECT
  public:
    SpatialBasicStem(int deck, int slot, QWidget* pParent = nullptr);
    void poll();

  private:
    const int m_slot;
    SpatialLabel* m_pName;
    SpatialMeter* m_pMeter;
    QList<SpatialKnob*> m_knobs;
    QList<SpatialSlider*> m_sliders;
    QList<SpatialButton*> m_buttons;
    std::unique_ptr<PollingControlProxy> m_pStemCount;
    std::unique_ptr<PollingControlProxy> m_pColor;
    std::unique_ptr<PollingControlProxy> m_pUnit;
    SpatialButton* m_pAuto;
    int m_available = -1;
    double m_color = -2;
    int m_unit = 1;
};

/// The auto-pan column, playing unit A or B.
class SpatialBasicAutopan : public SpatialPanel {
    Q_OBJECT
  public:
    explicit SpatialBasicAutopan(SpatialPresetBank* pBank, QWidget* pParent = nullptr);
    void poll();

  private:
    SpatialLabel* m_pTitle;
    SpatialKnob* m_pShape;
    SpatialKnob* m_pWave;
    SpatialKnob* m_pBeats;
    SpatialSlider* m_pSize;
    SpatialButton* m_pReverse;
    SpatialButton* m_pFocusButton;
    std::unique_ptr<PollingControlProxy> m_pFocus;
    int m_unit = 0;
};

/// The Spatial FX column.
class SpatialBasicFx : public SpatialPanel {
    Q_OBJECT
  public:
    explicit SpatialBasicFx(SpatialPresetBank* pBank, QWidget* pParent = nullptr);
};

/// The Basic layout: eight columns laid out like the Novation Launch
/// Control XL MK3, each with 3 knobs, 1 fader and 2 buttons:
///   6 stems (deck 1 and 2: DRUMS, INST, VOCAL): pan, width, FX send;
///     volume; auto-pan, Front. The touch A/B buttons pick the unit.
///   AUTO-PAN: shape, wave, beats; size; reverse, and A/B choosing which
///     unit the column plays ([Spatial],autopan_focus).
///   SPATIAL FX: the SPACE, ECHO and TONE macros; LEVEL; delay on, reverb on.
class SpatialBasicSurface : public QWidget {
    Q_OBJECT
  public:
    SpatialBasicSurface(SpatialPresetBank* pAutopanBank,
            SpatialPresetBank* pFxBank,
            QWidget* pParent = nullptr);
    void poll();

  private:
    QList<SpatialBasicStem*> m_stems;
    SpatialBasicAutopan* m_pAutopan;
};
