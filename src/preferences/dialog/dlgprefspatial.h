#pragma once

#include <QList>
#include <QTimer>
#include <memory>

#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/usersettings.h"

class ControlProxy;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
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
