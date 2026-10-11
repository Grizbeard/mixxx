#pragma once

#include <QHash>
#include <QPointF>
#include <QWidget>
#include <array>
#include <memory>
#include <vector>

#include "control/pollingcontrolproxy.h"
#include "engine/spatial/spatialmath.h"

class ControlProxy;

/// The room seen from above: the speaker ring with live speaker levels, the
/// stems of each deck as dots (colour = stem, ring = deck), their width,
/// auto-pan paths and the echo placement. Drag a dot to pan it (angle) and
/// spread it (towards the centre); double-tap a dot to toggle Front. Every
/// finger drags its own dot.
class SpatialField : public QWidget {
    Q_OBJECT
  public:
    explicit SpatialField(int decks, QWidget* pParent = nullptr);
    ~SpatialField() override;

    /// Reads the controls; repaints if anything moved. Call at ~60 Hz.
    void poll();

    QSize sizeHint() const override {
        return {640, 640};
    }
    bool hasHeightForWidth() const override {
        return true;
    }
    int heightForWidth(int width) const override {
        return width;
    }

  signals:
    void sourceTouched(int deck, int slot);

  protected:
    bool event(QEvent* pEvent) override;
    void paintEvent(QPaintEvent* pEvent) override;
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseMoveEvent(QMouseEvent* pEvent) override;
    void mouseReleaseEvent(QMouseEvent* pEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* pEvent) override;

  private:
    struct Source {
        int deck = 0;
        int slot = 0;
        std::unique_ptr<PollingControlProxy> pX, pY, pLevel, pActive, pAzimuth, pWidth, pSpread,
                pAutopan, pUnit, pFront, pFrontBlend, pColor, pSend;
        ControlProxy* pSetAzimuth = nullptr;
        ControlProxy* pSetSpread = nullptr;
        ControlProxy* pSetFront = nullptr;
        // Polled state
        QPointF position;
        double level = 0;
        bool active = false;
        double azimuth = 0;
        double width = 0;
        bool autopan = false;
        int unit = 1;
        bool front = false;
        QColor color;
    };
    struct Grab {
        int source = -1;
        QPointF offset; // dot centre minus finger, in pixels
    };

    QPointF toScreen(QPointF unit) const;
    QPointF toUnit(QPointF screen) const;
    double ringRadius() const;
    int sourceAt(QPointF screen) const;
    void grab(int id, QPointF screen);
    void drag(int id, QPointF screen);
    void release(int id);
    void toggleFront(int source);
    void paintSpeakers(QPainter& p);
    void paintPaths(QPainter& p);
    void paintSource(QPainter& p, const Source& source, bool selected);

    const int m_decks;
    std::vector<Source> m_sources;
    std::unique_ptr<PollingControlProxy> m_pSpeakerCount, m_pLayoutCustom, m_pChannelCount,
            m_pLfEnabled, m_pDelayShape, m_pDelaySize, m_pDelayEnabled, m_pHeadphones;
    std::array<std::unique_ptr<PollingControlProxy>, 8> m_pEchoLevel;
    std::array<std::unique_ptr<PollingControlProxy>, mixxx::spatial::kMaxSpeakers> m_pSpeakerAzimuth;
    std::array<std::unique_ptr<PollingControlProxy>, mixxx::spatial::kMaxOutputChannels> m_pVu;
    std::array<std::unique_ptr<PollingControlProxy>, 2> m_pAutopanShape, m_pAutopanSize,
            m_pAutopanPosition;
    std::vector<std::unique_ptr<PollingControlProxy>> m_pStemCount;

    // Polled state
    int m_speakers = 6;
    std::array<double, mixxx::spatial::kMaxSpeakers> m_azimuths{};
    std::array<double, mixxx::spatial::kMaxOutputChannels> m_vu{};
    bool m_lf = false;
    int m_channels = 0;
    bool m_headphones = false;
    std::array<double, 8> m_echoLevel{};
    bool m_echoVisible = false;
    QByteArray m_lastState;

    QHash<int, Grab> m_grabs;
    int m_selected = -1;
    qint64 m_lastTapMs = 0;
    int m_lastTapSource = -1;
};
