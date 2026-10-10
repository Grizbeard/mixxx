#pragma once

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QRectF>

/// The spatial window's look: the LateNight skin's PaleMoon palette
/// (res/skins/LateNight/style_palemoon.qss), sized for a touch screen.
namespace spatialstyle {

// Backgrounds
inline const QColor kWindow{0x08, 0x08, 0x08};      // #SkinContainer
inline const QColor kContainer{0x1e, 0x1e, 0x20};   // decks, mixer
inline const QColor kDim{0x15, 0x15, 0x17};         // toolbar, dim containers
inline const QColor kBar{0x02, 0x02, 0x02};         // slider / meter wells
inline const QColor kEmbedded{0x0f, 0x0f, 0x10};    // embedded buttons
inline const QColor kBorderLight{0x33, 0x33, 0x33}; // container top edge
inline const QColor kBorderDark{0x0c, 0x0c, 0x0c};  // container bottom edge
inline const QColor kButtonEdge{0x2c, 0x2c, 0x2e};

// Text
inline const QColor kText{0xc2, 0xb3, 0xa5};      // ivory
inline const QColor kTextDim{0x98, 0x8f, 0x86};   // dim ivory
inline const QColor kLabel{0x77, 0x77, 0x77};     // deck labels
inline const QColor kLabelDim{0x66, 0x66, 0x66};  // knob labels
inline const QColor kDisabled{0x44, 0x44, 0x43};

// Accents
inline const QColor kDeck12{0xd9, 0xb2, 0x8c};    // deck 1/2 signal (tan)
inline const QColor kDeck34{0x7b, 0xc6, 0xc3};    // deck 3/4 signal (teal)
inline const QColor kOrange{0xb2, 0x4c, 0x12};    // record / analyze
inline const QColor kHotOrange{0xf0, 0x8c, 0x00}; // slip border
inline const QColor kGreen{0x51, 0x8f, 0x00};     // FX 1/2, QuickEffect
inline const QColor kTeal{0x02, 0x83, 0x92};      // FX 3/4
inline const QColor kRed{0xa0, 0x00, 0x00};       // balance, mix
inline const QColor kPlayPos{0x00, 0xc6, 0xff};
inline const QColor kGain{0xb9, 0x63, 0x00};      // gain arc

// Stems (NI stem defaults; the track's own colours are used when known)
inline const QColor kDrums{0xfd, 0x4a, 0x4a};
inline const QColor kInstruments{0xff, 0xff, 0x00};
inline const QColor kVocals{0xad, 0x65, 0xff};

/// Deck identity in the spatial view: odd decks tan, even decks teal, so
/// the two decks' stems tell apart on the ring.
inline QColor deckColor(int deck) {
    return deck % 2 == 1 ? kDeck12 : kDeck34;
}

inline QFont styleFont(int pixelSize, bool semibold = false) {
    QFont f(semibold ? QStringLiteral("Open Sans Semibold") : QStringLiteral("Open Sans"));
    f.setFamilies({semibold ? QStringLiteral("Open Sans Semibold") : QStringLiteral("Open Sans"),
            QStringLiteral("Open Sans")});
    f.setPixelSize(pixelSize);
    if (semibold) {
        f.setWeight(QFont::DemiBold);
    }
    return f;
}

/// A LateNight container panel: flat fill with the lit top/left and dark
/// bottom/right edges.
inline void paintContainer(QPainter& p, const QRectF& r, const QColor& fill = kContainer) {
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(r, 2, 2);
    p.setPen(QPen(kBorderLight, 1));
    p.drawLine(r.topLeft() + QPointF(1, 0.5), r.topRight() + QPointF(-1, 0.5));
    p.setPen(QPen(kBorderDark, 1));
    p.drawLine(r.bottomLeft() + QPointF(1, -0.5), r.bottomRight() + QPointF(-1, -0.5));
    p.restore();
}

/// An embedded LateNight button face; active buttons fill with the accent
/// and take black text, as LateNight's toggles do.
inline void paintButton(QPainter& p, const QRectF& r, bool active, bool pressed, const QColor& accent) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF face = r.adjusted(0.5, 0.5, -0.5, -0.5);
    QColor fill = active ? accent : kEmbedded;
    if (pressed) {
        fill = active ? accent.lighter(115) : QColor(0x24, 0x24, 0x26);
    }
    p.setPen(QPen(kBorderDark, 1));
    p.setBrush(fill);
    p.drawRoundedRect(face, 2, 2);
    // Subtle top highlight like btn_embedded_*.svg.
    p.setPen(QPen(QColor(220, 220, 220, active ? 40 : 18), 1));
    p.drawLine(face.topLeft() + QPointF(2, 1), face.topRight() + QPointF(-2, 1));
    p.restore();
}

} // namespace spatialstyle
