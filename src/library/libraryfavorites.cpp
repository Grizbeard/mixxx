#include "library/libraryfavorites.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtDebug>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "library/libraryfeature.h"
#include "library/sidebarmodel.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "moc_libraryfavorites.cpp"
#include "widget/wlibrarysidebar.h"

namespace {

const QString kLibraryGroup = QStringLiteral("[Library]");
const QString kConfigGroup = QStringLiteral("[LibraryFavorites]");

ConfigKey slotKey(int slot, const QString& suffix) {
    return ConfigKey(kLibraryGroup,
            QStringLiteral("favorite_%1_%2").arg(slot + 1).arg(suffix));
}

ConfigKey configKey(int slot) {
    return ConfigKey(kConfigGroup, QStringLiteral("favorite_%1").arg(slot + 1));
}

void collectMatches(const TreeItem* pItem,
        const LibraryFavorite& favorite,
        QList<const TreeItem*>* pMatches) {
    for (const TreeItem* pChild : pItem->children()) {
        const bool matches = favorite.dataKey.isEmpty()
                ? mixxx::libraryfavorites::dataKey(pChild).isEmpty() &&
                        mixxx::libraryfavorites::labelPath(pChild) ==
                                favorite.labelPath
                : mixxx::libraryfavorites::dataKey(pChild) == favorite.dataKey;
        if (matches) {
            pMatches->append(pChild);
        }
        collectMatches(pChild, favorite, pMatches);
    }
}

} // namespace

QString LibraryFavorite::toString() const {
    QJsonObject object;
    object.insert(QStringLiteral("feature"), feature);
    object.insert(QStringLiteral("data"), dataKey);
    object.insert(QStringLiteral("path"), QJsonArray::fromStringList(labelPath));
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// static
LibraryFavorite LibraryFavorite::fromString(const QString& text) {
    const QJsonObject object = QJsonDocument::fromJson(text.toUtf8()).object();
    LibraryFavorite favorite;
    favorite.feature = object.value(QStringLiteral("feature")).toString();
    favorite.dataKey = object.value(QStringLiteral("data")).toString();
    const QJsonArray path = object.value(QStringLiteral("path")).toArray();
    for (const auto& label : path) {
        favorite.labelPath.append(label.toString());
    }
    return favorite;
}

namespace mixxx {
namespace libraryfavorites {

QString dataKey(const TreeItem* pItem) {
    const QVariant& data = pItem->getData();
    if (!data.isValid()) {
        return QString();
    }
    const QVariantList list = data.toList();
    if (!list.isEmpty()) {
        return list.first().toString();
    }
    return data.toString();
}

QStringList labelPath(const TreeItem* pItem) {
    QStringList path;
    for (const TreeItem* pNode = pItem; pNode && !pNode->isRoot();
            pNode = pNode->parent()) {
        path.prepend(pNode->getLabel());
    }
    return path;
}

LibraryFavorite favoriteFor(const QString& feature, const TreeItem* pItem) {
    LibraryFavorite favorite;
    favorite.feature = feature;
    if (pItem && !pItem->isRoot()) {
        favorite.dataKey = dataKey(pItem);
        favorite.labelPath = labelPath(pItem);
    }
    return favorite;
}

const TreeItem* find(const TreeItem* pRoot, const LibraryFavorite& favorite) {
    if (!pRoot || !favorite.isValid()) {
        return nullptr;
    }
    if (favorite.isFeatureRoot()) {
        return pRoot;
    }
    QList<const TreeItem*> matches;
    collectMatches(pRoot, favorite, &matches);
    if (matches.size() <= 1) {
        return matches.isEmpty() ? nullptr : matches.first();
    }
    // Several items share the data: the label path decides. If no label
    // matches any more (a count in it changed), the first is as good a guess
    // as any, and better than doing nothing.
    for (const TreeItem* pMatch : std::as_const(matches)) {
        if (labelPath(pMatch) == favorite.labelPath) {
            return pMatch;
        }
    }
    return matches.first();
}

} // namespace libraryfavorites
} // namespace mixxx

LibraryFavorites::LibraryFavorites(UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_pConfig(std::move(pConfig)),
          m_pSidebarWidget(nullptr),
          m_pRecalls(std::make_unique<ControlObject>(
                  ConfigKey(kLibraryGroup, QStringLiteral("favorite_recalls")))) {
    m_pRecalls->setReadOnly();
    for (int slot = 0; slot < kCount; ++slot) {
        m_favorites.push_back(LibraryFavorite::fromString(
                m_pConfig->getValueString(configKey(slot))));

        auto pStore = std::make_unique<ControlPushButton>(slotKey(slot, QStringLiteral("store")));
        connect(pStore.get(),
                &ControlObject::valueChanged,
                this,
                [this, slot](double value) {
                    if (value > 0) {
                        store(slot);
                    }
                });
        m_storeControls.push_back(std::move(pStore));

        auto pActivate = std::make_unique<ControlPushButton>(
                slotKey(slot, QStringLiteral("activate")));
        connect(pActivate.get(),
                &ControlObject::valueChanged,
                this,
                [this, slot](double value) {
                    if (value > 0) {
                        activate(slot);
                    }
                });
        m_activateControls.push_back(std::move(pActivate));

        auto pStatus = std::make_unique<ControlObject>(slotKey(slot, QStringLiteral("status")));
        pStatus->setReadOnly();
        pStatus->forceSet(m_favorites.back().isValid() ? 1.0 : 0.0);
        m_statusControls.push_back(std::move(pStatus));
    }
}

LibraryFavorites::~LibraryFavorites() = default;

void LibraryFavorites::bindSidebarWidget(WLibrarySidebar* pSidebarWidget) {
    if (m_pSidebarWidget) {
        disconnect(m_pSidebarWidget, nullptr, this, nullptr);
    }
    m_pSidebarWidget = pSidebarWidget;
    if (m_pSidebarWidget) {
        connect(m_pSidebarWidget,
                &QObject::destroyed,
                this,
                &LibraryFavorites::slotSidebarWidgetDeleted);
    }
}

void LibraryFavorites::slotSidebarWidgetDeleted() {
    m_pSidebarWidget = nullptr;
}

SidebarModel* LibraryFavorites::sidebarModel() const {
    return m_pSidebarWidget ? qobject_cast<SidebarModel*>(m_pSidebarWidget->model())
                            : nullptr;
}

void LibraryFavorites::store(int slot) {
    SidebarModel* pModel = sidebarModel();
    if (!pModel) {
        return;
    }
    const QModelIndex index = m_pSidebarWidget->selectedIndex();
    LibraryFeature* pFeature = pModel->featureForIndex(index);
    if (!pFeature) {
        qDebug() << "LibraryFavorites: nothing selected to store as favorite" << slot + 1;
        return;
    }
    const TreeItem* pItem = index.internalPointer() == pModel
            ? nullptr // a feature's own row
            : static_cast<const TreeItem*>(index.internalPointer());
    const LibraryFavorite favorite =
            mixxx::libraryfavorites::favoriteFor(pFeature->iconName(), pItem);
    m_favorites[slot] = favorite;
    m_pConfig->setValue(configKey(slot), favorite.toString());
    m_statusControls[slot]->forceSet(1.0);
    qInfo() << "LibraryFavorites: stored favorite" << slot + 1 << favorite.toString();
}

void LibraryFavorites::activate(int slot) {
    SidebarModel* pModel = sidebarModel();
    const LibraryFavorite& favorite = m_favorites[slot];
    if (!pModel || !favorite.isValid()) {
        return;
    }
    for (LibraryFeature* pFeature : pModel->features()) {
        if (pFeature->iconName() != favorite.feature) {
            continue;
        }
        const TreeItemModel* pTreeModel = pFeature->sidebarModel();
        const TreeItem* pItem = mixxx::libraryfavorites::find(
                pTreeModel ? pTreeModel->getRootItem() : nullptr, favorite);
        if (!pItem) {
            break;
        }
        const QModelIndex index = pItem->isRoot()
                ? pModel->getFeatureRootIndex(pFeature)
                : pModel->indexForTreeItem(const_cast<TreeItem*>(pItem));
        if (!index.isValid()) {
            break;
        }
        m_pSidebarWidget->activateIndex(index);
        m_pRecalls->forceSet(m_pRecalls->get() + 1);
        return;
    }
    qDebug() << "LibraryFavorites: favorite" << slot + 1 << "is not in the sidebar"
             << favorite.toString();
}
