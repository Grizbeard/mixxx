#pragma once

#include "library/trackset/crate/crateid.h"
#include "util/db/dbnamedentity.h"

class Crate : public DbNamedEntity<CrateId> {
  public:
    explicit Crate(CrateId id = CrateId())
            : DbNamedEntity(id),
              m_locked(false),
              m_autoDjSource(false) {
    }
    ~Crate() override = default;

    bool isLocked() const {
        return m_locked;
    }
    void setLocked(bool locked = true) {
        m_locked = locked;
    }

    bool isAutoDjSource() const {
        return m_autoDjSource;
    }
    void setAutoDjSource(bool autoDjSource = true) {
        m_autoDjSource = autoDjSource;
    }

    /// Id of the crate this crate is nested inside. An invalid id means the
    /// crate sits at the top level of the crate tree.
    CrateId getParentId() const {
        return m_parentId;
    }
    void setParentId(CrateId parentId = CrateId()) {
        m_parentId = parentId;
    }
    bool hasParent() const {
        return m_parentId.isValid();
    }

  private:
    bool m_locked;
    bool m_autoDjSource;
    CrateId m_parentId;
};
