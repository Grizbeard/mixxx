#include "library/browse/browsetablemodel.h"

#include <QMessageBox>
#include <QMimeData>
#include <QStringList>
#include <QUrl>

#include "library/browse/browsetablemodel.h"
#include "library/browse/browsethread.h"
#include "library/columncache.h"
#include "library/dao/trackschema.h"
#include "library/tabledelegates/previewbuttondelegate.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_browsetablemodel.cpp"
#include "recording/recordingmanager.h"
#include "track/track.h"
#include "util/clipboard.h"
#include "widget/wlibrarytableview.h"

namespace {

/// Helper to insert values into a QList with specific indices.
///
/// *For legacy code only - Do not use for new code!*
/// Untranslated name that identifies a column in the persisted header state.
/// Columns which show the same thing as a column of the library use the same
/// name as the library, so that a column layout shared by all views applies to
/// both of them.
QString columnName(int column) {
    switch (column) {
    case COLUMN_PREVIEW:
        return LIBRARYTABLE_PREVIEW;
    case COLUMN_FILENAME:
        return TRACKLOCATIONSTABLE_FILENAME;
    case COLUMN_ARTIST:
        return LIBRARYTABLE_ARTIST;
    case COLUMN_TITLE:
        return LIBRARYTABLE_TITLE;
    case COLUMN_ALBUM:
        return LIBRARYTABLE_ALBUM;
    case COLUMN_TRACK_NUMBER:
        return LIBRARYTABLE_TRACKNUMBER;
    case COLUMN_YEAR:
        return LIBRARYTABLE_YEAR;
    case COLUMN_GENRE:
        return LIBRARYTABLE_GENRE;
    case COLUMN_COMPOSER:
        return LIBRARYTABLE_COMPOSER;
    case COLUMN_COMMENT:
        return LIBRARYTABLE_COMMENT;
    case COLUMN_DURATION:
        return LIBRARYTABLE_DURATION;
    case COLUMN_BPM:
        return LIBRARYTABLE_BPM;
    case COLUMN_KEY:
        return LIBRARYTABLE_KEY;
    case COLUMN_TYPE:
        return LIBRARYTABLE_FILETYPE;
    case COLUMN_BITRATE:
        return LIBRARYTABLE_BITRATE;
    case COLUMN_NATIVELOCATION:
        return TRACKLOCATIONSTABLE_LOCATION;
    case COLUMN_ALBUMARTIST:
        return LIBRARYTABLE_ALBUMARTIST;
    case COLUMN_GROUPING:
        return LIBRARYTABLE_GROUPING;
    case COLUMN_REPLAYGAIN:
        return LIBRARYTABLE_REPLAYGAIN;
    // The library has no columns for these.
    case COLUMN_FILE_MODIFIED_TIME:
        return QStringLiteral("file_modified");
    case COLUMN_FILE_CREATION_TIME:
        return QStringLiteral("file_created");
    default:
        return QString();
    }
}

/// Same widths the library uses for the columns it also has.
int defaultColumnWidth(int column) {
    const int width = ColumnCache::defaultColumnWidth();
    switch (column) {
    case COLUMN_PREVIEW:
        return width / 2;
    case COLUMN_BPM:
    case COLUMN_REPLAYGAIN:
        return width * 2;
    case COLUMN_ARTIST:
    case COLUMN_TITLE:
    case COLUMN_ALBUM:
    case COLUMN_ALBUMARTIST:
    case COLUMN_GENRE:
    case COLUMN_COMPOSER:
    case COLUMN_GROUPING:
    case COLUMN_FILENAME:
        return width * 4;
    case COLUMN_COMMENT:
    case COLUMN_NATIVELOCATION:
        return width * 6;
    case COLUMN_FILE_MODIFIED_TIME:
    case COLUMN_FILE_CREATION_TIME:
        return width * 3;
    default:
        return width;
    }
}

template<typename T>
void listAppendOrReplaceAt(QList<T>* pList, int index, const T& value) {
    VERIFY_OR_DEBUG_ASSERT(index <= pList->size()) {
        qWarning() << "listAppendOrReplaceAt: Padding list with"
                   << (index - pList->size()) << "default elements";
        while (index > pList->size()) {
            pList->append(T());
        }
    }
    VERIFY_OR_DEBUG_ASSERT(index == pList->size()) {
        pList->replace(index, value);
        return;
    }
    pList->append(value);
}

} // anonymous namespace

BrowseTableModel::BrowseTableModel(QObject* parent,
        TrackCollectionManager* pTrackCollectionManager,
        RecordingManager* pRecordingManager,
        const char* nameSpace)
        : TrackModel(pTrackCollectionManager->internalCollection()->database(),
                  nameSpace),
          QStandardItemModel(parent),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_pRecordingManager(pRecordingManager),
          m_previewDeckGroup(PlayerManager::groupForPreviewDeck(0)) {
    QStringList headerLabels;
    /// The order of the columns appended here must exactly match the ordering
    /// of the enum that is used for indexing.
    listAppendOrReplaceAt(&headerLabels, COLUMN_PREVIEW, tr("Preview"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_FILENAME, tr("Filename"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_ARTIST, tr("Artist"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_TITLE, tr("Title"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_ALBUM, tr("Album"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_TRACK_NUMBER, tr("Track #"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_YEAR, tr("Year"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_GENRE, tr("Genre"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_COMPOSER, tr("Composer"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_COMMENT, tr("Comment"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_DURATION, tr("Duration"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_BPM, tr("BPM"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_KEY, tr("Key"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_TYPE, tr("Type"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_BITRATE, tr("Bitrate"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_NATIVELOCATION, tr("Location"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_ALBUMARTIST, tr("Album Artist"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_GROUPING, tr("Grouping"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_FILE_MODIFIED_TIME, tr("File Modified"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_FILE_CREATION_TIME, tr("File Created"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_REPLAYGAIN, tr("ReplayGain"));

    m_searchColumns = {
            COLUMN_FILENAME,
            COLUMN_ARTIST,
            COLUMN_ALBUM,
            COLUMN_TITLE,
            COLUMN_GENRE,
            COLUMN_COMPOSER,
            COLUMN_COMMENT,
            COLUMN_ALBUMARTIST,
            COLUMN_GROUPING};

    setDefaultSort(COLUMN_FILENAME, Qt::AscendingOrder);

    for (int i = 0; i < static_cast<int>(TrackModel::SortColumnId::IdMax); ++i) {
        m_columnIndexBySortColumnId[i] = -1;
    }
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Filename)] = COLUMN_FILENAME;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Artist)] = COLUMN_ARTIST;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Title)] = COLUMN_TITLE;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Album)] = COLUMN_ALBUM;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::AlbumArtist)] =
            COLUMN_ALBUMARTIST;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Year)] = COLUMN_YEAR;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Genre)] = COLUMN_GENRE;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Composer)] = COLUMN_COMPOSER;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Grouping)] = COLUMN_GROUPING;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TrackNumber)] =
            COLUMN_TRACK_NUMBER;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::FileType)] = COLUMN_TYPE;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::NativeLocation)] =
            COLUMN_NATIVELOCATION;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Comment)] = COLUMN_COMMENT;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Duration)] = COLUMN_DURATION;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::BitRate)] = COLUMN_BITRATE;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Bpm)] = COLUMN_BPM;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::ReplayGain)] =
            COLUMN_REPLAYGAIN;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Key)] = COLUMN_KEY;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Preview)] = COLUMN_PREVIEW;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Grouping)] = COLUMN_GROUPING;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::FileModifiedTime)] =
            COLUMN_FILE_MODIFIED_TIME;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::FileCreationTime)] =
            COLUMN_FILE_CREATION_TIME;

    m_sortColumnIdByColumnIndex.clear();
    for (int i = static_cast<int>(TrackModel::SortColumnId::IdMin);
            i < static_cast<int>(TrackModel::SortColumnId::IdMax);
            ++i) {
        TrackModel::SortColumnId sortColumn = static_cast<TrackModel::SortColumnId>(i);
        int columnIndex = m_columnIndexBySortColumnId[static_cast<int>(sortColumn)];
        if (columnIndex >= 0) {
            m_sortColumnIdByColumnIndex.insert(columnIndex, sortColumn);
        }
    }

    setHorizontalHeaderLabels(headerLabels);
    // register the QList<T> as a metatype since we use QueuedConnection below
    qRegisterMetaType<QList<QList<QStandardItem*>>>(
            "QList< QList<QStandardItem*>>");
    qRegisterMetaType<BrowseTableModel*>("BrowseTableModel*");

    m_pBrowseThread = BrowseThread::getInstanceRef();
    connect(m_pBrowseThread.data(),
            &BrowseThread::clearModel,
            this,
            &BrowseTableModel::slotClear,
            Qt::QueuedConnection);

    connect(m_pBrowseThread.data(),
            &BrowseThread::rowsAppended,
            this,
            &BrowseTableModel::slotInsert,
            Qt::QueuedConnection);

    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            &BrowseTableModel::trackChanged);
    trackChanged(m_previewDeckGroup,
            PlayerInfo::instance().getTrackInfo(m_previewDeckGroup),
            TrackPointer());
}

BrowseTableModel::~BrowseTableModel() {
}

int BrowseTableModel::columnIndexFromSortColumnId(TrackModel::SortColumnId column) const {
    if (column < TrackModel::SortColumnId::IdMin ||
            column >= TrackModel::SortColumnId::IdMax) {
        return -1;
    }

    return m_columnIndexBySortColumnId[static_cast<int>(column)];
}

TrackModel::SortColumnId BrowseTableModel::sortColumnIdFromColumnIndex(int index) const {
    return m_sortColumnIdByColumnIndex.value(index, TrackModel::SortColumnId::Invalid);
}

const QList<int>& BrowseTableModel::searchColumns() const {
    return m_searchColumns;
}

void BrowseTableModel::setPath(mixxx::FileAccess path) {
    VERIFY_OR_DEBUG_ASSERT(m_pBrowseThread) {
        return;
    }

    if (path.info().hasLocation() && path.info().isDir()) {
        m_currentDirectory = path.info().location();
        m_pBrowseThread->executePopulation(std::move(path), this);
    } else {
        m_currentDirectory = {};
        m_pBrowseThread->executePopulation({}, this);
    }
}

TrackPointer BrowseTableModel::getTrack(const QModelIndex& index) const {
    return getTrackByRef(TrackRef::fromFilePath(getTrackLocation(index)));
}

TrackPointer BrowseTableModel::getTrackByRef(const TrackRef& trackRef) const {
    if (m_pRecordingManager &&
            m_pRecordingManager->getRecordingLocation() ==
                    trackRef.getLocation()) {
        QMessageBox::critical(nullptr,
                tr("Mixxx Library"),
                tr("Could not load the following file because it is in use by "
                   "Mixxx or another application.") +
                        "\n" + trackRef.getLocation());
        return TrackPointer();
    }
    // NOTE(uklotzde, 2015-12-08): Accessing tracks from the browse view
    // will implicitly add them to the library. Is this really what we
    // want here??
    // NOTE(rryan, 2015-12-27): This was intentional at the time since
    // some people use Browse instead of the library and we want to let
    // them edit the tracks in a way that persists across sessions
    // and we didn't want to edit the files on disk by default
    // unless the user opts in to that.
    return m_pTrackCollectionManager->getOrAddTrack(trackRef);
}

QString BrowseTableModel::getTrackLocation(const QModelIndex& index) const {
    int row = index.row();

    QModelIndex index2 = this->index(row, COLUMN_NATIVELOCATION);
    QString nativeLocation = data(index2).toString();
    QString location = QDir::fromNativeSeparators(nativeLocation);
    return location;
}

TrackId BrowseTableModel::getTrackId(const QModelIndex& index) const {
    TrackPointer pTrack = getTrack(index);
    if (pTrack) {
        return pTrack->getId();
    } else {
        qWarning()
                << "Track is not available in library"
                << getTrackUrl(index);
        return TrackId();
    }
}

QUrl BrowseTableModel::getTrackUrl(const QModelIndex& index) const {
    const QString trackLocation = getTrackLocation(index);
    DEBUG_ASSERT(trackLocation.trimmed() == trackLocation);
    if (trackLocation.isEmpty()) {
        return {};
    }
    return QUrl::fromLocalFile(trackLocation);
}

CoverInfo BrowseTableModel::getCoverInfo(const QModelIndex& index) const {
    TrackPointer pTrack = getTrack(index);
    if (pTrack) {
        return CoverInfo(pTrack->getCoverInfo(), getTrackLocation(index));
    } else {
        qWarning()
                << "Track is not available in library"
                << getTrackUrl(index);
        return CoverInfo();
    }
}

const QVector<int> BrowseTableModel::getTrackRows(TrackId trackId) const {
    Q_UNUSED(trackId);
    // We can't implement this as it stands.
    return QVector<int>();
}

void BrowseTableModel::search(const QString&) {
}

const QString BrowseTableModel::currentSearch() const {
    return QString("");
}

QVariant BrowseTableModel::headerData(
        int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal) {
        // QStandardItemModel only knows the displayed labels, but the track
        // view needs a stable name and a default width per column.
        if (role == TrackModel::kHeaderNameRole) {
            return columnName(section);
        }
        if (role == TrackModel::kHeaderWidthRole) {
            return defaultColumnWidth(section);
        }
    }
    return QStandardItemModel::headerData(section, orientation, role);
}

bool BrowseTableModel::isColumnInternal(int) {
    return false;
}

bool BrowseTableModel::isColumnHiddenByDefault(int column) {
    if (column == COLUMN_COMPOSER ||
            column == COLUMN_TRACK_NUMBER ||
            column == COLUMN_YEAR ||
            column == COLUMN_GROUPING ||
            column == COLUMN_NATIVELOCATION ||
            column == COLUMN_ALBUMARTIST ||
            column == COLUMN_FILE_CREATION_TIME ||
            column == COLUMN_REPLAYGAIN) {
        return true;
    }
    return false;
}

void BrowseTableModel::moveTrack(const QModelIndex&, const QModelIndex&) {
}

void BrowseTableModel::copyTracks(const QModelIndexList& indices) const {
    Clipboard::start();
    for (const QModelIndex& index : indices) {
        if (index.isValid()) {
            Clipboard::add(QUrl::fromLocalFile(getTrackLocation(index)));
        }
    }
    Clipboard::finish();

    // TODO Investigate if we can also implement cut and paste (via QFile
    // operations) so mixxx could manage files in the filesystem, rather than
    // having to go switch between mixxx and the system file browser.
}

void BrowseTableModel::removeTracks(const QModelIndexList&) {
}

QMimeData* BrowseTableModel::mimeData(const QModelIndexList& indexes) const {
    QMimeData* mimeData = new QMimeData();
    QList<QUrl> urls;

    // Ok, so the list of indexes we're given contains separates indexes for
    // each column, so even if only one row is selected, we'll have like 7
    // indexes.  We need to only count each row once:
    QList<int> rows;

    foreach (QModelIndex index, indexes) {
        if (index.isValid()) {
            if (!rows.contains(index.row())) {
                rows.push_back(index.row());
                QUrl url = getTrackUrl(index);
                if (!url.isValid()) {
                    qDebug() << "ERROR invalid url" << url;
                    continue;
                }
                urls.append(url);
            }
        }
    }
    mimeData->setUrls(urls);
    return mimeData;
}

void BrowseTableModel::slotClear(BrowseTableModel* caller_object) {
    if (caller_object == this) {
        removeRows(0, rowCount());
    }
}

void BrowseTableModel::slotInsert(const BrowseTableItems& rows,
        BrowseTableModel* caller_object) {
    // There exists more than one BrowseTableModel in Mixxx and we only want to
    // receive items this object has 'ordered' from the BrowseThread (singleton)
    if (caller_object == this) {
        emit saveModelState();
        //qDebug() << "BrowseTableModel::slotInsert";
        for (int i = 0; i < rows.size(); ++i) {
            appendRow(rows.at(i));
        }
        emit restoreModelState();
    }
}

TrackModel::Capabilities BrowseTableModel::getCapabilities() const {
    return Capability::AddToTrackSet |
            Capability::AddToAutoDJ |
            Capability::LoadToDeck |
            Capability::LoadToPreviewDeck |
            Capability::LoadToSampler |
            Capability::RemoveFromDisk |
            Capability::Sorting;
}

QString BrowseTableModel::modelKey(bool noSearch) const {
    // Searching is handled by the proxy model, so if there is an active search
    // modelkey is composed there, too.
    Q_UNUSED(noSearch);
    return QStringLiteral("browse:") + m_currentDirectory;
}

Qt::ItemFlags BrowseTableModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags defaultFlags = QAbstractItemModel::flags(index);

    // Enable dragging songs from this data model to elsewhere (like the
    // waveform widget to load a track into a Player).
    defaultFlags |= Qt::ItemIsDragEnabled;

    int column = index.column();

#ifdef Q_OS_IOS
    // Make items non-editable on iOS by default, since tapping any track will
    // otherwise trigger the on-screen keyboard (even if they cannot actually
    // be edited).
    return defaultFlags;
#else
    switch (column) {
    case COLUMN_FILENAME:
    case COLUMN_BITRATE:
    case COLUMN_DURATION:
    case COLUMN_TYPE:
    case COLUMN_FILE_MODIFIED_TIME:
    case COLUMN_FILE_CREATION_TIME:
    case COLUMN_REPLAYGAIN:
        // read-only
        return defaultFlags;
    default:
        // editable
        return defaultFlags | Qt::ItemIsEditable;
    }
#endif
}

bool BrowseTableModel::setData(
        const QModelIndex& index,
        const QVariant& value,
        int role) {
    Q_UNUSED(role);

    QStandardItem* item = itemFromIndex(index);
    DEBUG_ASSERT(nullptr != item);

    TrackPointer pTrack(getTrack(index));
    if (!pTrack) {
        qWarning() << "BrowseTableModel::setData():"
                   << "Failed to resolve track"
                   << getTrackLocation(index);
        // restore previous item content
        item->setText(index.data().toString());
        item->setToolTip(item->text());
        return false;
    }

    // check if one the item were edited
    int col = index.column();
    switch (col) {
    case COLUMN_ARTIST:
        pTrack->setArtist(value.toString());
        break;
    case COLUMN_TITLE:
        pTrack->setTitle(value.toString());
        break;
    case COLUMN_ALBUM:
        pTrack->setAlbum(value.toString());
        break;
    case COLUMN_BPM:
        pTrack->trySetBpm(value.toDouble());
        break;
    case COLUMN_KEY:
        pTrack->setKeyText(value.toString());
        break;
    case COLUMN_TRACK_NUMBER:
        pTrack->setTrackNumber(value.toString());
        break;
    case COLUMN_COMMENT:
        pTrack->setComment(value.toString());
        break;
    case COLUMN_GENRE:
        m_pTrackCollectionManager->updateTrackGenre(pTrack.get(), value.toString());
        break;
    case COLUMN_COMPOSER:
        pTrack->setComposer(value.toString());
        break;
    case COLUMN_YEAR:
        pTrack->setYear(value.toString());
        break;
    case COLUMN_ALBUMARTIST:
        pTrack->setAlbumArtist(value.toString());
        break;
    case COLUMN_GROUPING:
        pTrack->setGrouping(value.toString());
        break;
    default:
        qWarning() << "BrowseTableModel::setData():"
                   << "No tagger column";
        // restore previous item context
        item->setText(index.data().toString());
        item->setToolTip(item->text());
        return false;
    }

    item->setText(value.toString());
    item->setToolTip(item->text());
    return true;
}

void BrowseTableModel::trackChanged(
        const QString& group, TrackPointer pNewTrack, TrackPointer pOldTrack) {
    Q_UNUSED(pOldTrack);
    if (group == m_previewDeckGroup) {
        for (int row = 0; row < rowCount(); ++row) {
            QModelIndex i = index(row, COLUMN_PREVIEW);
            if (i.data().toBool()) {
                QStandardItem* item = itemFromIndex(i);
                item->setText("0");
            }
        }
        if (pNewTrack) {
            QString trackLocation = pNewTrack->getLocation();
            for (int row = 0; row < rowCount(); ++row) {
                QModelIndex i = index(row, COLUMN_PREVIEW);
                QString location = getTrackLocation(i);
                if (location == trackLocation) {
                    QStandardItem* item = itemFromIndex(i);
                    item->setText("1");
                    break;
                }
            }
        }
    }
}

bool BrowseTableModel::isColumnSortable(int column) const {
    return COLUMN_PREVIEW != column;
}

QAbstractItemDelegate* BrowseTableModel::delegateForColumn(const int i, QObject* pParent) {
    WLibraryTableView* pTableView = qobject_cast<WLibraryTableView*>(pParent);
    DEBUG_ASSERT(pTableView);
    if (PlayerInfo::instance().numPreviewDecks() > 0 && i == COLUMN_PREVIEW) {
        return new PreviewButtonDelegate(pTableView, i);
    }
    return nullptr;
}

bool BrowseTableModel::updateTrackGenre(
        Track* pTrack,
        const QString& genre) const {
    return m_pTrackCollectionManager->updateTrackGenre(pTrack, genre);
}

#if defined(__EXTRA_METADATA__)
bool BrowseTableModel::updateTrackMood(
        Track* pTrack,
        const QString& mood) const {
    return m_pTrackCollectionManager->updateTrackMood(pTrack, mood);
}
#endif // __EXTRA_METADATA__

void BrowseTableModel::releaseBrowseThread() {
    // The shared browse thread is stopped in the destructor
    // if this is the last reference. All references must be reset before
    // the library is destructed.
    m_pBrowseThread.reset();
}
