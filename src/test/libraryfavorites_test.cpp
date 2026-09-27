#include "library/libraryfavorites.h"

#include <gtest/gtest.h>

#include <QStringList>
#include <QVariantList>

#include "library/treeitem.h"

namespace {

using namespace mixxx::libraryfavorites;

// A sidebar tree shaped like the ones the features build, with no feature
// behind it: the matching only looks at labels and data.
class LibraryFavoritesTest : public testing::Test {
  protected:
    TreeItem m_root;
};

TEST_F(LibraryFavoritesTest, KeysAnItemByItsData) {
    TreeItem* pCrate = m_root.appendChild(QStringLiteral("House (12)"), 42);
    EXPECT_EQ(QStringLiteral("42"), dataKey(pCrate));
    EXPECT_EQ(QStringList{QStringLiteral("House (12)")}, labelPath(pCrate));
}

TEST_F(LibraryFavoritesTest, KeysAListByItsPathNotItsLoadedFlag) {
    // Rekordbox and Serato keep a path and a flag that flips once the item
    // has been opened. The flag must not be part of the key.
    TreeItem* pDevice = m_root.appendChild(QStringLiteral("GRIZ_T7"),
            QVariant(QList<QString>{QStringLiteral("/run/media/griz/GRIZ_T7"),
                    QStringLiteral("IS_RECORDBOX_DEVICE")}));
    const LibraryFavorite before = favoriteFor(QStringLiteral("rekordbox"), pDevice);
    pDevice->setData(QVariant(QList<QString>{QStringLiteral("/run/media/griz/GRIZ_T7"),
            QStringLiteral("IS_NOT_RECORDBOX_DEVICE")}));
    EXPECT_EQ(QStringLiteral("/run/media/griz/GRIZ_T7"), before.dataKey);
    EXPECT_EQ(pDevice, find(&m_root, before));

    TreeItem* pCrate = m_root.appendChild(QStringLiteral("Tech"),
            QVariant(QVariantList{QStringLiteral("/d/_Serato_/Subcrates/Tech.crate"), true}));
    EXPECT_EQ(QStringLiteral("/d/_Serato_/Subcrates/Tech.crate"), dataKey(pCrate));
}

TEST_F(LibraryFavoritesTest, FindsADeepItemByData) {
    TreeItem* pDevice = m_root.appendChild(QStringLiteral("GRIZ_T7"),
            QVariant(QList<QString>{QStringLiteral("/m/GRIZ_T7"), QStringLiteral("x")}));
    TreeItem* pFolder = pDevice->appendChild(QStringLiteral("Sets"),
            QVariant(QList<QString>{QStringLiteral("/m/GRIZ_T7/Sets"), QStringLiteral("x")}));
    TreeItem* pPlaylist = pFolder->appendChild(QStringLiteral("Warmup"),
            QVariant(QList<QString>{
                    QStringLiteral("/m/GRIZ_T7/Sets/Warmup"), QStringLiteral("x")}));
    const LibraryFavorite favorite = favoriteFor(QStringLiteral("rekordbox"), pPlaylist);
    EXPECT_EQ((QStringList{QStringLiteral("GRIZ_T7"),
                      QStringLiteral("Sets"),
                      QStringLiteral("Warmup")}),
            favorite.labelPath);
    EXPECT_EQ(pPlaylist, find(&m_root, favorite));
}

TEST_F(LibraryFavoritesTest, SurvivesALabelChangeWhenTheDataIsUnique) {
    // A crate's label carries its track count, which changes.
    TreeItem* pCrate = m_root.appendChild(QStringLiteral("House (12)"), 42);
    const LibraryFavorite favorite = favoriteFor(QStringLiteral("crates"), pCrate);
    pCrate->setLabel(QStringLiteral("House (13)"));
    EXPECT_EQ(pCrate, find(&m_root, favorite));
}

TEST_F(LibraryFavoritesTest, SharedDataIsToldApartByLabel) {
    // History files its playlists under year folders that all carry the same
    // placeholder id.
    TreeItem* p2025 = m_root.appendChild(QStringLiteral("2025"), -2);
    TreeItem* p2026 = m_root.appendChild(QStringLiteral("2026"), -2);
    EXPECT_EQ(p2026, find(&m_root, favoriteFor(QStringLiteral("history"), p2026)));
    EXPECT_EQ(p2025, find(&m_root, favoriteFor(QStringLiteral("history"), p2025)));
}

TEST_F(LibraryFavoritesTest, ItemsWithoutDataAreFoundByLabelPath) {
    // The folder levels of nested Serato crates have no data of their own.
    TreeItem* pDb = m_root.appendChild(QStringLiteral("GRIZ_T7"),
            QVariant(QVariantList{QStringLiteral("/m/GRIZ_T7/_Serato_/database V2"), true}));
    TreeItem* pFolderA = pDb->appendChild(QStringLiteral("Genres"));
    TreeItem* pFolderB = pDb->appendChild(QStringLiteral("Gigs"));
    const LibraryFavorite favorite = favoriteFor(QStringLiteral("serato"), pFolderB);
    EXPECT_TRUE(favorite.dataKey.isEmpty());
    EXPECT_FALSE(favorite.isFeatureRoot());
    EXPECT_EQ(pFolderB, find(&m_root, favorite));
    EXPECT_NE(pFolderA, find(&m_root, favorite));
}

TEST_F(LibraryFavoritesTest, AGoneItemIsNotFound) {
    TreeItem* pCrate = m_root.appendChild(QStringLiteral("House"), 42);
    const LibraryFavorite favorite = favoriteFor(QStringLiteral("crates"), pCrate);
    m_root.removeChildren(0, 1);
    EXPECT_EQ(nullptr, find(&m_root, favorite));
}

TEST_F(LibraryFavoritesTest, AFeatureRootFindsTheRoot) {
    const LibraryFavorite favorite = favoriteFor(QStringLiteral("autodj"), nullptr);
    EXPECT_TRUE(favorite.isValid());
    EXPECT_TRUE(favorite.isFeatureRoot());
    EXPECT_EQ(&m_root, find(&m_root, favorite));
    EXPECT_TRUE(favoriteFor(QStringLiteral("autodj"), &m_root).isFeatureRoot());
}

TEST_F(LibraryFavoritesTest, RoundTripsThroughText) {
    LibraryFavorite favorite;
    favorite.feature = QStringLiteral("rekordbox");
    favorite.dataKey = QStringLiteral("/m/GRIZ T7/Sets\\Warm \"up\"");
    favorite.labelPath = QStringList{QStringLiteral("GRIZ T7"), QStringLiteral("Warm \"up\"")};
    const LibraryFavorite back = LibraryFavorite::fromString(favorite.toString());
    EXPECT_EQ(favorite.feature, back.feature);
    EXPECT_EQ(favorite.dataKey, back.dataKey);
    EXPECT_EQ(favorite.labelPath, back.labelPath);

    EXPECT_FALSE(LibraryFavorite::fromString(QString()).isValid());
    EXPECT_FALSE(LibraryFavorite::fromString(QStringLiteral("not json")).isValid());
}

} // namespace
