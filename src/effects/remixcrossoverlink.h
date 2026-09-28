#pragma once

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QSet>

#include "effects/defs.h"

class EffectChain;
class EffectSlot;

/// Keeps the crossover knobs and slope switches of the Remix effects in one
/// effect chain in step. Remix Low's and Remix Mid's low crossover move
/// together, and so do Remix Mid's and Remix High's high crossover, so
/// neighbouring bands meet at the same frequency without a gap or an overlap
/// between them. Flipping one effect's Slope flips them all.
///
/// Parameters are matched by id, not by position, because the user can hide
/// and reorder an effect's knobs and buttons. A Remix effect loaded next to
/// others takes over their settings; when a whole chain loads at once, the
/// first slot's values win.
class RemixCrossoverLink : public QObject {
    Q_OBJECT
  public:
    explicit RemixCrossoverLink(EffectChain* pChain);
    ~RemixCrossoverLink() override;

  private slots:
    void slotAdoptNewlyLoaded();

  private:
    void effectChanged(EffectSlot* pSlot);
    void rebind();
    void propagate(EffectSlot* pSource, const QString& parameterId, double value);
    void setLinkedValue(EffectSlot* pTarget, const QString& parameterId, double value);

    EffectChain* m_pChain;
    QList<QMetaObject::Connection> m_parameterConnections;
    QSet<EffectSlot*> m_newlyLoaded;
    bool m_adoptPending;
    bool m_propagating;
};
