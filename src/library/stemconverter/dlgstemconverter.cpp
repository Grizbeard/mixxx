#include "library/stemconverter/dlgstemconverter.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "library/stemconverter/stemconverter.h"
#include "moc_dlgstemconverter.cpp"

namespace mixxx::stemconverter {

namespace {

enum Column {
    kColumnTrack,
    kColumnStatus,
    kColumnDetails,
    kColumnOutput,
    kColumnCount,
};

QWidget* pathRow(QLineEdit* pEdit, QPushButton* pBrowse) {
    auto* pRow = new QWidget;
    auto* pLayout = new QHBoxLayout(pRow);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->addWidget(pEdit, 1);
    pLayout->addWidget(pBrowse);
    return pRow;
}

QString presetDescription(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll())
            .object()
            .value(QStringLiteral("description"))
            .toString();
}

} // namespace

// -- DlgStemConvert ---------------------------------------------------------

DlgStemConvert::DlgStemConvert(QWidget* pParent,
        StemConverter* pConverter,
        UserSettingsPointer pConfig,
        CrateId crateId,
        const QString& crateName)
        : QDialog(pParent),
          m_pConverter(pConverter),
          m_pConfig(std::move(pConfig)),
          m_crateId(crateId) {
    setWindowTitle(tr("Convert to Stems"));
    const Settings settings = Settings::load(m_pConfig);

    auto* pHeading = new QLabel(tr("Separate every track in <b>%1</b> into drums, bass, "
                                   "other and vocals, and file the results under the "
                                   "<b>%2</b> crate.")
                                        .arg(crateName.toHtmlEscaped(),
                                                QString::fromLatin1(
                                                        StemConverter::kStemsRootCrateName)));
    pHeading->setWordWrap(true);

    m_pSummary = new QLabel;
    m_pIncludeSubcrates = new QCheckBox(tr("Include subcrates"));
    m_pIncludeSubcrates->setChecked(true);

    m_pPreset = new QComboBox;
    m_pPresetDescription = new QLabel;
    m_pPresetDescription->setWordWrap(true);
    auto* pEditPreset = new QPushButton(tr("Edit…"));
    pEditPreset->setToolTip(tr("Open the preset's JSON file. Models, overlap, segment size, "
                               "fp16 and output format are all set there."));
    auto* pReloadPresets = new QPushButton(tr("Reload"));
    auto* pPresetRow = new QWidget;
    auto* pPresetLayout = new QHBoxLayout(pPresetRow);
    pPresetLayout->setContentsMargins(0, 0, 0, 0);
    pPresetLayout->addWidget(m_pPreset, 1);
    pPresetLayout->addWidget(pEditPreset);
    pPresetLayout->addWidget(pReloadPresets);

    m_pOutputRoot = new QLineEdit(QDir::toNativeSeparators(settings.outputRoot));
    auto* pBrowseOutput = new QPushButton(tr("Browse…"));
    m_pSkipUnchanged = new QCheckBox(tr("Skip tracks that are already converted and unchanged"));
    m_pSkipUnchanged->setChecked(settings.skipUnchanged);
    m_pPauseWhilePlaying = new QCheckBox(tr("Pause between tracks while a deck is playing"));
    m_pPauseWhilePlaying->setChecked(settings.pauseWhilePlaying);
    m_pExecutable = new QLineEdit(QDir::toNativeSeparators(settings.executable));
    auto* pBrowseExecutable = new QPushButton(tr("Browse…"));

    auto* pForm = new QFormLayout;
    pForm->addRow(QString(), m_pIncludeSubcrates);
    pForm->addRow(tr("Preset:"), pPresetRow);
    pForm->addRow(QString(), m_pPresetDescription);
    pForm->addRow(tr("Output folder:"), pathRow(m_pOutputRoot, pBrowseOutput));
    pForm->addRow(QString(), m_pSkipUnchanged);
    pForm->addRow(QString(), m_pPauseWhilePlaying);
    pForm->addRow(tr("stemforge:"), pathRow(m_pExecutable, pBrowseExecutable));

    auto* pButtons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_pConvertButton = pButtons->addButton(tr("Convert"), QDialogButtonBox::AcceptRole);
    m_pConvertButton->setDefault(true);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->addWidget(pHeading);
    pLayout->addWidget(m_pSummary);
    pLayout->addLayout(pForm);
    pLayout->addWidget(pButtons);
    setMinimumWidth(560);

    connect(pButtons, &QDialogButtonBox::accepted, this, &DlgStemConvert::accept);
    connect(pButtons, &QDialogButtonBox::rejected, this, &DlgStemConvert::reject);
    connect(m_pIncludeSubcrates, &QCheckBox::toggled, this, &DlgStemConvert::slotUpdateSummary);
    connect(m_pPreset,
            &QComboBox::currentIndexChanged,
            this,
            &DlgStemConvert::slotUpdatePresetDescription);
    connect(pReloadPresets, &QPushButton::clicked, this, &DlgStemConvert::slotReloadPresets);
    connect(m_pExecutable, &QLineEdit::editingFinished, this, &DlgStemConvert::slotReloadPresets);
    connect(pEditPreset, &QPushButton::clicked, this, [this] {
        const QString path = m_pPreset->currentData().toString();
        if (!path.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
    });
    connect(pBrowseOutput, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
                this, tr("Stems output folder"), m_pOutputRoot->text());
        if (!dir.isEmpty()) {
            m_pOutputRoot->setText(QDir::toNativeSeparators(dir));
        }
    });
    connect(pBrowseExecutable, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this,
                tr("stemforge executable"),
                m_pExecutable->text(),
                tr("Programs (*.exe);;All files (*)"));
        if (!file.isEmpty()) {
            m_pExecutable->setText(QDir::toNativeSeparators(file));
            slotReloadPresets();
        }
    });

    slotReloadPresets();
    slotUpdateSummary();
}

void DlgStemConvert::slotReloadPresets() {
    const Settings settings = Settings::load(m_pConfig);
    const QString wanted = m_pPreset->count() > 0
            ? m_pPreset->currentData().toString()
            : settings.presetPath;
    m_pPreset->blockSignals(true);
    m_pPreset->clear();
    const QStringList files = presetFiles(QDir::fromNativeSeparators(m_pExecutable->text()), m_pConfig);
    for (const QString& path : files) {
        m_pPreset->addItem(presetDisplayName(path), path);
    }
    const int index = m_pPreset->findData(wanted);
    m_pPreset->setCurrentIndex(index >= 0 ? index : 0);
    m_pPreset->blockSignals(false);
    slotUpdatePresetDescription();
}

void DlgStemConvert::slotUpdatePresetDescription() {
    const QString path = m_pPreset->currentData().toString();
    m_pPresetDescription->setText(path.isEmpty()
                    ? tr("No presets found next to stemforge.")
                    : presetDescription(path));
    m_pConvertButton->setEnabled(!path.isEmpty() && m_pConverter);
}

void DlgStemConvert::slotUpdateSummary() {
    const auto plan = m_pConverter->planCrate(m_crateId, m_pIncludeSubcrates->isChecked());
    QString text = tr("%n track(s)", "", plan.tracks) + QStringLiteral(" · ") +
            tr("%n crate(s)", "", plan.crates);
    if (plan.alreadyStems > 0) {
        text += QStringLiteral(" · ") +
                tr("%n stem file(s) left out", "", plan.alreadyStems);
    }
    m_pSummary->setText(text);
    m_pConvertButton->setEnabled(plan.tracks > 0 && m_pPreset->count() > 0);
}

void DlgStemConvert::accept() {
    Settings settings;
    settings.executable = QDir::fromNativeSeparators(m_pExecutable->text().trimmed());
    settings.outputRoot = QDir::fromNativeSeparators(m_pOutputRoot->text().trimmed());
    settings.presetPath = m_pPreset->currentData().toString();
    settings.skipUnchanged = m_pSkipUnchanged->isChecked();
    settings.pauseWhilePlaying = m_pPauseWhilePlaying->isChecked();
    settings.save(m_pConfig);

    if (m_pConverter->enqueueCrate(m_crateId, m_pIncludeSubcrates->isChecked()) > 0) {
        DlgStemConversionStatus::showFor(parentWidget(), m_pConverter, m_pConfig);
    }
    QDialog::accept();
}

// -- DlgStemConversionStatus ------------------------------------------------

DlgStemConversionStatus::DlgStemConversionStatus(
        QWidget* pParent, StemConverter* pConverter, UserSettingsPointer pConfig)
        : QDialog(pParent),
          m_pConverter(pConverter),
          m_pConfig(std::move(pConfig)) {
    setWindowTitle(tr("Stem Conversion"));
    setAttribute(Qt::WA_DeleteOnClose, false);

    m_pHeadline = new QLabel;
    m_pTracks = new QTreeWidget;
    m_pTracks->setColumnCount(kColumnCount);
    m_pTracks->setHeaderLabels({tr("Track"), tr("Status"), tr("Details"), tr("Output")});
    m_pTracks->setRootIsDecorated(false);
    m_pTracks->setUniformRowHeights(true);
    m_pTracks->header()->setSectionResizeMode(kColumnTrack, QHeaderView::Interactive);
    m_pTracks->header()->setStretchLastSection(true);
    m_pTracks->setColumnWidth(kColumnTrack, 320);
    m_pTracks->setColumnWidth(kColumnStatus, 90);
    m_pTracks->setColumnWidth(kColumnDetails, 320);

    m_pPauseResume = new QPushButton;
    m_pCancel = new QPushButton(tr("Cancel all"));
    auto* pOpenOutput = new QPushButton(tr("Open output folder"));
    auto* pOpenLogs = new QPushButton(tr("Open logs"));
    auto* pClose = new QPushButton(tr("Close"));

    auto* pButtons = new QHBoxLayout;
    pButtons->addWidget(m_pPauseResume);
    pButtons->addWidget(m_pCancel);
    pButtons->addStretch(1);
    pButtons->addWidget(pOpenOutput);
    pButtons->addWidget(pOpenLogs);
    pButtons->addWidget(pClose);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->addWidget(m_pHeadline);
    pLayout->addWidget(m_pTracks, 1);
    pLayout->addLayout(pButtons);
    resize(1000, 480);

    connect(m_pPauseResume, &QPushButton::clicked, this, &DlgStemConversionStatus::slotPauseResume);
    connect(m_pCancel, &QPushButton::clicked, m_pConverter, &StemConverter::cancel);
    connect(pClose, &QPushButton::clicked, this, &QDialog::hide);
    connect(pOpenOutput, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(Settings::load(m_pConfig).outputRoot));
    });
    connect(pOpenLogs, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(
                QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("stemforge/logs"))));
    });
    connect(m_pTracks, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem* pItem) {
        const QString output = pItem->text(kColumnOutput);
        if (QFileInfo::exists(output)) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(output).absolutePath()));
        }
    });
    connect(m_pConverter, &StemConverter::entriesAdded, this, &DlgStemConversionStatus::slotEntriesAdded);
    connect(m_pConverter, &StemConverter::entryChanged, this, &DlgStemConversionStatus::slotEntryChanged);
    connect(m_pConverter, &StemConverter::stateChanged, this, &DlgStemConversionStatus::slotStateChanged);

    slotEntriesAdded(0, static_cast<int>(m_pConverter->entries().size()));
    slotStateChanged();
}

void DlgStemConversionStatus::showFor(
        QWidget* pParent, StemConverter* pConverter, UserSettingsPointer pConfig) {
    static QPointer<DlgStemConversionStatus> s_pDialog;
    if (!s_pDialog) {
        s_pDialog = new DlgStemConversionStatus(pParent, pConverter, std::move(pConfig));
    }
    s_pDialog->show();
    s_pDialog->raise();
    s_pDialog->activateWindow();
}

void DlgStemConversionStatus::slotEntriesAdded(int first, int count) {
    for (int i = first; i < first + count; ++i) {
        auto* pItem = new QTreeWidgetItem(m_pTracks);
        pItem->setText(kColumnOutput, m_pConverter->entries().at(i).outputPath);
        slotEntryChanged(i);
    }
    slotStateChanged();
}

void DlgStemConversionStatus::slotEntryChanged(int index) {
    QTreeWidgetItem* pItem = m_pTracks->topLevelItem(index);
    if (!pItem) {
        return;
    }
    const TrackEntry& entry = m_pConverter->entries().at(index);
    pItem->setText(kColumnTrack, entry.displayName);
    pItem->setToolTip(kColumnTrack, entry.sourcePath);
    pItem->setText(kColumnStatus, trackStateLabel(entry.state));
    pItem->setText(kColumnDetails, entry.detail);
    pItem->setToolTip(kColumnDetails, entry.detail);
    if (entry.state == TrackState::Running) {
        m_pTracks->scrollToItem(pItem);
    }
    slotStateChanged();
}

void DlgStemConversionStatus::slotStateChanged() {
    int finished = 0;
    int failed = 0;
    const auto& entries = m_pConverter->entries();
    for (const TrackEntry& entry : entries) {
        switch (entry.state) {
        case TrackState::Done:
        case TrackState::Skipped:
            ++finished;
            break;
        case TrackState::Failed:
        case TrackState::Cancelled:
            ++failed;
            break;
        default:
            break;
        }
    }
    QString headline = tr("%1 of %2 tracks converted").arg(finished).arg(entries.size());
    if (failed > 0) {
        headline += QStringLiteral(" · ") + tr("%n not converted", "", failed);
    }
    if (m_pConverter->isRunning() && m_pConverter->isPaused()) {
        headline += QStringLiteral(" · ") + tr("paused after the current track");
    }
    m_pHeadline->setText(headline);
    m_pPauseResume->setText(m_pConverter->isUserPaused() ? tr("Resume") : tr("Pause"));
    m_pPauseResume->setEnabled(m_pConverter->isRunning());
    m_pCancel->setEnabled(m_pConverter->isRunning());
}

void DlgStemConversionStatus::slotPauseResume() {
    if (m_pConverter->isUserPaused()) {
        m_pConverter->resume();
    } else {
        m_pConverter->pause();
    }
}

} // namespace mixxx::stemconverter
