#pragma once

#include <QString>
#include <atomic>
#include <memory>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "control/controlpushbutton.h"

/// Helpers to declare the spatial controls compactly.
namespace mixxx::spatial {

inline std::unique_ptr<ControlPotmeter> makePot(const QString& group,
        const QString& key,
        double min,
        double max,
        double defaultValue,
        bool persist = false,
        bool allowOutOfBounds = false) {
    auto pControl = std::make_unique<ControlPotmeter>(ConfigKey(group, key),
            min,
            max,
            allowOutOfBounds,
            true,
            false,
            persist,
            defaultValue);
    // ControlPotmeter makes the middle of the range its default (and, when
    // not persisted, its value); use ours.
    pControl->setDefaultValue(defaultValue);
    if (!persist) {
        pControl->set(defaultValue);
    }
    return pControl;
}

/// A toggle (or, with `states` > 2, a selector that cycles on press).
inline std::unique_ptr<ControlPushButton> makeToggle(const QString& group,
        const QString& key,
        double defaultValue = 0,
        bool persist = false,
        int states = 2) {
    auto pControl = std::make_unique<ControlPushButton>(
            ConfigKey(group, key), persist, defaultValue);
    pControl->setButtonMode(mixxx::control::ButtonMode::Toggle);
    if (states > 2) {
        pControl->setStates(states);
    }
    return pControl;
}

/// A read-only value the engine publishes (levels, positions).
inline std::unique_ptr<ControlObject> makeReadOnly(
        const QString& group, const QString& key, double defaultValue = 0) {
    auto pControl = std::make_unique<ControlObject>(
            ConfigKey(group, key), true, false, false, defaultValue);
    pControl->setReadOnly();
    return pControl;
}

/// A momentary button whose presses the engine picks up once each.
class TriggerButton {
  public:
    TriggerButton(const QString& group, const QString& key)
            : m_pButton(std::make_unique<ControlPushButton>(ConfigKey(group, key))) {
        QObject::connect(m_pButton.get(), &ControlObject::valueChanged, [this](double value) {
            if (value > 0) {
                m_pending.store(true);
            }
        });
    }
    /// True once per press.
    bool take() {
        return m_pending.exchange(false);
    }

  private:
    std::unique_ptr<ControlPushButton> m_pButton;
    std::atomic<bool> m_pending{false};
};

inline int toIndex(double value, int min, int max) {
    const int i = static_cast<int>(value + 0.5);
    return i < min ? min : (i > max ? max : i);
}

} // namespace mixxx::spatial
