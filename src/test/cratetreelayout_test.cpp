#include "library/trackset/crate/cratetreelayout.h"

#include <gtest/gtest.h>

#include <QStringList>
#include <string>

namespace {

// Braces throughout: CrateSummary crateSummary(CrateId(QVariant(id))) is
// parsed as a function declaration, not a variable.
CrateSummary makeCrate(int id, const QString& name, int parentId = -1) {
    CrateSummary crateSummary{CrateId{QVariant{id}}};
    crateSummary.setName(name);
    if (parentId >= 0) {
        crateSummary.setParentId(CrateId{QVariant{parentId}});
    }
    return crateSummary;
}

// Joined into one string so that a failure prints the crate names instead of
// gtest's byte-by-byte dump of a QStringList.
std::string namesOf(const QList<CrateSummary>& crateSummaries) {
    QStringList names;
    for (const CrateSummary& crateSummary : crateSummaries) {
        names.append(crateSummary.getName());
    }
    return names.join(QStringLiteral(" | ")).toStdString();
}

CrateId crateId(int id) {
    return CrateId{QVariant{id}};
}

} // namespace

TEST(CrateTreeLayoutTest, emptyInputHasNothingAnywhere) {
    const auto layout = CrateTreeLayout::fromCrateSummaries({});

    EXPECT_EQ("", namesOf(layout.childrenOf(CrateId())));
    EXPECT_EQ("", namesOf(layout.unreachableCrates()));
}

TEST(CrateTreeLayoutTest, cratesWithoutParentsAreAllAtTheTopLevel) {
    const auto layout = CrateTreeLayout::fromCrateSummaries({
            makeCrate(1, "Alpha"),
            makeCrate(2, "Beta"),
    });

    EXPECT_EQ("Alpha | Beta", namesOf(layout.childrenOf(CrateId())));
    EXPECT_EQ("", namesOf(layout.unreachableCrates()));
}

TEST(CrateTreeLayoutTest, groupsCratesUnderTheirParent) {
    const auto layout = CrateTreeLayout::fromCrateSummaries({
            makeCrate(1, "House"),
            makeCrate(2, "Rooftop", 1),
            makeCrate(3, "UKG", 1),
            makeCrate(4, "Open Format"),
    });

    EXPECT_EQ("House | Open Format", namesOf(layout.childrenOf(CrateId())));
    EXPECT_EQ("Rooftop | UKG", namesOf(layout.childrenOf(crateId(1))));
    EXPECT_EQ("", namesOf(layout.childrenOf(crateId(4))));
    EXPECT_EQ("", namesOf(layout.unreachableCrates()));
}

TEST(CrateTreeLayoutTest, preservesTheOrderOfTheSummariesItWasGiven) {
    // The query orders crates by name, and each group has to inherit that
    // rather than whatever order a hash happens to produce.
    const auto layout = CrateTreeLayout::fromCrateSummaries({
            makeCrate(1, "Archive"),
            makeCrate(2, "Blade Rave", 1),
            makeCrate(3, "Funky Collection", 1),
            makeCrate(4, "stutter house", 1),
            makeCrate(5, "XMAS 2023", 1),
    });

    EXPECT_EQ("Blade Rave | Funky Collection | stutter house | XMAS 2023",
            namesOf(layout.childrenOf(crateId(1))));
}

TEST(CrateTreeLayoutTest, handlesNestingSeveralLevelsDeep) {
    const auto layout = CrateTreeLayout::fromCrateSummaries({
            makeCrate(1, "Grizbeard"),
            makeCrate(2, "Archive", 1),
            makeCrate(3, "Funky Collection", 2),
            makeCrate(4, "Funky Disco", 3),
    });

    EXPECT_EQ("Grizbeard", namesOf(layout.childrenOf(CrateId())));
    EXPECT_EQ("Archive", namesOf(layout.childrenOf(crateId(1))));
    EXPECT_EQ("Funky Collection", namesOf(layout.childrenOf(crateId(2))));
    EXPECT_EQ("Funky Disco", namesOf(layout.childrenOf(crateId(3))));
    EXPECT_EQ("", namesOf(layout.unreachableCrates()));
}

TEST(CrateTreeLayoutTest, reportsCratesWhoseParentIsMissing) {
    // Nothing links crate 9 to the top level, so the tree cannot show the
    // orphan in its nominal place and has to surface it rather than drop it.
    const auto layout = CrateTreeLayout::fromCrateSummaries({
            makeCrate(1, "Present"),
            makeCrate(2, "Orphan", 9),
    });

    EXPECT_EQ("Present", namesOf(layout.childrenOf(CrateId())));
    EXPECT_EQ("Orphan", namesOf(layout.unreachableCrates()));
}

TEST(CrateTreeLayoutTest, reportsCratesCaughtInAParentCycle) {
    // 2 -> 3 -> 2 never arrives at the top level.
    const auto layout = CrateTreeLayout::fromCrateSummaries({
            makeCrate(1, "Reachable"),
            makeCrate(2, "Looped", 3),
            makeCrate(3, "Also Looped", 2),
    });

    EXPECT_EQ("Reachable", namesOf(layout.childrenOf(CrateId())));
    EXPECT_EQ("Looped | Also Looped", namesOf(layout.unreachableCrates()));
}

TEST(CrateTreeLayoutTest, reportsACrateThatIsItsOwnParent) {
    const auto layout = CrateTreeLayout::fromCrateSummaries({
            makeCrate(1, "Fine"),
            makeCrate(2, "Self Parented", 2),
    });

    EXPECT_EQ("Fine", namesOf(layout.childrenOf(CrateId())));
    EXPECT_EQ("Self Parented", namesOf(layout.unreachableCrates()));
}

TEST(CrateTreeLayoutTest, keepsASubtreeHangingOffACycleOutOfTheReachableTree) {
    // Crate 4 has a perfectly ordinary parent, but that parent sits inside a
    // cycle, so crate 4 is unreachable too and must not be lost.
    const auto layout = CrateTreeLayout::fromCrateSummaries({
            makeCrate(1, "Top"),
            makeCrate(2, "Looped", 3),
            makeCrate(3, "Also Looped", 2),
            makeCrate(4, "Below The Cycle", 3),
    });

    EXPECT_EQ("Top", namesOf(layout.childrenOf(CrateId())));
    EXPECT_EQ("Looped | Also Looped | Below The Cycle",
            namesOf(layout.unreachableCrates()));
    // The grouping is still intact, so once the cycle members are surfaced
    // the tree can render crate 4 underneath its real parent.
    EXPECT_EQ("Looped | Below The Cycle", namesOf(layout.childrenOf(crateId(3))));
}
