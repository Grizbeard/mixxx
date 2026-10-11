#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QPointF>
#include <QWidget>
#include <functional>

class ControlProxy;

/// Touch-first controls for the spatial window, drawn in the LateNight
/// PaleMoon style. Each handles its own touch points, so several can be
/// moved at once with different fingers; the mouse works the same way.

/// Base: one pointer (the first touch point or the mouse) per widget, plus
/// double-tap detection.
class SpatialTouchWidget : public QWidget {
    Q_OBJECT
  public:
    explicit SpatialTouchWidget(QWidget* pParent = nullptr);

  protected:
    virtual void pointerPressed(QPointF position) = 0;
    virtual void pointerMoved(QPointF position) = 0;
    virtual void pointerReleased(QPointF position) = 0;
    virtual void doubleTapped(QPointF position) {
        Q_UNUSED(position);
    }

    bool event(QEvent* pEvent) override;
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseMoveEvent(QMouseEvent* pEvent) override;
    void mouseReleaseEvent(QMouseEvent* pEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* pEvent) override;

    bool m_pressed = false;

  private:
    void press(QPointF position);
    int m_touchId = -1;
    QElapsedTimer m_lastTap;
    QPointF m_lastTapPosition;
};

/// A fader bound to a control. Drags are relative (no jump on touch);
/// double-tap resets to the default. Optionally writes extra controls too
/// (to keep linked controls in step).
class SpatialSlider : public SpatialTouchWidget {
    Q_OBJECT
  public:
    enum class Curve {
        Linear,
        Log, // min > 0: equal ratios per distance (times, frequencies)
    };

    SpatialSlider(const QString& group,
            const QString& key,
            double min,
            double max,
            const QString& label,
            Qt::Orientation orientation,
            QWidget* pParent = nullptr);

    void setAccent(const QColor& accent);
    void setCurve(Curve curve);
    /// Fill from the middle (pan-like controls).
    void setBipolar(bool bipolar);
    void setFormatter(std::function<QString(double)> formatter);
    void addMirror(const QString& group, const QString& key);
    void setControlEnabled(bool enabled);
    /// Points the slider at the same key of another group.
    void rebind(const QString& group);

    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void pointerPressed(QPointF position) override;
    void pointerMoved(QPointF position) override;
    void pointerReleased(QPointF position) override;
    void doubleTapped(QPointF position) override;

  private:
    double parameter() const;
    void setParameter(double parameter);
    double travel() const;
    QRectF trackRect() const;

    ControlProxy* m_pControl;
    QList<ControlProxy*> m_mirrors;
    const double m_min;
    const double m_max;
    const QString m_label;
    const Qt::Orientation m_orientation;
    QColor m_accent;
    Curve m_curve = Curve::Linear;
    bool m_bipolar = false;
    bool m_enabled = true;
    std::function<QString(double)> m_formatter;
    QPointF m_pressPosition;
    double m_pressParameter = 0;
};

/// A button bound to a control: a toggle, a momentary button, or one of a
/// set of buttons each selecting a value (shapes, beat lengths). Draws a
/// text label or a custom icon.
class SpatialButton : public SpatialTouchWidget {
    Q_OBJECT
  public:
    enum class Mode {
        Toggle,
        Momentary,
        Select, // sets `value`; lit while the control equals it
    };
    using IconPainter = std::function<void(QPainter&, const QRectF&, const QColor&)>;

    SpatialButton(const QString& group,
            const QString& key,
            Mode mode,
            const QString& text,
            QWidget* pParent = nullptr);

    void setValue(double value) {
        m_value = value;
    }
    void setText(const QString& text) {
        m_text = text;
        update();
    }
    /// Lit whatever the value: for switches between two states shown by
    /// colour (auto-pan A green / B orange).
    void setAlwaysLit(bool alwaysLit) {
        m_alwaysLit = alwaysLit;
        update();
    }
    void setAccent(const QColor& accent);
    void setIcon(IconPainter painter);
    void setFontSize(int pixels);
    void setControlEnabled(bool enabled);
    /// Extra controls this button writes with the same value.
    void addMirror(const QString& group, const QString& key);
    /// Points the button at the same key of another group.
    void rebind(const QString& group);

    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void pointerPressed(QPointF position) override;
    void pointerMoved(QPointF position) override;
    void pointerReleased(QPointF position) override;

  private:
    bool isActive() const;
    void write(double value);

    ControlProxy* m_pControl;
    QList<ControlProxy*> m_mirrors;
    const Mode m_mode;
    QString m_text;
    double m_value = 1;
    QColor m_accent;
    IconPainter m_icon;
    int m_fontSize = 13;
    bool m_enabled = true;
    bool m_alwaysLit = false;
};

/// A knob bound to a control, LateNight style: an arc ring that fills with
/// the value. Drag up/down (relative, no jump); double-tap resets. With
/// steps it snaps through a list of values (shapes, waves, beat lengths)
/// and can draw each one.
class SpatialKnob : public SpatialTouchWidget {
    Q_OBJECT
  public:
    using ValuePainter = std::function<void(QPainter&, const QRectF&, const QColor&, double)>;

    SpatialKnob(const QString& group,
            const QString& key,
            double min,
            double max,
            const QString& label,
            QWidget* pParent = nullptr);

    void setAccent(const QColor& accent);
    void setBipolar(bool bipolar);
    void setFormatter(std::function<QString(double)> formatter);
    void setSteps(const QList<double>& steps);
    void setValuePainter(ValuePainter painter);
    void setControlEnabled(bool enabled);
    void rebind(const QString& group);

    QSize sizeHint() const override {
        return {96, 118};
    }

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void pointerPressed(QPointF position) override;
    void pointerMoved(QPointF position) override;
    void pointerReleased(QPointF position) override;
    void doubleTapped(QPointF position) override;

  private:
    double parameter() const;
    void setParameter(double parameter);

    ControlProxy* m_pControl;
    const double m_min;
    const double m_max;
    const QString m_label;
    QColor m_accent;
    bool m_bipolar = false;
    bool m_enabled = true;
    QList<double> m_steps;
    std::function<QString(double)> m_formatter;
    ValuePainter m_valuePainter;
    QPointF m_pressPosition;
    double m_pressParameter = 0;
};

/// A vertical peak meter fed by polling (call poll() from a timer).
class SpatialMeter : public QWidget {
    Q_OBJECT
  public:
    SpatialMeter(const QString& group, const QString& key, QWidget* pParent = nullptr);
    void poll();
    QSize sizeHint() const override {
        return {8, 120};
    }

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    ControlProxy* m_pControl;
    double m_value = 0;
};

/// Icons for shapes and waves, shared by the auto-pan and delay panels.
namespace spatialicons {
void paintShape(QPainter& p, const QRectF& r, const QColor& color, int shape);
void paintWave(QPainter& p, const QRectF& r, const QColor& color, int wave);
} // namespace spatialicons
