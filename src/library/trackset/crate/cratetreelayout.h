#pragma once

#include <QHash>
#include <QList>

#include "library/trackset/crate/crateid.h"
#include "library/trackset/crate/cratesummary.h"

/// The shape of the crate tree, derived from a flat list of crate summaries.
///
/// This is kept separate from building the sidebar's TreeItem nodes so that
/// the grouping and reachability rules can be tested without a widget.
class CrateTreeLayout {
  public:
    /// Group the summaries by the crate they are nested inside and work out
    /// which of them the tree can actually reach.
    static CrateTreeLayout fromCrateSummaries(
            const QList<CrateSummary>& crateSummaries);

    /// The crates nested directly inside parentId, or the top-level crates
    /// for an invalid parentId. The order of the summaries passed in is
    /// preserved, so an ordered query stays ordered.
    QList<CrateSummary> childrenOf(CrateId parentId) const {
        return m_childrenByParentId.value(parentId);
    }

    /// The crates that no descent from the top level reaches, in the order
    /// they were passed in. Only a parent cycle can put a crate here, and it
    /// has to be displayed somewhere regardless or it would look to the user
    /// as though the crate had been lost.
    const QList<CrateSummary>& unreachableCrates() const {
        return m_unreachableCrates;
    }

  private:
    QHash<CrateId, QList<CrateSummary>> m_childrenByParentId;
    QList<CrateSummary> m_unreachableCrates;
};
