#include "effects/remixcrossoverlink.h"

#include <QStringList>
#include <utility>

#include "control/controlproxy.h"
#include "effects/backends/builtin/remixeffect.h"
#include "effects/effectbuttonparameterslot.h"
#include "effects/effectchain.h"
#include "effects/effectknobparameterslot.h"
#include "effects/effectparameter.h"
#include "effects/effectparameterslotbase.h"
#include "effects/effectslot.h"
#include "moc_remixcrossoverlink.cpp"

namespace {

const QStringList kLinkedParameterIds = {
        remix::kLowCrossoverId,
        remix::kHighCrossoverId,
        remix::kSlopeId,
};

const EffectParameterType kLinkedParameterTypes[] = {
        EffectParameterType::Knob,
        EffectParameterType::Button,
};

/// Where a parameter is shown: its type and the slot of that type it is
/// loaded in, or -1 if the user has hidden it.
struct ParameterPlace {
    EffectParameterType type;
    int slot;
};

bool holdsRemixEffect(const EffectSlot* pSlot) {
    return pSlot->isLoaded() && remix::isRemixEffect(pSlot->id());
}

EffectParameterPointer findParameter(
        const EffectSlot* pSlot, const QString& parameterId, ParameterPlace* pPlace) {
    for (EffectParameterType type : kLinkedParameterTypes) {
        const auto loaded = pSlot->getLoadedParameters().value(type);
        for (int i = 0; i < loaded.size(); ++i) {
            if (loaded[i]->manifest()->id() == parameterId) {
                *pPlace = ParameterPlace{type, i};
                return loaded[i];
            }
        }
        const auto hidden = pSlot->getHiddenParameters().value(type);
        for (const auto& pParameter : hidden) {
            if (pParameter->manifest()->id() == parameterId) {
                *pPlace = ParameterPlace{type, -1};
                return pParameter;
            }
        }
    }
    return EffectParameterPointer();
}

QString controlItem(const ParameterPlace& place) {
    if (place.type == EffectParameterType::Button) {
        return EffectButtonParameterSlot::formatItemPrefix(place.slot);
    }
    return EffectKnobParameterSlot::formatItemPrefix(place.slot);
}

} // anonymous namespace

RemixCrossoverLink::RemixCrossoverLink(EffectChain* pChain)
        : m_pChain(pChain),
          m_adoptPending(false),
          m_propagating(false) {
    for (const auto& pSlot : m_pChain->getEffectSlots()) {
        EffectSlot* pEffectSlot = pSlot.data();
        connect(pEffectSlot,
                &EffectSlot::effectChanged,
                this,
                [this, pEffectSlot]() {
                    effectChanged(pEffectSlot);
                });
        connect(pEffectSlot,
                &EffectSlot::parametersChanged,
                this,
                &RemixCrossoverLink::rebind);
    }
    rebind();
}

RemixCrossoverLink::~RemixCrossoverLink() = default;

void RemixCrossoverLink::effectChanged(EffectSlot* pSlot) {
    if (holdsRemixEffect(pSlot)) {
        m_newlyLoaded.insert(pSlot);
        // A chain preset loads its effects one after another. Wait until it
        // is done so the new effects agree with each other, not with the
        // ones they are replacing.
        if (!m_adoptPending) {
            m_adoptPending = true;
            QMetaObject::invokeMethod(this,
                    &RemixCrossoverLink::slotAdoptNewlyLoaded,
                    Qt::QueuedConnection);
        }
    }
    rebind();
}

void RemixCrossoverLink::slotAdoptNewlyLoaded() {
    m_adoptPending = false;
    const QSet<EffectSlot*> newlyLoaded = std::exchange(m_newlyLoaded, {});

    for (const QString& parameterId : kLinkedParameterIds) {
        // Prefer an effect that was already loaded; among new ones, the first.
        EffectSlot* pReference = nullptr;
        EffectParameterPointer pReferenceParameter;
        for (const auto& pSlot : m_pChain->getEffectSlots()) {
            if (!holdsRemixEffect(pSlot.data())) {
                continue;
            }
            ParameterPlace place;
            EffectParameterPointer pParameter =
                    findParameter(pSlot.data(), parameterId, &place);
            if (!pParameter) {
                continue;
            }
            const bool isNew = newlyLoaded.contains(pSlot.data());
            if (!pReference || (!isNew && newlyLoaded.contains(pReference))) {
                pReference = pSlot.data();
                pReferenceParameter = pParameter;
            }
        }
        if (!pReference) {
            continue;
        }
        const double value = pReferenceParameter->getValue();
        m_propagating = true;
        for (const auto& pSlot : m_pChain->getEffectSlots()) {
            if (pSlot.data() != pReference && holdsRemixEffect(pSlot.data())) {
                setLinkedValue(pSlot.data(), parameterId, value);
            }
        }
        m_propagating = false;
    }
}

void RemixCrossoverLink::rebind() {
    for (const auto& connection : std::as_const(m_parameterConnections)) {
        disconnect(connection);
    }
    m_parameterConnections.clear();

    for (const auto& pSlot : m_pChain->getEffectSlots()) {
        EffectSlot* pEffectSlot = pSlot.data();
        if (!holdsRemixEffect(pEffectSlot)) {
            continue;
        }
        for (EffectParameterType type : kLinkedParameterTypes) {
            const auto loaded = pEffectSlot->getLoadedParameters().value(type);
            for (int i = 0; i < loaded.size(); ++i) {
                const QString parameterId = loaded[i]->manifest()->id();
                if (!kLinkedParameterIds.contains(parameterId)) {
                    continue;
                }
                EffectParameterSlotBasePointer pParameterSlot =
                        pEffectSlot->getEffectParameterSlot(type, i);
                if (!pParameterSlot) {
                    continue;
                }
                m_parameterConnections.append(connect(pParameterSlot.data(),
                        &EffectParameterSlotBase::valueChanged,
                        this,
                        [this, pEffectSlot, parameterId](double value) {
                            propagate(pEffectSlot, parameterId, value);
                        }));
            }
        }
    }
}

void RemixCrossoverLink::propagate(
        EffectSlot* pSource, const QString& parameterId, double value) {
    // Setting a partner's knob changes that knob too, which would come back here.
    if (m_propagating) {
        return;
    }
    m_propagating = true;
    for (const auto& pSlot : m_pChain->getEffectSlots()) {
        if (pSlot.data() != pSource && holdsRemixEffect(pSlot.data())) {
            setLinkedValue(pSlot.data(), parameterId, value);
        }
    }
    m_propagating = false;
}

void RemixCrossoverLink::setLinkedValue(
        EffectSlot* pTarget, const QString& parameterId, double value) {
    ParameterPlace place;
    EffectParameterPointer pParameter = findParameter(pTarget, parameterId, &place);
    if (!pParameter || pParameter->getValue() == value) {
        return;
    }
    if (place.slot >= 0) {
        // Go through the parameter's control, as a skin or a controller would,
        // so the knob or button shows the new value and the engine gets it.
        ControlProxy control(pTarget->getGroup(), controlItem(place));
        control.set(value);
    } else {
        pParameter->setValue(value);
    }
}
