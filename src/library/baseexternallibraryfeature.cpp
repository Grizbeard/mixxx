#include "library/baseexternallibraryfeature.h"

#include <QMenu>
#include <QMessageBox>

#include "library/basesqltablemodel.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/trackset/crate/crate.h"
#include "library/trackset/crate/cratefeaturehelper.h"
#include "library/treeitem.h"
#include "moc_baseexternallibraryfeature.cpp"
#include "util/logger.h"
#include "widget/wlibrarysidebar.h"

namespace {

const mixxx::Logger kLogger("BaseExternalLibraryFeature");

} // namespace

BaseExternalLibraryFeature::BaseExternalLibraryFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig,
        const QString& iconName)
        : LibraryFeature(pLibrary, pConfig, iconName),
          m_pTrackCollection(pLibrary->trackCollectionManager()->internalCollection()) {
    m_pAddToAutoDJAction = make_parented<QAction>(tr("Add to Auto DJ Queue (bottom)"), this);
    connect(m_pAddToAutoDJAction,
            &QAction::triggered,
            this,
            &BaseExternalLibraryFeature::slotAddToAutoDJ);

    m_pAddToAutoDJTopAction = make_parented<QAction>(tr("Add to Auto DJ Queue (top)"), this);
    connect(m_pAddToAutoDJTopAction,
            &QAction::triggered,
            this,
            &BaseExternalLibraryFeature::slotAddToAutoDJTop);

    m_pAddToAutoDJReplaceAction = make_parented<QAction>(tr("Add to Auto DJ Queue (replace)"), this);
    connect(m_pAddToAutoDJReplaceAction,
            &QAction::triggered,
            this,
            &BaseExternalLibraryFeature::slotAddToAutoDJReplace);

    m_pImportAsMixxxPlaylistAction = make_parented<QAction>(tr("Import as Playlist"), this);
    connect(m_pImportAsMixxxPlaylistAction,
            &QAction::triggered,
            this,
            &BaseExternalLibraryFeature::slotImportAsMixxxPlaylist);

    m_pImportAsMixxxCrateAction = make_parented<QAction>(tr("Import as Crate"), this);
    connect(m_pImportAsMixxxCrateAction,
            &QAction::triggered,
            this,
            &BaseExternalLibraryFeature::slotImportAsMixxxCrate);
}

void BaseExternalLibraryFeature::bindSidebarWidget(WLibrarySidebar* pSidebarWidget) {
    // store the sidebar widget pointer for later use in onRightClickChild
    m_pSidebarWidget = pSidebarWidget;
}

void BaseExternalLibraryFeature::onRightClick(const QPoint& globalPos) {
    Q_UNUSED(globalPos);
    m_lastRightClickedIndex = QModelIndex();
}

void BaseExternalLibraryFeature::onRightClickChild(
        const QPoint& globalPos, const QModelIndex& index) {
    // Save the model index so we can get it in the action slots...
    // Make sure that this is reset when the related TreeItem is deleted.
    m_lastRightClickedIndex = index;
    QMenu menu(m_pSidebarWidget);
    menu.addAction(m_pAddToAutoDJAction);
    menu.addAction(m_pAddToAutoDJTopAction);
    menu.addAction(m_pAddToAutoDJReplaceAction);
    menu.addSeparator();
    menu.addAction(m_pImportAsMixxxPlaylistAction);
    menu.addAction(m_pImportAsMixxxCrateAction);
    menu.exec(globalPos);
}

void BaseExternalLibraryFeature::slotAddToAutoDJ() {
    //qDebug() << "slotAddToAutoDJ() row:" << m_lastRightClickedIndex.data();
    addToAutoDJ(PlaylistDAO::AutoDJSendLoc::BOTTOM);
}

void BaseExternalLibraryFeature::slotAddToAutoDJTop() {
    //qDebug() << "slotAddToAutoDJTop() row:" << m_lastRightClickedIndex.data();
    addToAutoDJ(PlaylistDAO::AutoDJSendLoc::TOP);
}

void BaseExternalLibraryFeature::slotAddToAutoDJReplace() {
    //qDebug() << "slotAddToAutoDJReplace() row:" << m_lastRightClickedIndex.data();
    addToAutoDJ(PlaylistDAO::AutoDJSendLoc::REPLACE);
}

void BaseExternalLibraryFeature::addToAutoDJ(PlaylistDAO::AutoDJSendLoc loc) {
    //qDebug() << "slotAddToAutoDJ() row:" << m_lastRightClickedIndex.data();

    QList<TrackId> trackIds;
    QString playlist;
    appendTrackIdsFromRightClickIndex(&trackIds, &playlist);
    if (trackIds.isEmpty()) {
        return;
    }

    PlaylistDAO &playlistDao = m_pTrackCollection->getPlaylistDAO();
    playlistDao.addTracksToAutoDJQueue(trackIds, loc);
}

void BaseExternalLibraryFeature::slotImportAsMixxxPlaylist() {
    // qDebug() << "slotImportAsMixxxPlaylist() row:" << m_lastRightClickedIndex.data();

    QList<TrackId> trackIds;
    QString playlist;
    appendTrackIdsFromRightClickIndex(&trackIds, &playlist);
    if (trackIds.isEmpty()) {
        return;
    }

    PlaylistDAO& playlistDao = m_pTrackCollection->getPlaylistDAO();

    int playlistId = playlistDao.createUniquePlaylist(&playlist);

    if (playlistId != kInvalidPlaylistId) {
        playlistDao.appendTracksToPlaylist(trackIds, playlistId);
    } else {
        // Do not change strings here without also changing strings in
        // src/library/trackset/baseplaylistfeature.cpp
        QMessageBox::warning(nullptr,
                tr("Playlist Creation Failed"),
                tr("An unknown error occurred while creating playlist: ") + playlist);
    }
}

std::unique_ptr<BaseSqlTableModel>
BaseExternalLibraryFeature::createPlaylistModelForItemItself(const QVariant& data) {
    return createPlaylistModelForPlaylist(data);
}

QList<TrackId> BaseExternalLibraryFeature::collectTrackIdsForItemItself(
        const QVariant& data, const QString& label) {
    // Only the tracks this item holds itself. The items below it are imported
    // as their own crates, and a Mixxx crate already displays what is nested
    // inside it, so taking an aggregate here would repeat the same tracks at
    // every level.
    QList<TrackId> trackIds;
    const std::unique_ptr<BaseSqlTableModel> pModel =
            createPlaylistModelForItemItself(data);
    if (!pModel || !pModel->initialized()) {
        // Normal for a folder the source library keeps no track list for.
        kLogger.debug() << "No track list of its own for" << label;
        return trackIds;
    }

    // The model holds no rows until it is sorted and selected, matching what
    // appendTrackIdsFromRightClickIndex() does.
    pModel->setSort(
            pModel->fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION),
            Qt::AscendingOrder);
    pModel->select();

    const int rowCount = pModel->rowCount();
    trackIds.reserve(rowCount);
    for (int row = 0; row < rowCount; ++row) {
        const QModelIndex index = pModel->index(row, 0);
        VERIFY_OR_DEBUG_ASSERT(index.isValid()) {
            continue;
        }
        // Resolving a track adds it to the library when it is not there yet,
        // which is why this is the slow part of an import.
        const TrackId trackId = pModel->getTrackId(index);
        if (trackId.isValid()) {
            trackIds.append(trackId);
        }
    }
    kLogger.info() << "Collected" << trackIds.size() << "of" << rowCount
                   << "tracks for" << label;
    return trackIds;
}

void BaseExternalLibraryFeature::flattenSubtreeForImport(
        const TreeItem* pTreeItem, int parentIndex, QList<CrateImportItem>* pItems) {
    const int index = pItems->size();
    CrateImportItem item;
    item.label = pTreeItem->getLabel();
    item.data = pTreeItem->getData();
    item.parentIndex = parentIndex;
    pItems->append(std::move(item));

    for (const TreeItem* pChild : pTreeItem->children()) {
        flattenSubtreeForImport(pChild, index, pItems);
    }
}

void BaseExternalLibraryFeature::slotImportAsMixxxCrate() {
    // qDebug() << "slotImportAsMixxxCrate() row:" << m_lastRightClickedIndex.data();

    const auto* pTreeItem = m_lastRightClickedIndex.isValid()
            ? static_cast<const TreeItem*>(m_lastRightClickedIndex.internalPointer())
            : nullptr;
    if (preservesStructureOnCrateImport() && pTreeItem && pTreeItem->hasChildren()) {
        // Copy the subtree before anything else. Showing a dialog and writing
        // to the database both give the library tree a chance to be rebuilt,
        // and every TreeItem pointer would then be dangling.
        QList<CrateImportItem> items;
        flattenSubtreeForImport(pTreeItem, -1, &items);
        pTreeItem = nullptr;
        clearLastRightClickedIndex();

        const auto answer = QMessageBox::question(nullptr,
                tr("Import Crate Structure"),
                tr("Import \"%1\" and everything inside it as %n Mixxx "
                   "crate(s), keeping the structure?",
                        "",
                        items.size())
                        .arg(items.first().label),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::Yes);
        if (answer != QMessageBox::Yes) {
            return;
        }

        // Resolve every track before creating any crate. Reading these models
        // adds the missing tracks to the library, and interleaving that with
        // crate writes means each write is reacted to while another model is
        // mid-read. The flat import below reads everything first for the same
        // reason.
        kLogger.info() << "Importing a crate tree of" << items.size() << "items";
        for (CrateImportItem& item : items) {
            item.trackIds = collectTrackIdsForItemItself(item.data, item.label);
        }

        kLogger.info() << "Creating" << items.size() << "crates";
        QList<CrateId> crateIds;
        crateIds.reserve(items.size());
        int createdCount = 0;
        for (const CrateImportItem& item : items) {
            // A parent always precedes its children, so its crate id is known
            // by the time a child needs it.
            CrateId parentCrateId;
            if (item.parentIndex >= 0) {
                parentCrateId = crateIds.at(item.parentIndex);
            }

            // Crate names are unique across the collection rather than among
            // siblings, so two folders that both hold a "Warmup" arrive as
            // "Warmup" and "Warmup 2". An empty label would otherwise make a
            // crate with no name, which repairDatabase() deletes on sight.
            const QString label = item.label.trimmed();
            Crate crate;
            crate.setName(CrateFeatureHelper(m_pTrackCollection, m_pConfig)
                            .proposeNameForNewCrate(label.isEmpty()
                                            ? tr("Imported Crate")
                                            : label));
            crate.setParentId(parentCrateId);

            CrateId crateId;
            if (m_pTrackCollection->insertCrate(crate, &crateId)) {
                if (!item.trackIds.isEmpty()) {
                    m_pTrackCollection->addCrateTracks(crateId, item.trackIds);
                }
                ++createdCount;
            } else {
                kLogger.warning() << "Failed to create crate" << crate.getName()
                                  << "while importing a crate tree";
            }
            // Recorded even when invalid, so that the parent indices of the
            // items that follow still line up.
            crateIds.append(crateId);
        }

        kLogger.info() << "Created" << createdCount << "of" << items.size() << "crates";
        if (createdCount < items.size()) {
            QMessageBox::warning(nullptr,
                    tr("Crate Creation Failed"),
                    tr("Only %1 of %2 crates could be created.")
                            .arg(QString::number(createdCount),
                                    QString::number(items.size())));
        }
        return;
    }

    QList<TrackId> trackIds;
    QString playlist;
    appendTrackIdsFromRightClickIndex(&trackIds, &playlist);
    if (trackIds.isEmpty()) {
        return;
    }

    Crate crate;
    crate.setName(playlist);

    CrateId crateId;

    if (m_pTrackCollection->insertCrate(crate, &crateId)) {
        m_pTrackCollection->addCrateTracks(crateId, trackIds);
    } else {
        QMessageBox::warning(nullptr,
                tr("Crate Creation Failed"),
                tr("Could not create crate, it most likely already exists: ") + playlist);
    }
}

// This is a common function for all external libraries copied to Mixxx DB
void BaseExternalLibraryFeature::appendTrackIdsFromRightClickIndex(
        QList<TrackId>* trackIds, QString* pPlaylist) {
    if (!m_lastRightClickedIndex.isValid()) {
        return;
    }

    const auto* pTreeItem = static_cast<TreeItem*>(
            m_lastRightClickedIndex.internalPointer());
    VERIFY_OR_DEBUG_ASSERT(pTreeItem) {
        return;
    }

    DEBUG_ASSERT(pPlaylist);
    *pPlaylist = pTreeItem->getLabel();
    const std::unique_ptr<BaseSqlTableModel> pPlaylistModelToAdd =
            createPlaylistModelForPlaylist(pTreeItem->getData());

    if (!pPlaylistModelToAdd || !pPlaylistModelToAdd->initialized()) {
        qDebug() << "BaseExternalLibraryFeature::"
                    "appendTrackIdsFromRightClickIndex "
                    "could not initialize a playlist model for "
                    "playlist:"
                 << *pPlaylist;
        return;
    }

    pPlaylistModelToAdd->setSort(
            pPlaylistModelToAdd->fieldIndex(
                    ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION),
            Qt::AscendingOrder);
    pPlaylistModelToAdd->select();

    // Copy Tracks
    const int rows = pPlaylistModelToAdd->rowCount();
    for (int i = 0; i < rows; ++i) {
        QModelIndex index = pPlaylistModelToAdd->index(i, 0);
        VERIFY_OR_DEBUG_ASSERT(index.isValid()) {
            continue;
        }
        const TrackId trackId = pPlaylistModelToAdd->getTrackId(index);
        if (!trackId.isValid()) {
            kLogger.warning()
                    << "Failed to add track"
                    << pPlaylistModelToAdd->getTrackUrl(index)
                    << "to playlist"
                    << *pPlaylist;
            continue;
        }
        if (kLogger.traceEnabled()) {
            kLogger.trace()
                    << "Adding track"
                    << pPlaylistModelToAdd->getTrackUrl(index)
                    << "to playlist"
                    << *pPlaylist;
        }
        trackIds->append(trackId);
    }
}

std::unique_ptr<BaseSqlTableModel>
BaseExternalLibraryFeature::createPlaylistModelForPlaylist(
        const QVariant&) {
    return {};
}
