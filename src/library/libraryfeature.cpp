#include "library/libraryfeature.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include "library/library.h"
#include "library/parserm3u.h"
#include "library/parserpls.h"
#include "moc_libraryfeature.cpp"
#include "util/logger.h"

// KEEP THIS cpp file to tell scons that moc should be called on the class!!!
// The reason for this is that LibraryFeature uses slots/signals and for this
// to work the code has to be precompiles by moc

namespace {

const mixxx::Logger kLogger("LibraryFeature");
const QString kIconPath = QStringLiteral(":/images/library/ic_library_%1.svg");
const QString kSkinIconPath =
        QStringLiteral("%1/skins/%2/library/ic_library_%3.svg");

/// Path to a skin-provided replacement for a sidebar icon, or an empty string.
///
/// The built-in icons are compiled into the binary, so a skin whose palette
/// does not contain their colours has no way to bring the sidebar into line
/// with the rest of itself. A skin may ship
/// `<skin>/library/ic_library_<name>.svg` to override one.
///
/// This is purely additive: a skin that provides nothing keeps the built-in
/// icon, so existing skins are unaffected. Icons are resolved per skin rather
/// than per colour scheme, because features are constructed before any skin is
/// parsed and a scheme's asset directory is a skin-internal detail.
QString skinIconPath(const UserSettingsPointer& pConfig, const QString& iconName) {
    const QString skinName =
            pConfig->getValueString(ConfigKey("[Config]", "ResizableSkin"));
    if (skinName.isEmpty()) {
        return QString();
    }
    // Same search order as SkinLoader: a user skin shadows a system one.
    const QStringList bases = {
            pConfig->getSettingsPath(), pConfig->getResourcePath()};
    for (const QString& base : bases) {
        if (base.isEmpty()) {
            continue;
        }
        const QString candidate = kSkinIconPath.arg(
                QDir::cleanPath(base), skinName, iconName);
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
}

} // anonymous namespace

QIcon LibraryFeature::iconForName(
        const UserSettingsPointer& pConfig, const QString& iconName) {
    const QString overridePath = skinIconPath(pConfig, iconName);
    if (overridePath.isEmpty()) {
        return QIcon(kIconPath.arg(iconName));
    }
    QIcon icon(overridePath);
    // QIcon generates the pixmap for a selected row by blending the Normal one
    // 30% towards QPalette::Highlight, which on Windows is the OS accent
    // colour and so is unrelated to the skin. Naming the file for the Selected
    // mode as well makes QSvgIconEngine load it directly instead of generating
    // a tinted variant, so an icon a skin ships is drawn in the colour it was
    // authored in. Disabled is deliberately left to be generated, so disabled
    // items still grey out.
    icon.addFile(overridePath, QSize(), QIcon::Selected, QIcon::Off);
    icon.addFile(overridePath, QSize(), QIcon::Selected, QIcon::On);
    return icon;
}

LibraryFeature::LibraryFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig,
        const QString& iconName)
        : QObject(pLibrary),
          m_pLibrary(pLibrary),
          m_pConfig(pConfig),
          m_iconName(iconName) {
    if (!m_iconName.isEmpty()) {
        m_icon = iconForName(m_pConfig, m_iconName);
    }
}

void LibraryFeature::selectAndActivate(const QModelIndex& index) {
    if (index.isValid()) {
        emit featureSelect(this, index);
        activateChild(index);
    } else {
        // calling featureSelect with invalid index will select the root item
        emit featureSelect(this, QModelIndex());
        activate();
    }
}

QStringList LibraryFeature::getPlaylistFiles(QFileDialog::FileMode mode) const {
    QString lastPlaylistDirectory = m_pConfig->getValue(
            ConfigKey("[Library]", "LastImportExportPlaylistDirectory"),
            QStandardPaths::writableLocation(QStandardPaths::MusicLocation));

    QFileDialog dialog(nullptr,
            tr("Import Playlist"),
            lastPlaylistDirectory,
            tr("Playlist Files (*.m3u *.m3u8 *.pls *.csv)"));
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(mode);
    dialog.setModal(true);

    // If the user refuses return
    if (!dialog.exec()) {
        return QStringList();
    }
    return dialog.selectedFiles();
}

bool LibraryFeature::exportPlaylistItemsIntoFile(
        QString playlistFilePath,
        const QList<QString>& playlistItemLocations,
        bool useRelativePath)    {
    if (playlistFilePath.endsWith(
            QStringLiteral(".pls"),
            Qt::CaseInsensitive)) {
        return ParserPls::writePLSFile(
                playlistFilePath,
                playlistItemLocations,
                useRelativePath);
    } else if (playlistFilePath.endsWith(
            QStringLiteral(".m3u8"),
            Qt::CaseInsensitive)) {
        return ParserM3u::writeM3U8File(
                playlistFilePath,
                playlistItemLocations,
                useRelativePath);
    } else {
        //default export to M3U if file extension is missing
        if (!playlistFilePath.endsWith(
                QStringLiteral(".m3u"),
                Qt::CaseInsensitive)) {
            kLogger.debug()
                    << "No valid file extension for playlist export specified."
                    << "Appending .m3u and exporting to M3U.";
            playlistFilePath.append(QStringLiteral(".m3u"));
            if (QFileInfo::exists(playlistFilePath)) {
                auto overwrite = QMessageBox::question(
                        nullptr,
                        tr("Overwrite File?"),
                        tr("A playlist file with the name \"%1\" already exists.\n"
                           "The default \"m3u\" extension was added because none was specified.\n\n"
                           "Do you really want to overwrite it?")
                                .arg(playlistFilePath));
                if (overwrite != QMessageBox::StandardButton::Yes) {
                    return false;
                }
            }
        }
        return ParserM3u::writeM3UFile(
                playlistFilePath,
                playlistItemLocations,
                useRelativePath);
    }
}
