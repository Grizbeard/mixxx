#include "widget/wkey.h"

#include <QStyleOption>
#include <QStylePainter>

#include "library/library_prefs.h"
#include "moc_wkey.cpp"
#include "preferences/usersettings.h"
#include "skin/legacy/skincontext.h"
#include "track/keyutils.h"

WKey::WKey(const QString& group, UserSettingsPointer pConfig, QWidget* pParent)
        : WLabel(pParent),
          m_keyNotation(mixxx::library::prefs::kKeyNotationConfigKey, this),
          m_engineKeyDistance(group,
                  "visual_key_distance",
                  this,
                  ControlFlag::AllowMissingOrInvalid),
          m_engineKey(group,
                  "key",
                  this,
                  ControlFlag::AllowMissingOrInvalid),
          m_colorPaletteSettings(pConfig) {
    // What the key-colour paint path used to hard-code. As the default it is
    // the same picture for a skin that asks for nothing, and a skin that does
    // ask - in <Alignment> or in a stylesheet - now gets what it asked for.
    setAlignment(Qt::AlignCenter);
    setValue();
    m_keyNotation.connectValueChanged(this, &WKey::keyNotationChanged);
    m_engineKeyDistance.connectValueChanged(this, &WKey::setCents);
}

void WKey::onConnectedControlChanged(double dParameter, double dValue) {
    Q_UNUSED(dParameter);
    Q_UNUSED(dValue);
    // Enums are not currently represented using parameter space so it doesn't
    // make sense to use the parameter here yet.
    setValue();
}

void WKey::setup(const QDomNode& node, const SkinContext& context) {
    WLabel::setup(node, context);
    m_displayCents = context.selectBool(node, "DisplayCents", false);
    m_displayKey = context.selectBool(node, "DisplayKey", true);
}

void WKey::setValue() {
    m_key = KeyUtils::keyFromNumericValue(m_engineKey.get());
    m_diff_cents = m_engineKeyDistance.get();
    if (m_key != mixxx::track::io::key::INVALID) {
        // Render this key with the user-provided notation.
        QString keyStr = "";
        if (m_displayKey) {
            keyStr = KeyUtils::keyToString(m_key);
        }
        if (m_displayCents) {
            int cents_to_display = static_cast<int>(m_diff_cents * 100);
            char sign = ' ';
            if (m_diff_cents < 0) {
                sign = '-';
            } else if (m_diff_cents > 0) {
                sign = '+';
            }
            keyStr.append(QString(" %1%2c").arg(sign).arg(qAbs(cents_to_display)));
        }
        setText(keyStr);
    } else {
        setText("");
    }
    update();
}

void WKey::setCents() {
    setValue();
}

void WKey::keyNotationChanged(double dKeyNotationValue) {
    Q_UNUSED(dKeyNotationValue);
    // NOTE: dKeyNotationValue is the index of the key notation type, NOT the
    // key itself, so we intentionally set the old value again to update the UI.
    setValue();
}

void WKey::paintEvent(QPaintEvent* event) {
    if (m_key == mixxx::track::io::key::INVALID || !m_colorPaletteSettings.getKeyColorsEnabled()) {
        WLabel::paintEvent(event);
        return;
    }

    ColorPalette keyColorPalette = m_colorPaletteSettings.getConfigKeyColorPalette();

    QColor colorTop, colorBottom;
    double splitPoint = 0; // 'height' of top color
    if (m_diff_cents < 0) {
        colorTop = KeyUtils::keyToColor(m_key, keyColorPalette);
        colorBottom = KeyUtils::keyToColor(KeyUtils::scaleKeySteps(m_key, -1), keyColorPalette);
        splitPoint = m_diff_cents + 1;
    } else {
        colorTop = KeyUtils::keyToColor(KeyUtils::scaleKeySteps(m_key, 1), keyColorPalette);
        colorBottom = KeyUtils::keyToColor(m_key, keyColorPalette);
        splitPoint = m_diff_cents;
    }

    QStyleOption option;
    option.initFrom(this);
    QStylePainter painter(this);

    const QStyle* pStyle = style();
    const QRect contRect = pStyle->subElementRect(QStyle::SE_FrameContents, &option, this);

    const int rectWidth = 4;
    // Keep the text clear of the colour bar. Butted against it the first glyph
    // and the bar read as one smudged mark rather than a marker and a value.
    const int textGap = 4;
    const int splitHeight = static_cast<int>(contRect.height() * splitPoint);

    painter.fillRect(contRect.left(),
            contRect.top(),
            rectWidth,
            splitHeight,
            colorTop);

    painter.fillRect(contRect.left(),
            splitHeight + 1,
            rectWidth,
            contRect.height() - splitHeight,
            colorBottom);

    painter.setPen(option.palette.text().color());

    // Measured and drawn in the same rect, and from the same origin the bar is
    // drawn from: the text used to start at an absolute 4px, which is only the
    // bar's right edge while the widget has no padding of its own.
    const int textLeft = contRect.left() + rectWidth + textGap;
    const int textWidth = contRect.width() - rectWidth - textGap;

    QString elidedText = option.fontMetrics.elidedText(
            text(),
            Qt::ElideRight,
            textWidth);

    painter.drawText(textLeft,
            contRect.top(),
            textWidth,
            contRect.height(),
            static_cast<int>(alignment()),
            elidedText);
}
