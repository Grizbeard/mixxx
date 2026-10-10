#include "spatial/spatialfield.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointingDevice>
#include <QTouchEvent>
#include <algorithm>
#include <cmath>

#include "control/controlproxy.h"
#include "engine/spatial/enginespatialoutput.h"
#include "engine/spatial/spatialmath.h"
#include "moc_spatialfield.cpp"
#include "spatial/spatialstyle.h"

using namespace spatialstyle;
using namespace mixxx::spatial;

namespace {

constexpr double kDotRadius = 19;
constexpr double kGrabRadius = 46; // generous touch target
constexpr int kMouseId = -2;
constexpr qint64 kDoubleTapMs = 350;

const char* const kSlotLetters[] = {"D", "I", "V"};

std::unique_ptr<PollingControlProxy> makePoll(const QString& group, const QString& key) {
    return std::make_unique<PollingControlProxy>(group, key, ControlFlag::AllowMissingOrInvalid);
}

QColor defaultStemColor(int slot) {
    switch (slot) {
    case 0:
        return kDrums;
    case 1:
        return kInstruments;
    default:
        return kVocals;
    }
}

bool fromTouchScreen(const QMouseEvent* pEvent) {
    const QPointingDevice* pDevice = pEvent->pointingDevice();
    return pDevice && pDevice->type() == QInputDevice::DeviceType::TouchScreen;
}

} // namespace

SpatialField::SpatialField(int decks, QWidget* pParent)
        : QWidget(pParent),
          m_decks(decks) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(360, 360);

    const QString spatial = QStringLiteral("[Spatial]");
    m_pSpeakerCount = makePoll(spatial, "speaker_count");
    m_pLayoutCustom = makePoll(spatial, "layout_custom");
    m_pChannelCount = makePoll(spatial, "channel_count");
    m_pLfEnabled = makePoll(spatial, "lf_enabled");
    for (int i = 0; i < 6; ++i) {
        m_pSpeakerAzimuth[i] = makePoll(spatial, QStringLiteral("speaker_%1_azimuth").arg(i + 1));
    }
    for (int c = 0; c < 8; ++c) {
        m_pVu[c] = makePoll(spatial, QStringLiteral("vu_meter_%1").arg(c + 1));
    }
    for (int u = 0; u < 2; ++u) {
        const QString group = QStringLiteral("[SpatialAutopan%1]").arg(u + 1);
        m_pAutopanShape[u] = makePoll(group, "shape");
        m_pAutopanSize[u] = makePoll(group, "size");
        m_pAutopanPosition[u] = makePoll(group, "position");
    }
    m_pDelayPosition = makePoll("[SpatialDelay]", "position");
    m_pDelayShape = makePoll("[SpatialDelay]", "shape");
    m_pDelaySize = makePoll("[SpatialDelay]", "size");
    m_pDelayEnabled = makePoll("[SpatialDelay]", "enabled");

    for (int deck = 0; deck < decks; ++deck) {
        m_pStemCount.push_back(makePoll(QStringLiteral("[Channel%1]").arg(deck + 1), "stem_count"));
        for (int slot = 0; slot < EngineSpatialOutput::kSlots; ++slot) {
            const QString group = EngineSpatialOutput::sourceGroup(deck, slot);
            Source source;
            source.deck = deck;
            source.slot = slot;
            source.pX = makePoll(group, "spatial_x");
            source.pY = makePoll(group, "spatial_y");
            source.pLevel = makePoll(group, "spatial_level");
            source.pActive = makePoll(group, "spatial_active");
            source.pAzimuth = makePoll(group, "spatial_azimuth");
            source.pWidth = makePoll(group, "spatial_width");
            source.pSpread = makePoll(group, "spatial_spread");
            source.pAutopan = makePoll(group, "spatial_autopan");
            source.pUnit = makePoll(group, "spatial_autopan_unit");
            source.pFront = makePoll(group, "spatial_front");
            source.pFrontBlend = makePoll(group, "spatial_front_blend");
            source.pColor = makePoll(group, "color");
            source.pSend = makePoll(group, "spatial_fx_send");
            source.pSetAzimuth = new ControlProxy(group, "spatial_azimuth", this,
                    ControlFlag::NoAssertIfMissing);
            source.pSetSpread = new ControlProxy(group, "spatial_spread", this,
                    ControlFlag::NoAssertIfMissing);
            source.pSetFront = new ControlProxy(group, "spatial_front", this,
                    ControlFlag::NoAssertIfMissing);
            source.color = defaultStemColor(slot);
            m_sources.push_back(std::move(source));
        }
    }
    poll();
}

SpatialField::~SpatialField() = default;

double SpatialField::ringRadius() const {
    return std::min(width(), height()) / 2.0 - 46;
}

QPointF SpatialField::toScreen(QPointF unit) const {
    const double r = ringRadius();
    return {width() / 2.0 + unit.x() * r, height() / 2.0 - unit.y() * r};
}

QPointF SpatialField::toUnit(QPointF screen) const {
    const double r = std::max(1.0, ringRadius());
    return {(screen.x() - width() / 2.0) / r, (height() / 2.0 - screen.y()) / r};
}

void SpatialField::poll() {
    QByteArray state;
    QDataStream out(&state, QIODevice::WriteOnly);

    m_speakers = std::clamp(static_cast<int>(std::lround(m_pSpeakerCount->get())), 4, 6);
    m_channels = static_cast<int>(m_pChannelCount->get());
    if (m_channels > 0) {
        m_speakers = std::min(m_speakers, m_channels);
    }
    m_lf = m_pLfEnabled->get() > 0 && m_speakers < m_channels;
    const auto defaults = SpeakerLayout::defaultAzimuths(m_speakers);
    const bool custom = m_pLayoutCustom->get() > 0;
    for (int i = 0; i < m_speakers; ++i) {
        m_azimuths[i] = custom ? m_pSpeakerAzimuth[i]->get() : defaults[i];
        out << m_azimuths[i];
    }
    for (int c = 0; c < 8; ++c) {
        // Quantised so idle meters don't repaint.
        m_vu[c] = std::round(m_pVu[c]->get() * 64) / 64;
        out << m_vu[c];
    }
    out << m_speakers << m_lf;

    for (Source& source : m_sources) {
        const bool stems = m_pStemCount[source.deck]->get() > 0;
        source.active = source.pActive->get() > 0 && (stems || source.slot == 1);
        source.azimuth = source.pAzimuth->get();
        source.width = source.pWidth->get();
        source.autopan = source.pAutopan->get() > 0;
        source.unit = std::clamp(static_cast<int>(std::lround(source.pUnit->get())), 1, 2);
        source.front = source.pFront->get() > 0;
        source.level = std::round(source.pLevel->get() * 32) / 32;
        if (source.active) {
            source.position = QPointF(source.pX->get(), source.pY->get());
        } else {
            const Point p = pointAtAzimuth(source.azimuth, 1 - source.pSpread->get());
            source.position = QPointF(p.x, p.y);
        }
        const double color = source.pColor->get();
        source.color = color > 0 && stems ? QColor::fromRgb(static_cast<QRgb>(color))
                                           : defaultStemColor(source.slot);
        out << source.active << source.position << source.width << source.autopan
            << source.unit << source.front << source.level << source.color;
    }

    bool echo = false;
    if (m_pDelayEnabled->get() > 0) {
        for (const Source& source : m_sources) {
            echo = echo || (source.active && source.pSend->get() > 0);
        }
    }
    m_echoVisible = echo;
    m_delayPosition = m_pDelayPosition->get();
    out << echo << (echo ? m_delayPosition : 0.0);
    for (int u = 0; u < 2; ++u) {
        out << m_pAutopanShape[u]->get() << m_pAutopanSize[u]->get();
    }

    if (state != m_lastState) {
        m_lastState = state;
        update();
    }
}

int SpatialField::sourceAt(QPointF screen) const {
    int best = -1;
    double bestDistance = kGrabRadius;
    for (int i = 0; i < static_cast<int>(m_sources.size()); ++i) {
        const Source& source = m_sources[i];
        if (!source.active && m_pStemCount[source.deck]->get() <= 0 && source.slot != 1) {
            continue;
        }
        bool taken = false;
        for (const Grab& g : m_grabs) {
            taken = taken || g.source == i;
        }
        if (taken) {
            continue;
        }
        const double distance = QLineF(screen, toScreen(source.position)).length();
        if (distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

void SpatialField::grab(int id, QPointF screen) {
    const int index = sourceAt(screen);
    if (index < 0) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (index == m_lastTapSource && now - m_lastTapMs < kDoubleTapMs) {
        toggleFront(index);
        m_lastTapSource = -1;
        return;
    }
    m_lastTapSource = index;
    m_lastTapMs = now;
    m_grabs.insert(id, Grab{index, toScreen(m_sources[index].position) - screen});
    m_selected = index;
    emit sourceTouched(m_sources[index].deck, m_sources[index].slot);
    update();
}

void SpatialField::drag(int id, QPointF screen) {
    const auto it = m_grabs.constFind(id);
    if (it == m_grabs.constEnd()) {
        return;
    }
    Source& source = m_sources[it->source];
    const QPointF unit = toUnit(screen + it->offset);
    const Point p{unit.x(), unit.y()};
    const double radius = std::min(1.0, radiusOf(p));
    if (source.front) {
        source.pSetFront->set(0);
    }
    if (radius > 0.02) {
        source.pSetAzimuth->set(azimuthOf(p));
    }
    // With auto-pan the drag turns the shape; otherwise distance from the
    // centre sets the spread.
    if (!source.autopan) {
        source.pSetSpread->set(1 - radius);
    }
}

void SpatialField::release(int id) {
    m_grabs.remove(id);
    update();
}

void SpatialField::toggleFront(int index) {
    Source& source = m_sources[index];
    source.pSetFront->set(source.front ? 0 : 1);
}

bool SpatialField::event(QEvent* pEvent) {
    switch (pEvent->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        auto* pTouch = static_cast<QTouchEvent*>(pEvent);
        for (const QEventPoint& point : pTouch->points()) {
            switch (point.state()) {
            case QEventPoint::Pressed:
                grab(point.id(), point.position());
                break;
            case QEventPoint::Updated:
                drag(point.id(), point.position());
                break;
            case QEventPoint::Released:
                release(point.id());
                break;
            default:
                break;
            }
        }
        if (pEvent->type() == QEvent::TouchCancel || pEvent->type() == QEvent::TouchEnd) {
            m_grabs.clear();
        }
        pEvent->accept();
        return true;
    }
    default:
        return QWidget::event(pEvent);
    }
}

void SpatialField::mousePressEvent(QMouseEvent* pEvent) {
    if (!fromTouchScreen(pEvent) && pEvent->button() == Qt::LeftButton) {
        grab(kMouseId, pEvent->position());
    }
}

void SpatialField::mouseDoubleClickEvent(QMouseEvent* pEvent) {
    mousePressEvent(pEvent);
}

void SpatialField::mouseMoveEvent(QMouseEvent* pEvent) {
    if (!fromTouchScreen(pEvent)) {
        drag(kMouseId, pEvent->position());
    }
}

void SpatialField::mouseReleaseEvent(QMouseEvent* pEvent) {
    if (!fromTouchScreen(pEvent)) {
        release(kMouseId);
    }
}

void SpatialField::paintSpeakers(QPainter& p) {
    for (int i = 0; i < m_speakers; ++i) {
        const Point unit = pointAtAzimuth(m_azimuths[i], 1.0);
        const QPointF centre = toScreen(QPointF(unit.x, unit.y) * 1.08);
        const double level = std::clamp(m_vu[i], 0.0, 1.0);
        if (level > 0.01) {
            QRadialGradient glow(centre, 34);
            QColor hot = kHotOrange;
            hot.setAlphaF(std::min(0.85, 0.25 + level));
            glow.setColorAt(0, hot);
            glow.setColorAt(1, QColor(0, 0, 0, 0));
            p.setPen(Qt::NoPen);
            p.setBrush(glow);
            p.drawEllipse(centre, 34, 34);
        }
        p.save();
        p.translate(centre);
        p.rotate(m_azimuths[i]);
        const QRectF box(-15, -13, 30, 26);
        paintButton(p, box, false, false, kHotOrange);
        p.setPen(QPen(level > 0.01 ? kHotOrange : QColor(0x55, 0x55, 0x55), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(0, 1), 7, 7);
        p.restore();
        p.setPen(kText);
        p.setFont(styleFont(12, true));
        const QPointF labelAt = toScreen(QPointF(unit.x, unit.y) * 1.08 + QPointF(0, 0)) +
                QPointF(unit.x * 26, -unit.y * 26);
        p.drawText(QRectF(labelAt - QPointF(12, 9), QSizeF(24, 18)), Qt::AlignCenter,
                QString::number(i + 1));
    }
    if (m_lf) {
        // The LF feed: a meter in the corner.
        const QRectF box(width() - 70, height() - 44, 60, 32);
        paintContainer(p, box, kDim);
        p.setPen(kLabel);
        p.setFont(styleFont(11, true));
        p.drawText(box.adjusted(6, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("LF"));
        const double level = std::clamp(m_vu[m_speakers], 0.0, 1.0);
        const QRectF well(box.left() + 26, box.top() + 11, 28, 10);
        p.setPen(Qt::NoPen);
        p.setBrush(kBar);
        p.drawRect(well);
        p.setBrush(kHotOrange);
        p.drawRect(QRectF(well.left(), well.top(), well.width() * level, well.height()));
    }
}

void SpatialField::paintPaths(QPainter& p) {
    // Each auto-pan unit's shape, as each enrolled stem sees it (rotated by
    // its pan), drawn once per distinct rotation.
    QList<QPair<int, int>> drawn;
    for (const Source& source : m_sources) {
        if (!source.autopan || !source.active) {
            continue;
        }
        const int rotation = static_cast<int>(std::lround(source.azimuth));
        const QPair<int, int> key(source.unit, rotation);
        if (drawn.contains(key)) {
            continue;
        }
        drawn.append(key);
        const auto shape = static_cast<Shape>(std::clamp(
                static_cast<int>(std::lround(m_pAutopanShape[source.unit - 1]->get())), 0, 5));
        const double size = m_pAutopanSize[source.unit - 1]->get();
        const int points = shapePointCount(shape);
        QPainterPath path;
        constexpr int kSamples = 240;
        for (int s = 0; s <= kSamples; ++s) {
            Point q = shapeAt(shape, static_cast<double>(s) * points / kSamples);
            q = rotate({q.x * size, q.y * size}, source.azimuth);
            const QPointF screen = toScreen(QPointF(q.x, q.y));
            if (s == 0) {
                path.moveTo(screen);
            } else {
                path.lineTo(screen);
            }
        }
        QColor color = deckColor(source.deck + 1);
        color.setAlphaF(0.35);
        p.setPen(QPen(color, 1.5, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        for (int k = 0; k < points; ++k) {
            Point q = shapeAt(shape, k);
            q = rotate({q.x * size, q.y * size}, source.azimuth);
            p.drawEllipse(toScreen(QPointF(q.x, q.y)), 3, 3);
        }
    }
    if (m_echoVisible) {
        const auto shape = static_cast<Shape>(
                std::clamp(static_cast<int>(std::lround(m_pDelayShape->get())), 0, 5));
        const double size = m_pDelaySize->get();
        const Point q = shapeAt(shape, m_delayPosition);
        const QPointF at = toScreen(QPointF(q.x * size, q.y * size));
        p.setPen(QPen(kPlayPos, 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(at, 12, 12);
        p.setFont(styleFont(9, true));
        p.setPen(kPlayPos);
        p.drawText(QRectF(at.x() - 20, at.y() + 13, 40, 12), Qt::AlignCenter,
                QStringLiteral("ECHO"));
    }
}

void SpatialField::paintSource(QPainter& p, const Source& source, bool selected) {
    const QPointF centre = toScreen(source.position);
    const QColor deck = deckColor(source.deck + 1);
    const double alpha = source.active ? 1.0 : 0.35;

    // Width: the stem's L and R either side of the centre, on its arc.
    const Point unit{source.position.x(), source.position.y()};
    const double radius = radiusOf(unit);
    if (source.active && radius > 0.08 && source.width > 0.01) {
        const double az = azimuthOf(unit);
        const double half = source.width * 90;
        const double r = radius * ringRadius();
        const QRectF arcBox(width() / 2.0 - r, height() / 2.0 - r, 2 * r, 2 * r);
        QColor arc = source.color;
        arc.setAlphaF(0.55);
        p.setPen(QPen(arc, 4, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        // Qt angles: counter-clockwise from 3 o'clock, in 1/16 degrees.
        const double startQt = 90 - (az + half);
        p.drawArc(arcBox, static_cast<int>(startQt * 16), static_cast<int>(2 * half * 16));
        p.setFont(styleFont(9, true));
        p.setPen(arc);
        for (const auto& [side, offset] : {std::pair{"L", -half}, std::pair{"R", half}}) {
            const Point tick = pointAtAzimuth(az + offset, radius);
            const QPointF at = toScreen(QPointF(tick.x, tick.y));
            p.drawText(QRectF(at.x() - 8, at.y() - 7, 16, 14), Qt::AlignCenter, side);
        }
    }

    // Level glow
    if (source.active && source.level > 0.01) {
        QRadialGradient glow(centre, kDotRadius * 2.2);
        QColor c = source.color;
        c.setAlphaF(std::min(0.7, source.level));
        glow.setColorAt(0, c);
        glow.setColorAt(1, QColor(0, 0, 0, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(centre, kDotRadius * 2.2, kDotRadius * 2.2);
    }

    QColor fill = source.color;
    fill.setAlphaF(source.active ? 0.95 : 0.0);
    QColor ring = deck;
    ring.setAlphaF(alpha);
    p.setPen(QPen(ring, selected ? 4 : 2.5));
    p.setBrush(fill);
    p.drawEllipse(centre, kDotRadius, kDotRadius);

    p.setFont(styleFont(15, true));
    p.setPen(source.active ? QColor(Qt::black) : ring);
    p.drawText(QRectF(centre.x() - kDotRadius, centre.y() - kDotRadius, 2 * kDotRadius,
                       2 * kDotRadius),
            Qt::AlignCenter,
            kSlotLetters[source.slot]);
    // Deck number badge
    p.setFont(styleFont(10, true));
    p.setPen(ring);
    p.drawText(QRectF(centre.x() + kDotRadius * 0.6, centre.y() - kDotRadius * 1.5, 14, 12),
            Qt::AlignCenter,
            QString::number(source.deck + 1));
    if (source.front) {
        p.setPen(kHotOrange);
        p.drawText(QRectF(centre.x() - kDotRadius * 1.3, centre.y() - kDotRadius * 1.5, 14, 12),
                Qt::AlignCenter,
                QStringLiteral("F"));
    }
    if (source.autopan) {
        p.setPen(QPen(ring, 1.2, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(centre, kDotRadius + 5, kDotRadius + 5);
    }
}

void SpatialField::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), kWindow);
    paintContainer(p, QRectF(rect()).adjusted(1, 1, -1, -1), kDim);

    const QPointF centre(width() / 2.0, height() / 2.0);
    const double r = ringRadius();
    // The floor
    QRadialGradient floor(centre, r);
    floor.setColorAt(0, QColor(0x1e, 0x1e, 0x20));
    floor.setColorAt(1, QColor(0x10, 0x10, 0x11));
    p.setPen(QPen(kBorderLight, 1.5));
    p.setBrush(floor);
    p.drawEllipse(centre, r, r);
    p.setPen(QPen(QColor(0x2a, 0x2a, 0x2a), 1, Qt::DotLine));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(centre, r / 2, r / 2);
    p.drawLine(QPointF(centre.x(), centre.y() - r), QPointF(centre.x(), centre.y() + r));
    p.drawLine(QPointF(centre.x() - r, centre.y()), QPointF(centre.x() + r, centre.y()));
    p.setPen(kLabelDim);
    p.setFont(styleFont(11, true));
    p.drawText(QRectF(centre.x() - 40, centre.y() - r - 42, 80, 16), Qt::AlignCenter,
            QStringLiteral("FRONT"));

    if (m_channels <= 0) {
        p.setPen(kHotOrange);
        p.setFont(styleFont(13, true));
        p.drawText(QRectF(centre.x() - r, centre.y() + r / 2 - 20, 2 * r, 40),
                Qt::AlignCenter | Qt::TextWordWrap,
                tr("Spatial output not assigned (Preferences > Sound Hardware)"));
    }

    paintSpeakers(p);
    paintPaths(p);
    // Inactive first, so playing stems sit on top.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < static_cast<int>(m_sources.size()); ++i) {
            const Source& source = m_sources[i];
            const bool shown = source.active ||
                    (m_pStemCount[source.deck]->get() > 0 || source.slot == 1);
            if (shown && source.active == (pass == 1)) {
                paintSource(p, source, i == m_selected);
            }
        }
    }
}
