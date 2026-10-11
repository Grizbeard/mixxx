#include "spatial/spatialwindow.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPainter>
#include <QScreen>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <cmath>

#include "moc_spatialwindow.cpp"
#include "spatial/spatialfield.h"
#include "spatial/spatialpanels.h"
#include "spatial/spatialpresets.h"
#include "spatial/spatialstyle.h"

using namespace spatialstyle;

namespace {

const QString kConfigGroup = QStringLiteral("[SpatialWindow]");
constexpr int kDecks = 2;
constexpr int kPollMs = 16;

std::unique_ptr<PollingControlProxy> makePoll(const QString& group, const QString& key) {
    return std::make_unique<PollingControlProxy>(group, key, ControlFlag::AllowMissingOrInvalid);
}

} // namespace

/// The window's toolbar: title, beat, output meters, limiter, and the
/// window buttons.
class SpatialTopBar : public QWidget {
  public:
    SpatialTopBar(SpatialWindow* pWindow, QWidget* pParent)
            : QWidget(pParent) {
        setFixedHeight(54);
        const QString spatial = QStringLiteral("[Spatial]");
        m_pBeat = makePoll(spatial, "beat");
        m_pChannels = makePoll(spatial, "channel_count");
        m_pSpeakers = makePoll(spatial, "speaker_count");
        m_pLf = makePoll(spatial, "lf_enabled");
        m_pLimiter = makePoll(spatial, "limiter_reduction_db");
        m_pPassthrough = makePoll(spatial, "main_passthrough");
        m_pHeadphones = makePoll(spatial, "headphone_spatial");
        for (int c = 0; c < 8; ++c) {
            m_pVu[c] = makePoll(spatial, QStringLiteral("vu_meter_%1").arg(c + 1));
        }

        auto* pLayout = new QHBoxLayout(this);
        pLayout->setContentsMargins(0, 6, 8, 6);
        pLayout->setSpacing(6);
        pLayout->addStretch(1);
        // Hear the spatial mix in headphones (rendered binaurally).
        auto* pPhones = new SpatialButton(QStringLiteral("[Spatial]"),
                "headphone_spatial",
                SpatialButton::Mode::Toggle,
                tr("PHONES 3D"));
        pPhones->setAccent(kPlayPos);
        pPhones->setFixedWidth(130);
        pLayout->addWidget(pPhones);
        auto* pSetup = new SpatialActionButton(
                tr("SETUP"), [pWindow]() { emit pWindow->preferencesRequested(); }, this);
        m_pFullScreen = new SpatialActionButton(
                tr("FULL SCREEN"), [pWindow]() { pWindow->toggleFullScreen(); }, this);
        auto* pClose = new SpatialActionButton(tr("CLOSE"), [pWindow]() { pWindow->close(); }, this);
        for (SpatialActionButton* pButton : {pSetup, m_pFullScreen, pClose}) {
            pButton->setFixedWidth(130);
            pLayout->addWidget(pButton);
        }
    }

    void poll(bool fullScreen) {
        m_pFullScreen->setActive(fullScreen);
        QByteArray state;
        QDataStream out(&state, QIODevice::WriteOnly);
        m_beat = static_cast<int>(std::floor(m_pBeat->get())) % 4;
        if (m_beat < 0) {
            m_beat += 4;
        }
        m_channels = static_cast<int>(m_pChannels->get());
        m_speakers = std::min(static_cast<int>(std::lround(m_pSpeakers->get())),
                std::max(m_channels, 0));
        m_lf = m_pLf->get() > 0 && m_speakers < m_channels;
        m_limiter = std::round(m_pLimiter->get() * 2) / 2;
        m_passthrough = m_pPassthrough->get() > 0;
        m_headphones = m_pHeadphones->get() > 0;
        out << m_headphones << m_beat << m_channels << m_speakers << m_lf << m_limiter << m_passthrough;
        for (int c = 0; c < 8; ++c) {
            m_vu[c] = std::round(m_pVu[c]->get() * 32) / 32;
            out << m_vu[c];
        }
        if (state != m_state) {
            m_state = state;
            update();
        }
    }

  protected:
    void paintEvent(QPaintEvent* pEvent) override {
        Q_UNUSED(pEvent);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        paintContainer(p, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kDim);
        double x = 14;
        p.setPen(kText);
        p.setFont(styleFont(18, true));
        p.drawText(QRectF(x, 0, 220, height()), Qt::AlignLeft | Qt::AlignVCenter,
                tr("SPATIAL MIXER"));
        x += 200;

        // Beat: four steps, lit on the current beat of the spatial clock.
        for (int b = 0; b < 4; ++b) {
            const QRectF square(x + b * 22, height() / 2.0 - 8, 16, 16);
            paintButton(p, square, b == m_beat, false, b == 0 ? kHotOrange : kDeck12);
        }
        x += 4 * 22 + 24;

        // Output meters: one per channel, speakers then LF.
        p.setFont(styleFont(10, true));
        if (m_channels <= 0) {
            p.setPen(m_headphones ? kPlayPos : kHotOrange);
            p.setFont(styleFont(13, true));
            p.drawText(QRectF(x, 0, 420, height()), Qt::AlignLeft | Qt::AlignVCenter,
                    m_headphones ? tr("Headphones only: no Spatial output")
                                 : tr("Spatial output not assigned"));
            x += 420;
        } else {
            const int shown = m_speakers + (m_lf ? 1 : 0);
            for (int c = 0; c < shown; ++c) {
                const QRectF well(x + c * 30, 10, 22, height() - 30);
                p.setPen(QPen(kBorderDark, 1));
                p.setBrush(kBar);
                p.drawRect(well);
                const double db = m_vu[c] > 0 ? 20 * std::log10(m_vu[c]) : -96;
                const double level = std::clamp((db + 48) / 48, 0.0, 1.0);
                p.setPen(Qt::NoPen);
                p.setBrush(db > -1 ? QColor(0xa8, 0, 0) : (db > -9 ? QColor(0xd0, 0x93, 0) : kGreen));
                p.drawRect(QRectF(well.left() + 1, well.bottom() - level * well.height(),
                        well.width() - 1, level * well.height()));
                p.setPen(kLabel);
                p.drawText(QRectF(well.left() - 4, height() - 19, well.width() + 8, 14),
                        Qt::AlignCenter,
                        c < m_speakers ? QString::number(c + 1) : QStringLiteral("LF"));
            }
            x += shown * 30 + 16;
        }

        p.setFont(styleFont(13, true));
        if (m_passthrough) {
            p.setPen(kHotOrange);
            p.drawText(QRectF(x, 0, 260, height()), Qt::AlignLeft | Qt::AlignVCenter,
                    tr("STEREO PASSTHROUGH"));
            x += 230;
        }
        p.setPen(m_limiter < -0.5 ? QColor(0xe0, 0x30, 0x20) : kLabel);
        p.drawText(QRectF(x, 0, 200, height()), Qt::AlignLeft | Qt::AlignVCenter,
                m_limiter < -0.05 ? tr("LIMIT %1 dB").arg(m_limiter, 0, 'f', 1) : tr("LIMIT"));
    }

  private:
    std::unique_ptr<PollingControlProxy> m_pBeat, m_pChannels, m_pSpeakers, m_pLf, m_pLimiter,
            m_pPassthrough, m_pHeadphones;
    std::array<std::unique_ptr<PollingControlProxy>, 8> m_pVu;
    SpatialActionButton* m_pFullScreen;
    QByteArray m_state;
    int m_beat = 0;
    int m_channels = 0;
    int m_speakers = 0;
    bool m_lf = false;
    double m_limiter = 0;
    bool m_passthrough = false;
    bool m_headphones = false;
    std::array<double, 8> m_vu{};
};

SpatialWindow::SpatialWindow(UserSettingsPointer pConfig, QObject* pKeyboardFilter, QWidget* pParent)
        : QWidget(pParent, Qt::Window),
          m_pConfig(pConfig) {
    setWindowTitle(tr("Mixxx Spatial Mixer"));
    setAttribute(Qt::WA_AcceptTouchEvents);
    if (pKeyboardFilter) {
        // DJ keyboard shortcuts keep working while this window has focus.
        installEventFilter(pKeyboardFilter);
    }

    auto* pRoot = new QVBoxLayout(this);
    pRoot->setContentsMargins(6, 6, 6, 6);
    pRoot->setSpacing(6);

    m_pTopBar = new SpatialTopBar(this, this);
    pRoot->addWidget(m_pTopBar);

    auto* pMain = new QHBoxLayout();
    pMain->setSpacing(6);
    m_pField = new SpatialField(kDecks, this);
    pMain->addWidget(m_pField, 1);
    for (int deck = 0; deck < kDecks; ++deck) {
        auto* pDeck = new SpatialDeckPanel(deck, this);
        m_decks.append(pDeck);
        pMain->addWidget(pDeck);
    }
    pRoot->addLayout(pMain, 1);

    auto* pTabRow = new QHBoxLayout();
    pTabRow->setSpacing(4);
    const QStringList tabs = {tr("AUTO-PAN A"), tr("AUTO-PAN B"), tr("SPATIAL FX")};
    m_pTabs = new QStackedWidget(this);
    for (int i = 0; i < tabs.size(); ++i) {
        auto* pButton = new SpatialActionButton(tabs[i], [this, i]() { showTab(i); }, this);
        pButton->setAccent(i < 2 ? kGreen : kTeal);
        m_tabButtons.append(pButton);
        pTabRow->addWidget(pButton);
    }
    pRoot->addLayout(pTabRow);
    // Presets: one bank per tool; both auto-pan units share theirs.
    const QString settingsPath = m_pConfig->getSettingsPath();
    auto* pAutopanBank = new SpatialPresetBank(QStringLiteral("autopan"),
            spatialpresets::autopanKeys(),
            spatialpresets::autopanFactory(),
            settingsPath,
            this);
    auto* pFxBank = new SpatialPresetBank(QStringLiteral("fx"),
            spatialpresets::fxKeys(),
            spatialpresets::fxFactory(),
            settingsPath,
            this);
    // Presets saved before delay and reverb shared a list.
    pFxBank->importLegacy(QStringLiteral("delay"), QStringLiteral("[SpatialDelay]"));
    pFxBank->importLegacy(QStringLiteral("reverb"), QStringLiteral("[SpatialReverb]"));
    for (int unit = 1; unit <= 2; ++unit) {
        auto* pAutopan = new SpatialAutopanPanel(unit, pAutopanBank, m_pTabs);
        m_autopans.append(pAutopan);
        m_pTabs->addWidget(pAutopan);
    }
    m_pFx = new SpatialFxPanel(pFxBank, m_pConfig, m_pTabs);
    m_pTabs->addWidget(m_pFx);
    m_pTabs->setMinimumHeight(260);
    m_pTabs->setMaximumHeight(350);
    pRoot->addWidget(m_pTabs);
    showTab(m_pConfig->getValue(ConfigKey(kConfigGroup, "tab"), 0));

    m_pollTimer.setInterval(kPollMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &SpatialWindow::slotPoll);
}

SpatialWindow::~SpatialWindow() = default;

void SpatialWindow::showTab(int index) {
    index = std::clamp(index, 0, m_pTabs->count() - 1);
    m_pTabs->setCurrentIndex(index);
    for (int i = 0; i < m_tabButtons.size(); ++i) {
        m_tabButtons[i]->setActive(i == index);
    }
    m_pConfig->setValue(ConfigKey(kConfigGroup, "tab"), index);
}

void SpatialWindow::slotPoll() {
    m_pTopBar->poll(isFullScreen());
    m_pField->poll();
    for (SpatialDeckPanel* pDeck : std::as_const(m_decks)) {
        pDeck->poll();
    }
    for (SpatialAutopanPanel* pAutopan : std::as_const(m_autopans)) {
        pAutopan->poll();
    }
    m_pFx->poll();
}

void SpatialWindow::toggleFullScreen() {
    if (isFullScreen()) {
        showNormal();
    } else {
        showFullScreen();
    }
    m_pConfig->setValue(ConfigKey(kConfigGroup, "fullscreen"), isFullScreen() ? 1 : 0);
}

void SpatialWindow::showRestored() {
    const QString geometry = m_pConfig->getValueString(ConfigKey(kConfigGroup, "geometry"));
    const bool fullScreen = m_pConfig->getValue(ConfigKey(kConfigGroup, "fullscreen"), true);
    if (!geometry.isEmpty()) {
        restoreGeometry(QByteArray::fromBase64(geometry.toLatin1()));
    } else {
        // First time: the touch monitor, if there is a second screen.
        const QList<QScreen*> screens = QGuiApplication::screens();
        QScreen* pTarget = nullptr;
        const QWidget* pMain = parentWidget();
        for (QScreen* pScreen : screens) {
            if (!pMain || pScreen != pMain->screen()) {
                pTarget = pScreen;
                break;
            }
        }
        if (pTarget && screens.size() > 1) {
            setGeometry(pTarget->availableGeometry());
        } else if (QScreen* pScreen = pMain ? pMain->screen() : QGuiApplication::primaryScreen()) {
            const QRect available = pScreen->availableGeometry();
            resize(available.size() * 0.9);
            move(available.center() - rect().center());
        }
    }
    if (fullScreen && (!geometry.isEmpty() || QGuiApplication::screens().size() > 1)) {
        showFullScreen();
    } else {
        show();
    }
    raise();
    activateWindow();
}

void SpatialWindow::saveGeometryToConfig() {
    m_pConfig->setValue(ConfigKey(kConfigGroup, "geometry"),
            QString::fromLatin1(saveGeometry().toBase64()));
    m_pConfig->setValue(ConfigKey(kConfigGroup, "fullscreen"), isFullScreen() ? 1 : 0);
}

void SpatialWindow::closeEvent(QCloseEvent* pEvent) {
    saveGeometryToConfig();
    // Closed by the user: don't reopen at start-up.
    m_pConfig->setValue(ConfigKey(kConfigGroup, "open"), 0);
    QWidget::closeEvent(pEvent);
}

void SpatialWindow::showEvent(QShowEvent* pEvent) {
    QWidget::showEvent(pEvent);
    m_pConfig->setValue(ConfigKey(kConfigGroup, "open"), 1);
    m_pollTimer.start();
    emit visibilityChanged(true);
}

void SpatialWindow::hideEvent(QHideEvent* pEvent) {
    QWidget::hideEvent(pEvent);
    saveGeometryToConfig();
    m_pollTimer.stop();
    emit visibilityChanged(false);
}

void SpatialWindow::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter p(this);
    p.fillRect(rect(), kWindow);
}
