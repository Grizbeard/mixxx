#pragma once

#include <QTimer>
#include <QWidget>
#include <memory>

#include "control/pollingcontrolproxy.h"
#include "preferences/usersettings.h"

class QStackedWidget;
class SpatialActionButton;
class SpatialAutopanPanel;
class SpatialDeckPanel;
class SpatialFxPanel;
class SpatialBasicSurface;
class SpatialField;
class SpatialTopBar;

/// The Spatial Mixer: a separate top-level window, built for a touch
/// monitor, in the LateNight style. The room view with every stem, each
/// deck's stem strips, and the auto-pan units and Spatial FX unit in tabs.
///
/// It opens from View > Spatial Mixer (Ctrl+Shift+S), remembers its screen,
/// geometry and full-screen state, and reopens at start-up if it was open.
/// DJ keyboard shortcuts keep working while it has focus.
class SpatialWindow : public QWidget {
    Q_OBJECT
  public:
    SpatialWindow(UserSettingsPointer pConfig, QObject* pKeyboardFilter, QWidget* pParent);
    ~SpatialWindow() override;

    void toggleFullScreen();
    /// 0 = Advanced (every setting), 1 = Basic (the controller's layout).
    void setLayoutMode(int mode);
    int layoutMode() const {
        return m_layoutMode;
    }
    /// Shows it where it was last, or full screen on a second screen.
    void showRestored();

  signals:
    void visibilityChanged(bool visible);
    void preferencesRequested();

  protected:
    void closeEvent(QCloseEvent* pEvent) override;
    void showEvent(QShowEvent* pEvent) override;
    void hideEvent(QHideEvent* pEvent) override;
    void paintEvent(QPaintEvent* pEvent) override;

  private slots:
    void slotPoll();

  private:
    void showTab(int index);
    void saveGeometryToConfig();

    const UserSettingsPointer m_pConfig;
    SpatialTopBar* m_pTopBar;
    SpatialField* m_pField;
    QList<SpatialDeckPanel*> m_decks;
    QStackedWidget* m_pTabs;
    QList<SpatialActionButton*> m_tabButtons;
    QList<SpatialAutopanPanel*> m_autopans;
    SpatialFxPanel* m_pFx;
    QList<QWidget*> m_advanced;
    SpatialBasicSurface* m_pBasic;
    int m_layoutMode = 0;
    QTimer m_pollTimer;
};
