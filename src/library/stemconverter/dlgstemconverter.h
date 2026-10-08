#pragma once

#include <QDialog>

#include "library/trackset/crate/crateid.h"
#include "preferences/usersettings.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace mixxx::stemconverter {

class StemConverter;

/// "Convert to Stems": what to convert, with which preset, to where.
class DlgStemConvert : public QDialog {
    Q_OBJECT
  public:
    DlgStemConvert(QWidget* pParent,
            StemConverter* pConverter,
            UserSettingsPointer pConfig,
            CrateId crateId,
            const QString& crateName);

    void accept() override;

  private slots:
    void slotUpdateSummary();
    void slotUpdatePresetDescription();
    void slotReloadPresets();

  private:
    StemConverter* const m_pConverter;
    const UserSettingsPointer m_pConfig;
    const CrateId m_crateId;

    QLabel* m_pSummary;
    QCheckBox* m_pIncludeSubcrates;
    QComboBox* m_pPreset;
    QLabel* m_pPresetDescription;
    QLineEdit* m_pOutputRoot;
    QCheckBox* m_pSkipUnchanged;
    QCheckBox* m_pPauseWhilePlaying;
    QLineEdit* m_pExecutable;
    QPushButton* m_pConvertButton;
};

/// Live list of every queued track, with pause/resume/cancel. Non-modal; one
/// per converter, shown again rather than recreated.
class DlgStemConversionStatus : public QDialog {
    Q_OBJECT
  public:
    DlgStemConversionStatus(QWidget* pParent, StemConverter* pConverter, UserSettingsPointer pConfig);

    static void showFor(QWidget* pParent, StemConverter* pConverter, UserSettingsPointer pConfig);

  private slots:
    void slotEntriesAdded(int first, int count);
    void slotEntryChanged(int index);
    void slotStateChanged();
    void slotPauseResume();

  private:
    StemConverter* const m_pConverter;
    const UserSettingsPointer m_pConfig;
    QTreeWidget* m_pTracks;
    QLabel* m_pHeadline;
    QPushButton* m_pPauseResume;
    QPushButton* m_pCancel;
};

} // namespace mixxx::stemconverter
