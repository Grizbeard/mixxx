#pragma once

#include <QList>
#include <QTimer>
#include <functional>
#include <memory>

#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/usersettings.h"

class ControlProxy;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;

/// Spatial Audio preferences: the speaker ring on the Spatial output, and a
/// test signal to identify each channel in the room.
///
/// Every setting acts immediately (the values live in [Spatial] controls and
/// persist themselves), so Apply has nothing left to do. The test signal
/// stops whenever the page is hidden.
class DlgPrefSpatial : public DlgPreferencePage {
    Q_OBJECT
  public:
    DlgPrefSpatial(QWidget* pParent, UserSettingsPointer pConfig);
    ~DlgPrefSpatial() override;

  public slots:
    void slotUpdate() override;
    void slotApply() override;
    void slotResetToDefaults() override;

  protected:
    void hideEvent(QHideEvent* pEvent) override;

  private slots:
    void slotRebuildChannels();
    void slotTestToggled(int channel, bool checked);
    void slotWalkStep();

  private:
    QGroupBox* createAlignmentGroup();
    QGroupBox* createHeadphoneGroup();
    QDoubleSpinBox* bindSpinBox(const QString& key,
            double min,
            double max,
            double step,
            int decimals,
            const QString& suffix);
    QCheckBox* bindCheckBox(const QString& key, const QString& text);
    void refreshAlignment();
    void stopTest();
    QString roleOfChannel(int channel) const;

    const UserSettingsPointer m_pConfig;

    std::unique_ptr<ControlProxy> m_pChannelCount;
    std::unique_ptr<ControlProxy> m_pChannelBase;
    std::unique_ptr<ControlProxy> m_pSpeakerCount;
    std::unique_ptr<ControlProxy> m_pLfEnabled;
    std::unique_ptr<ControlProxy> m_pLfCrossover;
    std::unique_ptr<ControlProxy> m_pTestChannel;
    std::unique_ptr<ControlProxy> m_pTestSignal;
    std::unique_ptr<ControlProxy> m_pTestFrequency;
    std::unique_ptr<ControlProxy> m_pTestLevelDb;
    std::unique_ptr<ControlProxy> m_pMainPassthrough;
    std::unique_ptr<ControlProxy> m_pLayoutCustom;
    QList<QList<QWidget*>> m_speakerRows; // name, angle, trim, delay
    QList<std::function<void()>> m_alignmentRefresh;

    QLabel* m_pStatus;
    QSpinBox* m_pSpeakers;
    QCheckBox* m_pLf;
    QSpinBox* m_pCrossover;
    QCheckBox* m_pPassthrough;
    QComboBox* m_pSignal;
    QSpinBox* m_pFrequency;
    QDoubleSpinBox* m_pLevel;
    QGridLayout* m_pChannelGrid;
    QButtonGroup* m_pTestButtons;
    QPushButton* m_pWalk;
    QList<QWidget*> m_channelWidgets;

    QTimer m_walkTimer;
    int m_walkChannel = -1;
};
