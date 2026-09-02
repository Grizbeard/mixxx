#pragma once

#include <QAction>
#include <QModelIndex>
#include <QPointer>
#include <memory>

#include "library/dao/playlistdao.h"
#include "library/libraryfeature.h"
#include "library/trackset/crate/crateid.h"
#include "util/parented_ptr.h"

class BaseSqlTableModel;
class TrackCollection;
class TreeItem;

class BaseExternalLibraryFeature : public LibraryFeature {
    Q_OBJECT
  public:
    BaseExternalLibraryFeature(
            Library* pLibrary,
            UserSettingsPointer pConfig,
            const QString& iconName);
    ~BaseExternalLibraryFeature() override = default;

  public slots:
    void bindSidebarWidget(WLibrarySidebar* pSidebarWidget) override;
    void onRightClick(const QPoint& globalPos) override;
    void onRightClickChild(const QPoint& globalPos, const QModelIndex& index) override;

  protected:
    // Must be re-implemented by external Libraries copied to Mixxx DB
    virtual std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data);
    // Must be implemented by external Libraries not copied to Mixxx DB
    virtual void appendTrackIdsFromRightClickIndex(QList<TrackId>* trackIds,
            QString* pPlaylist);

    /// Whether importing an item that has children should recreate that
    /// structure as nested Mixxx crates instead of flattening it into a
    /// single crate. Off by default, so a library only takes the nested path
    /// once it has been checked against that library's own tree.
    virtual bool preservesStructureOnCrateImport() const {
        return false;
    }

    /// A model over the tracks belonging to this item alone, excluding
    /// anything nested below it. Defaults to the model the item displays,
    /// which is already correct wherever a parent item shows only its own
    /// tracks. A library whose parent items display an aggregate has to
    /// override this or every level would import the same tracks again.
    virtual std::unique_ptr<BaseSqlTableModel> createPlaylistModelForItemItself(
            const QVariant& data);

  private slots:
    void slotAddToAutoDJ();
    void slotAddToAutoDJTop();
    void slotAddToAutoDJReplace();
    void slotImportAsMixxxPlaylist();
    void slotImportAsMixxxCrate();

  protected:
    QModelIndex lastRightClickedIndex() const {
        return m_lastRightClickedIndex;
    }
    void clearLastRightClickedIndex() {
        m_lastRightClickedIndex = QModelIndex();
    };

    TrackCollection* const m_pTrackCollection;

  private:
    void addToAutoDJ(PlaylistDAO::AutoDJSendLoc loc);

    /// One item of a subtree that is being imported, captured up front so
    /// that the import never reads a TreeItem again once it has started.
    struct CrateImportItem {
        QString label;
        QVariant data;
        /// Position of this item's parent in the list, or -1 for the item the
        /// import started from. A parent always precedes its children.
        int parentIndex;
        QList<TrackId> trackIds;
    };

    /// The Mixxx track ids for the tracks this item holds itself, adding any
    /// that are not in the library yet. label is only used for logging.
    QList<TrackId> collectTrackIdsForItemItself(
            const QVariant& data, const QString& label);

    /// Flatten the subtree rooted at pTreeItem into items, parents first.
    ///
    /// The tree is copied rather than walked lazily because the import shows
    /// a dialog and writes to the database, and both of those let the library
    /// tree be rebuilt underneath us. A TreeItem pointer does not survive
    /// that; the captured label and data do.
    static void flattenSubtreeForImport(const TreeItem* pTreeItem,
            int parentIndex,
            QList<CrateImportItem>* pItems);

    // Caution: Make sure this is reset whenever the library tree is updated,
    // so that the internalPointer() does not become dangling
    QModelIndex m_lastRightClickedIndex;

    parented_ptr<QAction> m_pAddToAutoDJAction;
    parented_ptr<QAction> m_pAddToAutoDJTopAction;
    parented_ptr<QAction> m_pAddToAutoDJReplaceAction;
    parented_ptr<QAction> m_pImportAsMixxxPlaylistAction;
    parented_ptr<QAction> m_pImportAsMixxxCrateAction;

    QPointer<WLibrarySidebar> m_pSidebarWidget;
};
