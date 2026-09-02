#include "library/trackset/crate/cratestorage.h"

#include <algorithm>

#include "library/trackset/crate/crate.h"
#include "test/librarytest.h"
#include "util/db/fwdsqlquery.h"

class CrateStorageTest : public LibraryTest {
  protected:
    CrateStorageTest() {
        m_crateStorage.connectDatabase(dbConnection());
    }

    CrateId createCrate(const QString& name, CrateId parentId = CrateId()) {
        Crate crate;
        crate.setName(name);
        crate.setParentId(parentId);
        CrateId crateId;
        EXPECT_TRUE(m_crateStorage.onInsertingCrate(crate, &crateId));
        EXPECT_TRUE(crateId.isValid());
        return crateId;
    }

    CrateId parentIdOf(CrateId crateId) {
        Crate crate;
        EXPECT_TRUE(m_crateStorage.readCrateById(crateId, &crate));
        return crate.getParentId();
    }

    QList<CrateId> childIdsOf(CrateId parentId) {
        QList<CrateId> childIds;
        CrateSelectResult children(m_crateStorage.selectChildCrates(parentId));
        Crate child;
        while (children.populateNext(&child)) {
            childIds.append(child.getId());
        }
        return childIds;
    }

    CrateStorage m_crateStorage;
};

TEST_F(CrateStorageTest, persistentLifecycle) {
    constexpr uint kNumCrates = 10;

    // Insert some crates
    for (auto i = kNumCrates; i > 0; --i) {
        Crate crate;
        crate.setName(QString("Crate %1").arg(i));
        ASSERT_TRUE(m_crateStorage.onInsertingCrate(crate));
    }
    EXPECT_EQ(kNumCrates, m_crateStorage.countCrates());

    // Identify one of the "middle" crates by name
    const auto kCrateIndex = (kNumCrates + 1) / 2;
    const auto kCrateName = QString("Crate %1").arg(kCrateIndex);
    CrateId crateId;

    // Find this crate by name
    {
        Crate crate;
        ASSERT_TRUE(m_crateStorage.readCrateByName(kCrateName, &crate));
        EXPECT_EQ(kCrateName, crate.getName());
        crateId = crate.getId();
        ASSERT_TRUE(crateId.isValid());
    }

    // Find this crate crate again, but now by id
    Crate crate;
    {
        ASSERT_TRUE(m_crateStorage.readCrateById(crateId, &crate));
        EXPECT_EQ(crateId, crate.getId());
        EXPECT_EQ(kCrateName, crate.getName());
    }

    // Update the crate's name
    const auto kNewCrateName = QString("New%1").arg(kCrateName);
    crate.setName(kNewCrateName);
    ASSERT_TRUE(m_crateStorage.onUpdatingCrate(crate));
    // Reading the crate by its old name should fail
    EXPECT_FALSE(m_crateStorage.readCrateByName(kCrateName));
    // Reading by id should reflect the updated name
    {
        Crate updatedCrate;
        EXPECT_TRUE(m_crateStorage.readCrateById(crateId, &updatedCrate));
        EXPECT_EQ(kNewCrateName, updatedCrate.getName());
    }

    // Finally delete this crate
    ASSERT_TRUE(m_crateStorage.onDeletingCrate(crateId));
    EXPECT_FALSE(m_crateStorage.readCrateById(crateId));
    EXPECT_FALSE(m_crateStorage.readCrateByName(kCrateName));
    EXPECT_FALSE(m_crateStorage.readCrateByName(kNewCrateName));
    EXPECT_EQ(kNumCrates - 1, m_crateStorage.countCrates());
}

TEST_F(CrateStorageTest, newCrateIsAtTopLevel) {
    const CrateId crateId = createCrate("Top");

    EXPECT_FALSE(parentIdOf(crateId).isValid());
    EXPECT_EQ(QList<CrateId>{crateId}, childIdsOf(CrateId()));
    EXPECT_FALSE(m_crateStorage.hasChildCrates(crateId));
}

TEST_F(CrateStorageTest, insertNestedCrate) {
    const CrateId parentId = createCrate("Parent");
    const CrateId childId = createCrate("Child", parentId);

    EXPECT_EQ(parentId, parentIdOf(childId));
    EXPECT_TRUE(m_crateStorage.hasChildCrates(parentId));
    EXPECT_EQ(QList<CrateId>{childId}, childIdsOf(parentId));
    // A nested crate must not also show up at the top level.
    EXPECT_EQ(QList<CrateId>{parentId}, childIdsOf(CrateId()));
}

TEST_F(CrateStorageTest, insertIntoNonExistentParentFails) {
    const CrateId parentId = createCrate("Parent");
    ASSERT_TRUE(m_crateStorage.onDeletingCrate(parentId));

    Crate orphan;
    orphan.setName("Orphan");
    orphan.setParentId(parentId);
    EXPECT_FALSE(m_crateStorage.onInsertingCrate(orphan));
    EXPECT_FALSE(m_crateStorage.readCrateByName("Orphan"));
}

TEST_F(CrateStorageTest, childCratesAreOrderedByName) {
    const CrateId parentId = createCrate("Parent");
    const CrateId beta = createCrate("beta", parentId);
    const CrateId alpha = createCrate("Alpha", parentId);
    const CrateId gamma = createCrate("Gamma", parentId);

    // selectChildCrates() collates case-insensitively, like selectCrates().
    EXPECT_EQ(QList<CrateId>({alpha, beta, gamma}), childIdsOf(parentId));
}

TEST_F(CrateStorageTest, moveCrateBetweenParents) {
    const CrateId firstParentId = createCrate("First");
    const CrateId secondParentId = createCrate("Second");
    const CrateId childId = createCrate("Child", firstParentId);

    ASSERT_TRUE(m_crateStorage.onMovingCrate(childId, secondParentId));
    EXPECT_EQ(secondParentId, parentIdOf(childId));
    EXPECT_TRUE(childIdsOf(firstParentId).isEmpty());
    EXPECT_EQ(QList<CrateId>{childId}, childIdsOf(secondParentId));

    // Moving back to the top level clears the parent.
    ASSERT_TRUE(m_crateStorage.onMovingCrate(childId, CrateId()));
    EXPECT_FALSE(parentIdOf(childId).isValid());
    EXPECT_TRUE(childIdsOf(secondParentId).isEmpty());
}

TEST_F(CrateStorageTest, moveCrateRejectsCycles) {
    const CrateId grandParentId = createCrate("GrandParent");
    const CrateId parentId = createCrate("Parent", grandParentId);
    const CrateId childId = createCrate("Child", parentId);

    // A crate cannot be nested inside itself...
    EXPECT_FALSE(m_crateStorage.onMovingCrate(parentId, parentId));
    // ...nor inside one of its own descendants, at any depth.
    EXPECT_FALSE(m_crateStorage.onMovingCrate(parentId, childId));
    EXPECT_FALSE(m_crateStorage.onMovingCrate(grandParentId, childId));

    // The rejected moves must leave the tree untouched.
    EXPECT_EQ(grandParentId, parentIdOf(parentId));
    EXPECT_EQ(parentId, parentIdOf(childId));
}

TEST_F(CrateStorageTest, collectDescendantCrateIds) {
    const CrateId rootId = createCrate("Root");
    const CrateId branchId = createCrate("Branch", rootId);
    const CrateId leafId = createCrate("Leaf", branchId);
    const CrateId siblingId = createCrate("Sibling", rootId);
    const CrateId unrelatedId = createCrate("Unrelated");

    QList<CrateId> descendants = m_crateStorage.collectDescendantCrateIds(rootId);
    std::sort(descendants.begin(), descendants.end());
    QList<CrateId> expected({branchId, leafId, siblingId});
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(expected, descendants);

    EXPECT_EQ(QList<CrateId>{leafId},
            m_crateStorage.collectDescendantCrateIds(branchId));
    EXPECT_TRUE(m_crateStorage.collectDescendantCrateIds(leafId).isEmpty());
    EXPECT_TRUE(m_crateStorage.collectDescendantCrateIds(unrelatedId).isEmpty());
}

TEST_F(CrateStorageTest, isAncestorOf) {
    const CrateId rootId = createCrate("Root");
    const CrateId branchId = createCrate("Branch", rootId);
    const CrateId leafId = createCrate("Leaf", branchId);
    const CrateId unrelatedId = createCrate("Unrelated");

    EXPECT_TRUE(m_crateStorage.isAncestorOf(rootId, branchId));
    EXPECT_TRUE(m_crateStorage.isAncestorOf(rootId, leafId));
    EXPECT_TRUE(m_crateStorage.isAncestorOf(branchId, leafId));

    // Not reflexive, not symmetric, and unrelated subtrees stay unrelated.
    EXPECT_FALSE(m_crateStorage.isAncestorOf(rootId, rootId));
    EXPECT_FALSE(m_crateStorage.isAncestorOf(leafId, rootId));
    EXPECT_FALSE(m_crateStorage.isAncestorOf(rootId, unrelatedId));
    EXPECT_FALSE(m_crateStorage.isAncestorOf(CrateId(), rootId));
}

TEST_F(CrateStorageTest, deletingCrateLiftsChildrenToItsParent) {
    const CrateId grandParentId = createCrate("GrandParent");
    const CrateId parentId = createCrate("Parent", grandParentId);
    const CrateId childId = createCrate("Child", parentId);

    ASSERT_TRUE(m_crateStorage.onDeletingCrate(parentId));

    // The child survives and takes the deleted crate's place.
    EXPECT_TRUE(m_crateStorage.readCrateById(childId));
    EXPECT_EQ(grandParentId, parentIdOf(childId));
    EXPECT_EQ(QList<CrateId>{childId}, childIdsOf(grandParentId));
}

TEST_F(CrateStorageTest, deletingTopLevelCrateLiftsChildrenToTopLevel) {
    const CrateId parentId = createCrate("Parent");
    const CrateId childId = createCrate("Child", parentId);

    ASSERT_TRUE(m_crateStorage.onDeletingCrate(parentId));

    EXPECT_TRUE(m_crateStorage.readCrateById(childId));
    EXPECT_FALSE(parentIdOf(childId).isValid());
    EXPECT_EQ(QList<CrateId>{childId}, childIdsOf(CrateId()));
}

TEST_F(CrateStorageTest, renamingCrateKeepsItsParent) {
    const CrateId parentId = createCrate("Parent");
    const CrateId childId = createCrate("Child", parentId);

    Crate child;
    ASSERT_TRUE(m_crateStorage.readCrateById(childId, &child));
    child.setName("Renamed");
    ASSERT_TRUE(m_crateStorage.onUpdatingCrate(child));

    EXPECT_EQ(parentId, parentIdOf(childId));
}

TEST_F(CrateStorageTest, repairDatabaseDetachesMissingParent) {
    const CrateId parentId = createCrate("Parent");
    const CrateId childId = createCrate("Child", parentId);

    // Delete the parent row directly, bypassing the reparenting that
    // onDeletingCrate() performs, to simulate a damaged database.
    ASSERT_TRUE(FwdSqlQuery(dbConnection(),
            QStringLiteral("DELETE FROM crates WHERE id=%1").arg(parentId.toString()))
                    .execPrepared());
    ASSERT_EQ(parentId, parentIdOf(childId));

    m_crateStorage.repairDatabase(dbConnection());

    EXPECT_FALSE(parentIdOf(childId).isValid());
    EXPECT_EQ(QList<CrateId>{childId}, childIdsOf(CrateId()));
}

TEST_F(CrateStorageTest, repairDatabaseBreaksParentCycle) {
    const CrateId firstId = createCrate("First");
    const CrateId secondId = createCrate("Second", firstId);

    // Close the loop directly, since onMovingCrate() would refuse this.
    ASSERT_TRUE(FwdSqlQuery(dbConnection(),
            QStringLiteral("UPDATE crates SET parent_id=%1 WHERE id=%2")
                    .arg(secondId.toString(), firstId.toString()))
                    .execPrepared());
    ASSERT_TRUE(childIdsOf(CrateId()).isEmpty());

    m_crateStorage.repairDatabase(dbConnection());

    // Both crates are reachable again.
    EXPECT_EQ(2, childIdsOf(CrateId()).size());
}
