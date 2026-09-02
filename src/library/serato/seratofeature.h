#pragma once
// seratofeature.h
// Created 2020-01-31 by Jan Holthuis
//
// This feature reads tracks and crates from removable Serato Libraries,
// either in the Music directory or on removable devices (USB drives, etc),
// by parsing the contents of the _Serato_ directory on each device.
//
// Most of the groundwork for this has been done here:
//
//      https://github.com/Holzhaus/serato-tags
//      https://github.com/Holzhaus/serato-tags/blob/main/scripts/database_v2.py

#include <QFuture>
#include <QFutureWatcher>

#include "library/baseexternallibraryfeature.h"
#include "util/parented_ptr.h"

class SeratoPlaylistModel;
class BaseTrackCache;

/// What parsing one Serato database produced.
///
/// The crate items come back detached from any model. Parsing happens on a
/// worker thread, so it must not touch the tree the GUI thread is drawing;
/// the items are attached once the result is back on the GUI thread.
struct SeratoDatabaseParseResult {
    /// Playlist holding every track of the database, to show once parsed.
    QString databasePlaylistPath;
    /// The database's crates. Ownership passes to whoever receives the
    /// result. These are raw pointers because a QFuture result has to be
    /// copyable, which is the same reason findSeratoDatabases() returns
    /// them this way.
    QList<TreeItem*> crateItems;
};

class SeratoFeature : public BaseExternalLibraryFeature {
    Q_OBJECT
  public:
    SeratoFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~SeratoFeature() override;

    QVariant title() override;
    static bool isSupported();
    void bindLibraryWidget(WLibrary* libraryWidget,
            KeyboardEventFilter* keyboard) override;

    TreeItemModel* sidebarModel() const override;

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;
    void refreshLibraryModels();
    void onSeratoDatabasesFound();
    void onTracksFound();

  private slots:
    void htmlLinkClicked(const QUrl& link);

  private:
    QString formatRootViewHtml() const;
    std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data) override;

    parented_ptr<TreeItemModel> m_pSidebarModel;
    SeratoPlaylistModel* m_pSeratoPlaylistModel;

    QFutureWatcher<QList<TreeItem*>> m_databasesFutureWatcher;
    QFuture<QList<TreeItem*>> m_databasesFuture;
    QFutureWatcher<SeratoDatabaseParseResult> m_tracksFutureWatcher;
    QFuture<SeratoDatabaseParseResult> m_tracksFuture;
    /// Label of the database being parsed, so that the crates it produces
    /// can be attached to the right item once the parse finishes. Held as a
    /// label rather than an index or a pointer, neither of which survives
    /// the tree being rebuilt while the parse is in flight.
    QString m_parsingDatabaseLabel;
    QString m_title;

    QSharedPointer<BaseTrackCache> m_trackSource;
};
