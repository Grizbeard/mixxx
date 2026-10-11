#include "spatial/spatialwidgets.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointingDevice>
#include <QTouchEvent>
#include <algorithm>
#include <cmath>

#include "control/controlproxy.h"
#include "engine/spatial/spatialmath.h"
#include "moc_spatialwidgets.cpp"
#include "spatial/spatialstyle.h"

using namespace spatialstyle;

namespace {

constexpr int kDoubleTapMs = 350;
constexpr double kDoubleTapDistance = 30;

bool fromTouchScreen(const QMouseEvent* pEvent) {
    const QPointingDevice* pDevice = pEvent->pointingDevice();
    return pDevice && pDevice->type() == QInputDevice::DeviceType::TouchScreen;
}

ControlProxy* makeProxy(const QString& group, const QString& key, QObject* pParent) {
    return new ControlProxy(group, key, pParent, ControlFlag::NoAssertIfMissing);
}

} // namespace

// ---------------------------------------------------------------------------

SpatialTouchWidget::SpatialTouchWidget(QWidget* pParent)
        : QWidget(pParent) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void SpatialTouchWidget::press(QPointF position) {
    if (m_lastTap.isValid() && m_lastTap.elapsed() < kDoubleTapMs &&
            QLineF(position, m_lastTapPosition).length() < kDoubleTapDistance) {
        m_lastTap.invalidate();
        doubleTapped(position);
    } else {
        m_lastTap.start();
        m_lastTapPosition = position;
    }
    m_pressed = true;
    pointerPressed(position);
}

bool SpatialTouchWidget::event(QEvent* pEvent) {
    switch (pEvent->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        auto* pTouch = static_cast<QTouchEvent*>(pEvent);
        for (const QEventPoint& point : pTouch->points()) {
            if (m_touchId < 0 && point.state() == QEventPoint::Pressed) {
                m_touchId = point.id();
                press(point.position());
            } else if (point.id() == m_touchId) {
                if (point.state() == QEventPoint::Released ||
                        pEvent->type() == QEvent::TouchCancel) {
                    m_touchId = -1;
                    m_pressed = false;
                    pointerReleased(point.position());
                } else if (point.state() == QEventPoint::Updated) {
                    pointerMoved(point.position());
                }
            }
        }
        if (pEvent->type() == QEvent::TouchCancel && m_touchId >= 0) {
            m_touchId = -1;
            m_pressed = false;
            pointerReleased({});
        }
        pEvent->accept();
        return true;
    }
    default:
        return QWidget::event(pEvent);
    }
}

void SpatialTouchWidget::mousePressEvent(QMouseEvent* pEvent) {
    if (fromTouchScreen(pEvent) || m_touchId >= 0 || pEvent->button() != Qt::LeftButton) {
        return;
    }
    press(pEvent->position());
}

void SpatialTouchWidget::mouseDoubleClickEvent(QMouseEvent* pEvent) {
    // Qt replaces the second press of a double click with this event.
    mousePressEvent(pEvent);
}

void SpatialTouchWidget::mouseMoveEvent(QMouseEvent* pEvent) {
    if (fromTouchScreen(pEvent) || m_touchId >= 0 || !m_pressed) {
        return;
    }
    pointerMoved(pEvent->position());
}

void SpatialTouchWidget::mouseReleaseEvent(QMouseEvent* pEvent) {
    if (fromTouchScreen(pEvent) || m_touchId >= 0 || !m_pressed ||
            pEvent->button() != Qt::LeftButton) {
        return;
    }
    m_pressed = false;
    pointerReleased(pEvent->position());
}

// ---------------------------------------------------------------------------

SpatialSlider::SpatialSlider(const QString& group,
        const QString& key,
        double min,
        double max,
        const QString& label,
        Qt::Orientation orientation,
        QWidget* pParent)
        : SpatialTouchWidget(pParent),
          m_pControl(makeProxy(group, key, this)),
          m_min(min),
          m_max(max),
          m_label(label),
          m_orientation(orientation),
          m_accent(kGain) {
    m_pControl->connectValueChanged(this, [this](double) { update(); });
    if (orientation == Qt::Horizontal) {
        setMinimumHeight(42);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    } else {
        setMinimumSize(56, 110);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }
}

void SpatialSlider::setAccent(const QColor& accent) {
    m_accent = accent;
    update();
}

void SpatialSlider::setCurve(Curve curve) {
    m_curve = curve;
    update();
}

void SpatialSlider::setBipolar(bool bipolar) {
    m_bipolar = bipolar;
    update();
}

void SpatialSlider::setFormatter(std::function<QString(double)> formatter) {
    m_formatter = std::move(formatter);
    update();
}

void SpatialSlider::addMirror(const QString& group, const QString& key) {
    m_mirrors.append(makeProxy(group, key, this));
}

void SpatialSlider::setControlEnabled(bool enabled) {
    if (m_enabled != enabled) {
        m_enabled = enabled;
        update();
    }
}

QSize SpatialSlider::sizeHint() const {
    return m_orientation == Qt::Horizontal ? QSize(160, 46) : QSize(56, 200);
}

double SpatialSlider::parameter() const {
    const double value = std::clamp(m_pControl->get(), m_min, m_max);
    if (m_curve == Curve::Log && m_min > 0) {
        return std::log(value / m_min) / std::log(m_max / m_min);
    }
    return (value - m_min) / (m_max - m_min);
}

void SpatialSlider::setParameter(double parameter) {
    parameter = std::clamp(parameter, 0.0, 1.0);
    double value;
    if (m_curve == Curve::Log && m_min > 0) {
        value = m_min * std::pow(m_max / m_min, parameter);
    } else {
        value = m_min + (m_max - m_min) * parameter;
    }
    m_pControl->set(value);
    for (ControlProxy* pMirror : std::as_const(m_mirrors)) {
        pMirror->set(value);
    }
    update();
}

QRectF SpatialSlider::trackRect() const {
    if (m_orientation == Qt::Horizontal) {
        return QRectF(8, height() - 20, width() - 16, 10);
    }
    return QRectF(width() / 2.0 - 5, 26, 10, height() - 52);
}

double SpatialSlider::travel() const {
    const QRectF track = trackRect();
    return std::max(1.0, m_orientation == Qt::Horizontal ? track.width() : track.height());
}

void SpatialSlider::pointerPressed(QPointF position) {
    m_pressPosition = position;
    m_pressParameter = parameter();
    update();
}

void SpatialSlider::pointerMoved(QPointF position) {
    if (!m_enabled) {
        return;
    }
    const double delta = m_orientation == Qt::Horizontal
            ? (position.x() - m_pressPosition.x())
            : (m_pressPosition.y() - position.y());
    setParameter(m_pressParameter + delta / travel());
}

void SpatialSlider::pointerReleased(QPointF position) {
    Q_UNUSED(position);
    update();
}

void SpatialSlider::doubleTapped(QPointF position) {
    Q_UNUSED(position);
    if (!m_enabled) {
        return;
    }
    m_pControl->reset();
    for (ControlProxy* pMirror : std::as_const(m_mirrors)) {
        pMirror->set(m_pControl->getDefault());
    }
    m_pressParameter = parameter();
    update();
}

void SpatialSlider::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double param = parameter();
    const QColor accent = m_enabled ? m_accent : kDisabled;
    const QColor labelColor = m_enabled ? kLabel : kDisabled;
    const QColor valueColor = m_enabled ? kText : kDisabled;
    const QString valueText = m_formatter ? m_formatter(m_pControl->get())
                                          : QString::number(m_pControl->get(), 'f', 2);
    const QRectF track = trackRect();

    // Well
    p.setPen(QPen(kBorderDark, 1));
    p.setBrush(kBar);
    p.drawRoundedRect(track, 2, 2);

    p.setFont(styleFont(11, true));
    if (m_orientation == Qt::Horizontal) {
        p.setPen(labelColor);
        p.drawText(QRectF(8, 2, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter, m_label);
        p.setPen(valueColor);
        p.setFont(styleFont(12));
        p.drawText(QRectF(8, 2, width() - 16, 18), Qt::AlignRight | Qt::AlignVCenter, valueText);
        const double x = track.left() + param * track.width();
        const double from = m_bipolar ? track.center().x() : track.left();
        QRectF fill(std::min(from, x), track.top() + 2, std::abs(x - from), track.height() - 4);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRoundedRect(fill, 1, 1);
        // Handle
        const QRectF handle(x - 3, track.center().y() - 10, 6, 20);
        p.setPen(QPen(kBorderDark, 1));
        p.setBrush(m_pressed ? kText : QColor(0xb3, 0xb3, 0xb3));
        p.drawRoundedRect(handle, 1.5, 1.5);
    } else {
        p.setPen(labelColor);
        p.drawText(QRectF(0, 2, width(), 20), Qt::AlignHCenter | Qt::AlignVCenter, m_label);
        p.setPen(valueColor);
        p.setFont(styleFont(12));
        p.drawText(QRectF(0, height() - 22, width(), 20), Qt::AlignHCenter | Qt::AlignVCenter,
                valueText);
        const double y = track.bottom() - param * track.height();
        const double from = m_bipolar ? track.center().y() : track.bottom();
        QRectF fill(track.left() + 2, std::min(from, y), track.width() - 4, std::abs(y - from));
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRoundedRect(fill, 1, 1);
        // Fader cap, like LateNight's volume fader
        const QRectF cap(width() / 2.0 - 20, y - 9, 40, 18);
        QLinearGradient gradient(cap.topLeft(), cap.bottomLeft());
        gradient.setColorAt(0, QColor(0x58, 0x58, 0x58));
        gradient.setColorAt(1, QColor(0x29, 0x29, 0x29));
        p.setPen(QPen(kBorderDark, 1));
        p.setBrush(gradient);
        p.drawRoundedRect(cap, 2, 2);
        p.setPen(QPen(m_pressed ? kText : QColor(0xd2, 0xd2, 0xd2), 2));
        p.drawLine(QPointF(cap.left() + 4, y), QPointF(cap.right() - 4, y));
    }
}

// ---------------------------------------------------------------------------

SpatialButton::SpatialButton(const QString& group,
        const QString& key,
        Mode mode,
        const QString& text,
        QWidget* pParent)
        : SpatialTouchWidget(pParent),
          m_pControl(makeProxy(group, key, this)),
          m_mode(mode),
          m_text(text),
          m_accent(kHotOrange) {
    m_pControl->connectValueChanged(this, [this](double) { update(); });
    setMinimumSize(44, 40);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void SpatialButton::setAccent(const QColor& accent) {
    m_accent = accent;
    update();
}

void SpatialButton::setIcon(IconPainter painter) {
    m_icon = std::move(painter);
    update();
}

void SpatialButton::setFontSize(int pixels) {
    m_fontSize = pixels;
    update();
}

void SpatialButton::setControlEnabled(bool enabled) {
    if (m_enabled != enabled) {
        m_enabled = enabled;
        update();
    }
}

void SpatialButton::addMirror(const QString& group, const QString& key) {
    m_mirrors.append(makeProxy(group, key, this));
}

QSize SpatialButton::sizeHint() const {
    return {64, 44};
}

bool SpatialButton::isActive() const {
    switch (m_mode) {
    case Mode::Toggle:
        return m_pControl->get() > 0;
    case Mode::Momentary:
        return m_pressed || m_pControl->get() > 0;
    case Mode::Select:
        return std::abs(m_pControl->get() - m_value) < 1e-6 * std::max(1.0, std::abs(m_value));
    }
    return false;
}

void SpatialButton::write(double value) {
    m_pControl->set(value);
    for (ControlProxy* pMirror : std::as_const(m_mirrors)) {
        pMirror->set(value);
    }
    update();
}

void SpatialButton::pointerPressed(QPointF position) {
    Q_UNUSED(position);
    if (!m_enabled) {
        return;
    }
    switch (m_mode) {
    case Mode::Toggle:
        write(m_pControl->get() > 0 ? 0 : 1);
        break;
    case Mode::Momentary:
        write(1);
        break;
    case Mode::Select:
        write(m_value);
        break;
    }
}

void SpatialButton::pointerMoved(QPointF position) {
    Q_UNUSED(position);
}

void SpatialButton::pointerReleased(QPointF position) {
    Q_UNUSED(position);
    if (m_mode == Mode::Momentary && m_enabled) {
        write(0);
    }
    update();
}

void SpatialButton::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool active = isActive() && m_enabled;
    paintButton(p, rect().adjusted(1, 1, -1, -1), active, m_pressed && m_enabled, m_accent);
    const QColor ink = !m_enabled ? kDisabled : (active ? QColor(Qt::black) : kLabel);
    if (m_icon) {
        // Up to twice as wide as tall: shapes keep square, waves stretch.
        const double h = height() - 12;
        const double w = std::min(width() - 16.0, 2 * h);
        const QRectF iconRect(width() / 2.0 - w / 2, height() / 2.0 - h / 2, w, h);
        m_icon(p, iconRect, active ? QColor(Qt::black) : (m_enabled ? kTextDim : kDisabled));
    }
    if (!m_text.isEmpty()) {
        p.setPen(ink);
        p.setFont(styleFont(m_fontSize, true));
        p.drawText(rect(), Qt::AlignCenter, m_text);
    }
}

// ---------------------------------------------------------------------------

SpatialMeter::SpatialMeter(const QString& group, const QString& key, QWidget* pParent)
        : QWidget(pParent),
          m_pControl(makeProxy(group, key, this)) {
    setMinimumWidth(8);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void SpatialMeter::poll() {
    const double value = m_pControl->get();
    if (std::abs(value - m_value) > 1e-3) {
        m_value = value;
        update();
    }
}

void SpatialMeter::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter p(this);
    const QRectF well = rect().adjusted(0, 0, -1, -1);
    p.setPen(QPen(kBorderDark, 1));
    p.setBrush(kBar);
    p.drawRect(well);
    // dBFS scale: -48 .. 0
    const double db = m_value > 0 ? 20 * std::log10(m_value) : -96;
    const double level = std::clamp((db + 48) / 48, 0.0, 1.0);
    const QRectF fill(1, well.bottom() - level * (well.height() - 1), well.width() - 1,
            level * (well.height() - 1));
    QLinearGradient gradient(well.bottomLeft(), well.topLeft());
    gradient.setColorAt(0, kGreen);
    gradient.setColorAt(0.75, QColor(0xd0, 0x93, 0x00));
    gradient.setColorAt(1, QColor(0xa8, 0x00, 0x00));
    p.setPen(Qt::NoPen);
    p.setBrush(gradient);
    p.drawRect(fill);
}

// ---------------------------------------------------------------------------

namespace spatialicons {

void paintShape(QPainter& p, const QRectF& r, const QColor& color, int shape) {
    using namespace mixxx::spatial;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF centre = r.center();
    const double radius = std::min(r.width(), r.height()) / 2 - 2;
    const auto toScreen = [&](Point point) {
        return QPointF(centre.x() + point.x * radius, centre.y() - point.y * radius);
    };
    QColor faint = color;
    faint.setAlphaF(0.25);
    p.setPen(QPen(faint, 1));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(centre, radius, radius);
    const auto s = static_cast<Shape>(shape);
    if (s == Shape::Random) {
        // Random jumps across the ring between scattered points.
        QPainterPath jumps;
        for (int k = 0; k < 6; ++k) {
            const Point q = pointAtAzimuth(randomAzimuth(k + 3));
            if (k == 0) {
                jumps.moveTo(toScreen(q));
            } else {
                jumps.lineTo(toScreen(q));
            }
        }
        p.setPen(QPen(color, 1.4, Qt::DashLine));
        p.drawPath(jumps);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        for (int k = 0; k < 6; ++k) {
            p.drawEllipse(toScreen(pointAtAzimuth(randomAzimuth(k + 3))), 2.6, 2.6);
        }
        p.restore();
        return;
    }
    const int points = shapePointCount(s);
    QPainterPath path;
    constexpr int kSamples = 160;
    for (int i = 0; i <= kSamples; ++i) {
        const QPointF q = toScreen(shapeAt(s, static_cast<double>(i) * points / kSamples));
        if (i == 0) {
            path.moveTo(q);
        } else {
            path.lineTo(q);
        }
    }
    p.setPen(QPen(color, 1.6));
    p.drawPath(path);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    for (int k = 0; k < points; ++k) {
        p.drawEllipse(toScreen(shapeAt(s, k)), 2.2, 2.2);
    }
    p.restore();
}

void paintWave(QPainter& p, const QRectF& r, const QColor& color, int wave) {
    using namespace mixxx::spatial;
    // The classic oscillator symbol for each wave, two cycles wide.
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF box = r.adjusted(4, 8, -4, -8);
    const double top = box.top();
    const double bottom = box.bottom();
    const double middle = box.center().y();
    const double cycle = box.width() / 2;
    const auto x = [&](double c) {
        return box.left() + c * cycle;
    };
    QPainterPath path;
    switch (static_cast<Wave>(wave)) {
    case Wave::Square:
        path.moveTo(x(0), middle);
        path.lineTo(x(0), top);
        for (int c = 0; c < 2; ++c) {
            path.lineTo(x(c + 0.5), top);
            path.lineTo(x(c + 0.5), bottom);
            path.lineTo(x(c + 1), bottom);
            path.lineTo(x(c + 1), c == 1 ? middle : top);
        }
        break;
    case Wave::Sine: {
        constexpr int kSamples = 64;
        for (int i = 0; i <= kSamples; ++i) {
            const double c = 2.0 * i / kSamples;
            const QPointF q(x(c), middle - std::sin(2 * kPi * c) * (middle - top));
            if (i == 0) {
                path.moveTo(q);
            } else {
                path.lineTo(q);
            }
        }
        break;
    }
    case Wave::Triangle:
        path.moveTo(x(0), middle);
        for (int c = 0; c < 2; ++c) {
            path.lineTo(x(c + 0.25), top);
            path.lineTo(x(c + 0.75), bottom);
            path.lineTo(x(c + 1), middle);
        }
        break;
    case Wave::Saw:
        // Ramp up, then drop.
        path.moveTo(x(0), middle);
        path.lineTo(x(0.5), top);
        path.lineTo(x(0.5), bottom);
        path.lineTo(x(1.5), top);
        path.lineTo(x(1.5), bottom);
        path.lineTo(x(2), middle);
        break;
    case Wave::ReverseSaw:
        // Jump up, then ramp down.
        path.moveTo(x(0), middle);
        path.lineTo(x(0.5), bottom);
        path.lineTo(x(0.5), top);
        path.lineTo(x(1.5), bottom);
        path.lineTo(x(1.5), top);
        path.lineTo(x(2), middle);
        break;
    }
    // A faint centre line, like an oscilloscope's axis.
    QColor axis = color;
    axis.setAlphaF(0.25);
    p.setPen(QPen(axis, 1));
    p.drawLine(QPointF(box.left(), middle), QPointF(box.right(), middle));
    p.setPen(QPen(color, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.restore();
}

} // namespace spatialicons
