#pragma once

#include <QDialog>

#include "library/trackset/crate/crateid.h"
#include "preferences/usersettings.h"
#include "track/trackid.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace mixxx::stemconverter {

class StemConverter;

/// "Convert to Stems": what to convert, with which preset, to where.
/// Opened for a crate (with or without its subcrates) or for a selection of
/// tracks.
class DlgStemConvert : public QDialog {
    Q_OBJECT
  public:
    DlgStemConvert(QWidget* pParent,
            StemConverter* pConverter,
            UserSettingsPointer pConfig,
            CrateId crateId,
            const QString& crateName);
    DlgStemConvert(QWidget* pParent,
            StemConverter* pConverter,
            UserSettingsPointer pConfig,
            const QList<TrackId>& trackIds);

    void accept() override;

  private slots:
    void slotUpdateSummary();
    void slotUpdatePresetDescription();
    void slotReloadPresets();

  private:
    void setUp(const QString& heading);
    bool isCrate() const {
        return m_crateId.isValid();
    }

    StemConverter* const m_pConverter;
    const UserSettingsPointer m_pConfig;
    const CrateId m_crateId;
    const QList<TrackId> m_trackIds;

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
    QProgressBar* m_pProgress;
    QTimer* m_pRefreshTimer;
    QPushButton* m_pPauseResume;
    QPushButton* m_pCancel;
};

} // namespace mixxx::stemconverter
