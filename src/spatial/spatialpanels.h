#pragma once

#include <QWidget>
#include <functional>
#include <memory>

#include "control/pollingcontrolproxy.h"
#include "preferences/usersettings.h"
#include "spatial/spatialwidgets.h"

class QGridLayout;
class QStackedWidget;
class SpatialPresetBank;
class SpatialPresetBar;

/// A LateNight container frame for the panels below.
class SpatialPanel : public QWidget {
    Q_OBJECT
  public:
    explicit SpatialPanel(QWidget* pParent = nullptr, bool dim = false);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    const bool m_dim;
};

/// A small grey row label, LateNight's #KnobLabel style.
class SpatialLabel : public QWidget {
    Q_OBJECT
  public:
    SpatialLabel(const QString& text, QWidget* pParent = nullptr);
    void setText(const QString& text);
    void setColor(const QColor& color);
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    QString m_text;
    QColor m_color;
};

/// A button with its own action (tabs, presets), not bound to a control.
class SpatialActionButton : public SpatialTouchWidget {
    Q_OBJECT
  public:
    SpatialActionButton(const QString& text, std::function<void()> action, QWidget* pParent = nullptr);
    void setActive(bool active);
    void setAccent(const QColor& accent);
    void setAction(std::function<void()> action) {
        m_action = std::move(action);
    }
    QSize sizeHint() const override {
        return {120, 44};
    }

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void pointerPressed(QPointF position) override;
    void pointerMoved(QPointF position) override {
        Q_UNUSED(position);
    }
    void pointerReleased(QPointF position) override;

  private:
    QString m_text;
    std::function<void()> m_action;
    bool m_active = false;
    QColor m_accent;
};

/// One stem's strip: level, volume, pan, width, spread, FX send, auto-pan
/// (unit A/B and phase offset) and Front.
class SpatialStemStrip : public SpatialPanel {
    Q_OBJECT
  public:
    SpatialStemStrip(int deck, int slot, QWidget* pParent = nullptr);
    /// Polls meters and availability (stem track or not).
    void poll();

  private:
    const int m_deck;
    const int m_slot;
    SpatialLabel* m_pName;
    SpatialMeter* m_pMeter;
    QList<SpatialSlider*> m_sliders;
    QList<SpatialButton*> m_buttons;
    std::unique_ptr<PollingControlProxy> m_pStemCount;
    std::unique_ptr<PollingControlProxy> m_pColor;
    std::unique_ptr<PollingControlProxy> m_pUnit;
    SpatialButton* m_pAuto;
    SpatialSlider* m_pOffset;
    int m_available = -1;
    double m_color = -2;
    int m_unit = 1;
};

/// A deck's three strips with a deck header and "all Front".
class SpatialDeckPanel : public QWidget {
    Q_OBJECT
  public:
    SpatialDeckPanel(int deck, QWidget* pParent = nullptr);
    void poll();

  private:
    QList<SpatialStemStrip*> m_strips;
};

/// Auto-pan unit controls: shape, wave, rate (sync/triplet/seconds),
/// direction, size, retrigger.
class SpatialAutopanPanel : public SpatialPanel {
    Q_OBJECT
  public:
    SpatialAutopanPanel(int unit, SpatialPresetBank* pBank, QWidget* pParent = nullptr);
    void poll();

  private:
    QString m_group;
    SpatialPresetBar* m_pPresets;
    QList<SpatialButton*> m_beatButtons;
    SpatialSlider* m_pSeconds;
    std::unique_ptr<PollingControlProxy> m_pSync;
};

/// The Spatial FX unit's delay controls: time, placement shape, polyrhythm,
/// gate, tone.
class SpatialDelayPanel : public QWidget {
    Q_OBJECT
  public:
    explicit SpatialDelayPanel(QWidget* pParent = nullptr);
    void poll();

  private:
    QList<SpatialButton*> m_beatButtons;
    SpatialSlider* m_pTimeMs;
    std::unique_ptr<PollingControlProxy> m_pSync;
};

/// The Spatial FX unit's reverb controls.
class SpatialReverbPanel : public QWidget {
    Q_OBJECT
  public:
    explicit SpatialReverbPanel(QWidget* pParent = nullptr);
};

/// The Spatial FX unit: delay and reverb, fed by the stems' one FX send,
/// with one preset list for both (a preset also sets which units are on),
/// and a sub-tab per unit, each with its on/off beside it.
class SpatialFxPanel : public SpatialPanel {
    Q_OBJECT
  public:
    SpatialFxPanel(SpatialPresetBank* pBank, UserSettingsPointer pConfig, QWidget* pParent = nullptr);
    void poll();

  private:
    void showUnit(int index);

    const UserSettingsPointer m_pConfig;
    SpatialPresetBar* m_pPresets;
    QStackedWidget* m_pContent;
    SpatialDelayPanel* m_pDelay;
    SpatialReverbPanel* m_pReverb;
    QList<SpatialActionButton*> m_unitTabs;
};

/// Shared builders.
namespace spatialpanels {
QString beatLabel(double beats);
QString percent(double value);
/// How the send, delay and reverb connect, for the FX tabs.
QWidget* routingNote(QWidget* pParent);
QWidget* row(const QString& label, const QList<QWidget*>& widgets, QWidget* pParent);
} // namespace spatialpanels
