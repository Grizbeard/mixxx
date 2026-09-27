#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

#include "preferences/usersettings.h"

class ControlObject;
class ControlPushButton;
class LibraryFeature;
class SidebarModel;
class TreeItem;
class WLibrarySidebar;

/// A sidebar item remembered well enough to find it again: which feature it
/// belongs to, and a key for the item within that feature's tree.
///
/// The key is the item's data where it has any -- a crate or playlist id, a
/// folder path, a Rekordbox or Serato playlist path -- because that survives
/// renames and the track counts some labels carry. The label path is kept as
/// well: it tells apart items that share their data (History's year folders
/// all carry the same placeholder id), and it is the only key for items that
/// have no data at all (the folder levels of nested Serato crates).
struct LibraryFavorite {
    QString feature;       // LibraryFeature::iconName()
    QString dataKey;       // empty for a feature root or an item without data
    QStringList labelPath; // labels from below the feature root to the item

    bool isValid() const {
        return !feature.isEmpty();
    }
    bool isFeatureRoot() const {
        return dataKey.isEmpty() && labelPath.isEmpty();
    }
    QString toString() const;
    static LibraryFavorite fromString(const QString& text);
};

namespace mixxx {
namespace libraryfavorites {

/// The item's data as text. A list (Rekordbox, Serato) is keyed by its first
/// element, the path: the second is a loaded/not-loaded flag that changes when
/// the item is first opened.
QString dataKey(const TreeItem* pItem);

/// Labels from the item up to, but not including, the root of its tree.
QStringList labelPath(const TreeItem* pItem);

LibraryFavorite favoriteFor(const QString& feature, const TreeItem* pItem);

/// The item in the tree under pRoot that `favorite` names, or nullptr if the
/// tree holds none -- a drive that has left, say, or a crate since deleted.
/// Only the part of the tree already loaded is searched. A feature-root
/// favorite finds pRoot itself.
const TreeItem* find(const TreeItem* pRoot, const LibraryFavorite& favorite);

} // namespace libraryfavorites
} // namespace mixxx

/// Numbered favorites for sidebar items, driven by controls so that a
/// controller can store and recall them:
///
///   [Library],favorite_N_store     remember the selected sidebar item as N
///   [Library],favorite_N_activate  select and open favorite N, as a click would
///   [Library],favorite_N_status    read-only: 0 nothing stored, 1 stored
///   [Library],favorite_recalls     read-only: counts successful recalls
///
/// N is 1..kCount. A recall whose item is not in the sidebar (its drive is not
/// plugged in, or not opened yet) does nothing, and the counter does not move;
/// a mapping can follow the counter to switch to the library only when there
/// is something to show. Favorites are kept in the config and survive a
/// restart.
class LibraryFavorites : public QObject {
    Q_OBJECT
  public:
    static constexpr int kCount = 6;

    LibraryFavorites(UserSettingsPointer pConfig, QObject* pParent);
    ~LibraryFavorites() override;

    void bindSidebarWidget(WLibrarySidebar* pSidebarWidget);

  private slots:
    void slotSidebarWidgetDeleted();

  private:
    void store(int slot);
    void activate(int slot);
    SidebarModel* sidebarModel() const;

    const UserSettingsPointer m_pConfig;
    WLibrarySidebar* m_pSidebarWidget;
    std::vector<LibraryFavorite> m_favorites;
    std::vector<std::unique_ptr<ControlPushButton>> m_storeControls;
    std::vector<std::unique_ptr<ControlPushButton>> m_activateControls;
    std::vector<std::unique_ptr<ControlObject>> m_statusControls;
    std::unique_ptr<ControlObject> m_pRecalls;
};
