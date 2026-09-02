#include "library/trackset/crate/cratetreelayout.h"

#include <QSet>

CrateTreeLayout CrateTreeLayout::fromCrateSummaries(
        const QList<CrateSummary>& crateSummaries) {
    CrateTreeLayout layout;
    for (const CrateSummary& crateSummary : crateSummaries) {
        layout.m_childrenByParentId[crateSummary.getParentId()].append(crateSummary);
    }

    // Walk down from the top level to find every crate the tree can display.
    // Anything left over is in a parent cycle: it has a parent, and that
    // parent chain never arrives at the top level.
    QSet<CrateId> reachableCrateIds;
    QList<CrateId> pending({CrateId()});
    while (!pending.isEmpty()) {
        const CrateId parentId = pending.takeLast();
        const auto children = layout.m_childrenByParentId.value(parentId);
        for (const CrateSummary& child : children) {
            if (reachableCrateIds.contains(child.getId())) {
                // A crate is listed under exactly one parent, so reaching one
                // twice would mean the parent chain looped back into the part
                // of the tree already walked.
                continue;
            }
            reachableCrateIds.insert(child.getId());
            pending.append(child.getId());
        }
    }

    // Report the unreachable crates in the original order rather than the
    // order the walk happened to abandon them in.
    for (const CrateSummary& crateSummary : crateSummaries) {
        if (!reachableCrateIds.contains(crateSummary.getId())) {
            layout.m_unreachableCrates.append(crateSummary);
        }
    }
    return layout;
}
