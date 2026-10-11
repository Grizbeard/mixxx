#include "spatial/spatialpresets.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QScrollArea>
#include <QScroller>
#include <cmath>
#include <initializer_list>

#include "control/controlobject.h"
#include "engine/spatial/spatialfxmeta.h"
#include "moc_spatialpresets.cpp"
#include "spatial/spatialpanels.h"
#include "spatial/spatialstyle.h"

using namespace spatialstyle;

namespace {

// Dark dialogs, so saving a preset doesn't flash a light window over the
// LateNight-style mixer.
const QString kDialogStyle = QStringLiteral(
        "QWidget { background: #1e1e20; color: #c2b3a5; font-size: 16px; }"
        "QLineEdit { background: #0f0f10; border: 1px solid #333; padding: 8px; }"
        "QPushButton { background: #0f0f10; border: 1px solid #2c2c2e; padding: 10px 22px;"
        " min-width: 90px; }"
        "QPushButton:pressed { background: #242426; }");

using Values = std::initializer_list<std::pair<const char*, double>>;

SpatialPreset make(const QString& name, const QMap<QString, double>& defaults, Values values) {
    SpatialPreset preset{name, defaults, true};
    for (const auto& [key, value] : values) {
        preset.values.insert(QString::fromLatin1(key), value);
    }
    return preset;
}

QMap<QString, double> mapOf(Values values) {
    QMap<QString, double> map;
    for (const auto& [key, value] : values) {
        map.insert(QString::fromLatin1(key), value);
    }
    return map;
}

const QMap<QString, double>& autopanDefaults() {
    static const auto defaults = mapOf({{"shape", 0},
            {"wave", 2},
            {"sync", 1},
            {"beats", 1},
            {"triplet", 0},
            {"seconds", 1},
            {"direction", 0},
            {"size", 1}});
    return defaults;
}

const QMap<QString, double>& delayDefaults() {
    static const auto defaults = mapOf({{"level", 0.8},
            {"sync", 1},
            {"beats", 0.75},
            {"triplet", 0},
            {"time_ms", 375},
            {"feedback", 0.55},
            {"tone", 0.6},
            {"lowcut", 150},
            {"shape", 0},
            {"direction", 0},
            {"size", 1},
            {"poly", 0},
            {"poly_beats", 1},
            {"poly_triplet", 0},
            {"gate", 0},
            {"gate_beats", 0.25},
            {"gate_duty", 0.5},
            {"gate_smooth", 0.25},
            {"to_reverb", 0.3}});
    return defaults;
}

const QMap<QString, double>& reverbDefaults() {
    static const auto defaults = mapOf({{"level", 0.6},
            {"decay", 9},
            {"size", 0.8},
            {"predelay_ms", 60},
            {"damping", 0.45},
            {"lowcut", 250},
            {"modulation", 0.6},
            {"diffusion", 0.75},
            {"shimmer", 0},
            {"duck", 0},
            {"direct", 1}});
    return defaults;
}

/// A preset key: "key" belongs to `group`; "[Group],key" names its own.
ConfigKey resolve(const QString& group, const QString& key) {
    if (key.startsWith(QLatin1Char('['))) {
        const int end = key.indexOf(QStringLiteral("],"));
        if (end > 0) {
            return ConfigKey(key.left(end + 1), key.mid(end + 2));
        }
    }
    return ConfigKey(group, key);
}

bool sameValue(double a, double b) {
    return std::abs(a - b) <= 1e-6 * std::max(1.0, std::abs(b));
}

} // namespace

namespace spatialpresets {

QStringList autopanKeys() {
    return autopanDefaults().keys();
}

QStringList fxKeys() {
    QStringList keys;
    for (const QString& key : delayDefaults().keys()) {
        keys.append(QStringLiteral("[SpatialDelay],") + key);
    }
    keys.append(QStringLiteral("[SpatialDelay],enabled"));
    for (const QString& key : reverbDefaults().keys()) {
        keys.append(QStringLiteral("[SpatialReverb],") + key);
    }
    keys.append(QStringLiteral("[SpatialReverb],enabled"));
    return keys;
}

QList<SpatialPreset> autopanFactory() {
    const auto& d = autopanDefaults();
    return {
            make(QStringLiteral("SLOW ORBIT"), d, {{"shape", 0}, {"wave", 2}, {"beats", 2}}),
            make(QStringLiteral("CORNER SNAP"), d, {{"shape", 3}, {"wave", 0}, {"beats", 1}}),
            make(QStringLiteral("L-R SWEEP"), d, {{"shape", 1}, {"wave", 1}, {"beats", 1}}),
            make(QStringLiteral("SAW ORBIT"), d, {{"shape", 0}, {"wave", 3}, {"beats", 1}}),
            make(QStringLiteral("STAR GLIDE"), d, {{"shape", 4}, {"wave", 2}, {"beats", 1}}),
            make(QStringLiteral("RANDOM HITS"),
                    d,
                    {{"shape", 6}, {"wave", 0}, {"beats", 0.5}}),
    };
}

QList<SpatialPreset> fxFactory() {
    // Delay and reverb settings by name, each over its defaults.
    const auto delay = [](Values values) {
        QMap<QString, double> map = delayDefaults();
        for (const auto& [key, value] : values) {
            map.insert(QString::fromLatin1(key), value);
        }
        return map;
    };
    const auto reverb = [](Values values) {
        QMap<QString, double> map = reverbDefaults();
        for (const auto& [key, value] : values) {
            map.insert(QString::fromLatin1(key), value);
        }
        return map;
    };
    const auto dottedPing = delay({{"beats", 0.75}, {"shape", 1}, {"feedback", 0.5}});
    const auto quarterOrbit = delay({{"beats", 1}, {"shape", 0}, {"feedback", 0.45}});
    const auto tripletCorners = delay({{"beats", 0.5}, {"triplet", 1}, {"shape", 3}});
    const auto polyStars =
            delay({{"beats", 0.75}, {"shape", 4}, {"poly", 1}, {"poly_beats", 1}});
    const auto tranceGate = delay({{"beats", 0.5},
            {"feedback", 0.7},
            {"shape", 3},
            {"gate", 1},
            {"gate_beats", 1.0 / 16},
            {"gate_duty", 0.5}});
    const auto washThrow = delay({{"beats", 1},
            {"feedback", 0.6},
            {"tone", 0.45},
            {"shape", 2},
            {"to_reverb", 0.8}});
    const auto vocalSpace = reverb({{"decay", 9}, {"predelay_ms", 60}, {"duck", 0.3}});
    const auto shimmer = reverb({{"decay", 14},
            {"size", 0.9},
            {"predelay_ms", 40},
            {"damping", 0.35},
            {"lowcut", 300},
            {"modulation", 0.7},
            {"diffusion", 0.8},
            {"shimmer", 0.5},
            {"duck", 0.2}});
    const auto infinite = reverb({{"decay", 30},
            {"size", 1},
            {"predelay_ms", 20},
            {"damping", 0.3},
            {"lowcut", 300},
            {"modulation", 0.8},
            {"diffusion", 0.85},
            {"shimmer", 0.2}});
    const auto plate = reverb({{"decay", 3},
            {"size", 0.4},
            {"predelay_ms", 15},
            {"damping", 0.5},
            {"lowcut", 200},
            {"modulation", 0.3},
            {"diffusion", 0.9}});
    // Only the echoes reach this one: echo-into-wash throws.
    const auto echoWash = reverb({{"decay", 12},
            {"size", 0.9},
            {"predelay_ms", 0},
            {"damping", 0.4},
            {"lowcut", 300},
            {"modulation", 0.7},
            {"diffusion", 0.8},
            {"direct", 0}});
    const auto off = QMap<QString, double>();

    // A preset: the delay's and the reverb's settings, and which are on (an
    // empty map leaves that unit off at its defaults).
    const auto unit = [](const QString& name,
                              const QMap<QString, double>& delaySettings,
                              const QMap<QString, double>& reverbSettings) {
        SpatialPreset preset{name, {}, true};
        const auto add = [&preset](const QString& group,
                                 const QMap<QString, double>& defaults,
                                 const QMap<QString, double>& settings) {
            const auto& values = settings.isEmpty() ? defaults : settings;
            for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
                preset.values.insert(group + QStringLiteral(",") + it.key(), it.value());
            }
            preset.values.insert(group + QStringLiteral(",enabled"), settings.isEmpty() ? 0 : 1);
        };
        add(QStringLiteral("[SpatialDelay]"), delayDefaults(), delaySettings);
        add(QStringLiteral("[SpatialReverb]"), reverbDefaults(), reverbSettings);
        return preset;
    };
    return {
            // Reverb alone
            unit(QStringLiteral("VOCAL SPACE"), off, vocalSpace),
            unit(QStringLiteral("SHIMMER"), off, shimmer),
            unit(QStringLiteral("INFINITE"), off, infinite),
            unit(QStringLiteral("PLATE"), off, plate),
            // Delay alone
            unit(QStringLiteral("DOTTED PING"), dottedPing, off),
            unit(QStringLiteral("QUARTER ORBIT"), quarterOrbit, off),
            unit(QStringLiteral("TRIPLET CORNERS"), tripletCorners, off),
            unit(QStringLiteral("TRANCE GATE"), tranceGate, off),
            // Together
            unit(QStringLiteral("SPACE ECHO"), dottedPing, vocalSpace),
            unit(QStringLiteral("STAR SHIMMER"), polyStars, shimmer),
            unit(QStringLiteral("WASH THROW"), washThrow, echoWash),
    };
}

void bakeFxMacros() {
    mixxx::spatial::FxMeta meta;
    const QString fx = QStringLiteral("[SpatialFX]");
    meta.space = ControlObject::get(ConfigKey(fx, "space"));
    meta.echo = ControlObject::get(ConfigKey(fx, "echo"));
    meta.tone = ControlObject::get(ConfigKey(fx, "tone"));
    const auto bake = [](const char* group, const char* key, auto function) {
        const ConfigKey configKey(QString::fromLatin1(group), QString::fromLatin1(key));
        ControlObject::set(configKey, function(ControlObject::get(configKey)));
    };
    bake("[SpatialReverb]", "decay", [&](double v) { return meta.reverbDecay(v); });
    bake("[SpatialReverb]", "size", [&](double v) { return meta.reverbSize(v); });
    bake("[SpatialReverb]", "predelay_ms", [&](double v) { return meta.reverbPredelayMs(v); });
    bake("[SpatialReverb]", "damping", [&](double v) { return meta.reverbDamping(v); });
    bake("[SpatialDelay]", "feedback", [&](double v) { return meta.delayFeedback(v); });
    bake("[SpatialDelay]", "to_reverb", [&](double v) { return meta.delayToReverb(v); });
    bake("[SpatialDelay]", "tone", [&](double v) { return meta.delayTone(v); });
    centreFxMacros();
}

void centreFxMacros() {
    for (const char* key : {"space", "echo", "tone"}) {
        ControlObject::set(ConfigKey(QStringLiteral("[SpatialFX]"), QString::fromLatin1(key)), 0.5);
    }
}

} // namespace spatialpresets

// ---------------------------------------------------------------------------

SpatialPresetBank::SpatialPresetBank(const QString& kind,
        const QStringList& keys,
        const QList<SpatialPreset>& factory,
        const QString& settingsPath,
        QObject* pParent)
        : QObject(pParent),
          m_kind(kind),
          m_keys(keys),
          m_filePath(QDir(settingsPath).filePath(QStringLiteral("spatial_presets/%1.json").arg(kind))),
          m_presets(factory),
          m_settingsPath(settingsPath),
          m_legacyImport(!QFile::exists(m_filePath)) {
    load();
}

void SpatialPresetBank::importLegacy(const QString& kind, const QString& group) {
    if (!m_legacyImport) {
        return;
    }
    QFile file(QDir(m_settingsPath).filePath(QStringLiteral("spatial_presets/%1.json").arg(kind)));
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonArray presets =
            QJsonDocument::fromJson(file.readAll()).object().value("presets").toArray();
    bool imported = false;
    for (const auto& item : presets) {
        const QJsonObject object = item.toObject();
        QString name = object.value("name").toString();
        if (name.isEmpty()) {
            continue;
        }
        while (indexOf(name) >= 0) {
            name += QStringLiteral(" (OLD)");
        }
        SpatialPreset preset{name, {}, false};
        const QJsonObject values = object.value("values").toObject();
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            preset.values.insert(group + QStringLiteral(",") + it.key(), it.value().toDouble());
        }
        m_presets.append(preset);
        imported = true;
    }
    if (imported) {
        store();
        emit changed();
    }
}

int SpatialPresetBank::indexOf(const QString& name) const {
    for (int i = 0; i < m_presets.size(); ++i) {
        if (m_presets[i].name.compare(name, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

bool SpatialPresetBank::isFactory(const QString& name) const {
    const int index = indexOf(name);
    return index >= 0 && m_presets[index].factory;
}

void SpatialPresetBank::apply(int index, const QString& group) const {
    if (index < 0 || index >= m_presets.size()) {
        return;
    }
    const auto& values = m_presets[index].values;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        if (m_keys.contains(it.key())) {
            ControlObject::set(resolve(group, it.key()), it.value());
        }
    }
}

bool SpatialPresetBank::matches(int index, const QString& group) const {
    if (index < 0 || index >= m_presets.size()) {
        return false;
    }
    const auto& values = m_presets[index].values;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        if (m_keys.contains(it.key()) &&
                !sameValue(ControlObject::get(resolve(group, it.key())), it.value())) {
            return false;
        }
    }
    return true;
}

void SpatialPresetBank::saveCurrent(const QString& name, const QString& group) {
    SpatialPreset preset{name, {}, false};
    for (const QString& key : m_keys) {
        preset.values.insert(key, ControlObject::get(resolve(group, key)));
    }
    const int index = indexOf(name);
    if (index >= 0 && !m_presets[index].factory) {
        m_presets[index] = preset;
    } else if (index < 0) {
        m_presets.append(preset);
    } else {
        return; // factory names are taken
    }
    store();
    emit changed();
}

bool SpatialPresetBank::remove(const QString& name) {
    const int index = indexOf(name);
    if (index < 0 || m_presets[index].factory) {
        return false;
    }
    m_presets.removeAt(index);
    store();
    emit changed();
    return true;
}

QString SpatialPresetBank::suggestName() const {
    const QString base = m_kind.toUpper();
    for (int n = 1;; ++n) {
        const QString name = QStringLiteral("%1 %2").arg(base).arg(n);
        if (indexOf(name) < 0) {
            return name;
        }
    }
}

void SpatialPresetBank::load() {
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonArray presets =
            QJsonDocument::fromJson(file.readAll()).object().value("presets").toArray();
    for (const auto& item : presets) {
        const QJsonObject object = item.toObject();
        SpatialPreset preset{object.value("name").toString(), {}, false};
        const QJsonObject values = object.value("values").toObject();
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            preset.values.insert(it.key(), it.value().toDouble());
        }
        if (!preset.name.isEmpty() && indexOf(preset.name) < 0) {
            m_presets.append(preset);
        }
    }
}

void SpatialPresetBank::store() const {
    QJsonArray presets;
    for (const SpatialPreset& preset : m_presets) {
        if (preset.factory) {
            continue;
        }
        QJsonObject values;
        for (auto it = preset.values.constBegin(); it != preset.values.constEnd(); ++it) {
            values.insert(it.key(), it.value());
        }
        presets.append(QJsonObject{{"name", preset.name}, {"values", values}});
    }
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QFile file(m_filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(QJsonObject{{"presets", presets}}).toJson());
    }
}

// ---------------------------------------------------------------------------

SpatialPresetBar::SpatialPresetBar(SpatialPresetBank* pBank, const QString& group, QWidget* pParent)
        : QWidget(pParent),
          m_pBank(pBank),
          m_group(group) {
    auto* pLayout = new QHBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(4);
    auto* pLabel = new SpatialLabel(tr("PRESET"), this);
    pLabel->setFixedWidth(64);
    pLayout->addWidget(pLabel);

    auto* pScroll = new QScrollArea(this);
    pScroll->setFrameShape(QFrame::NoFrame);
    pScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pScroll->setWidgetResizable(true);
    pScroll->setFixedHeight(44);
    pScroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget"
                                          " { background: transparent; }"));
    // Swipe sideways through a long list.
    QScroller::grabGesture(pScroll->viewport(), QScroller::TouchGesture);
    auto* pStrip = new QWidget(pScroll);
    m_pChips = new QHBoxLayout(pStrip);
    m_pChips->setContentsMargins(0, 0, 0, 0);
    m_pChips->setSpacing(4);
    pScroll->setWidget(pStrip);
    pLayout->addWidget(pScroll, 1);

    auto* pSave = new SpatialActionButton(tr("SAVE"), [this]() { slotSave(); }, this);
    pSave->setFixedWidth(100);
    m_pDelete = new SpatialActionButton(tr("DELETE"), [this]() { slotDelete(); }, this);
    m_pDelete->setFixedWidth(100);
    pLayout->addWidget(pSave);
    pLayout->addWidget(m_pDelete);

    connect(m_pBank, &SpatialPresetBank::changed, this, &SpatialPresetBar::slotRebuild);
    slotRebuild();
}

void SpatialPresetBar::slotRebuild() {
    for (SpatialActionButton* pButton : std::as_const(m_buttons)) {
        delete pButton;
    }
    m_buttons.clear();
    while (m_pChips->count() > 0) {
        delete m_pChips->takeAt(0);
    }
    const auto& presets = m_pBank->presets();
    for (int i = 0; i < presets.size(); ++i) {
        const QString name = presets[i].name;
        auto* pButton = new SpatialActionButton(name, [this, i, name]() {
            m_pBank->apply(i, m_group);
            if (m_afterLoad) {
                m_afterLoad();
            }
            m_selected = name;
            m_pollCount = 0;
            poll();
        });
        pButton->setMinimumWidth(130);
        // User presets in teal, factory presets in the deck tan.
        pButton->setAccent(presets[i].factory ? kDeck12 : kTeal);
        m_pChips->addWidget(pButton);
        m_buttons.append(pButton);
    }
    m_pChips->addStretch(1);
    m_pollCount = 0;
    poll();
}

void SpatialPresetBar::poll() {
    // Ten times a second is plenty for lighting the matching preset.
    if (m_pollCount++ % 6 != 0) {
        return;
    }
    QString lit;
    for (int i = 0; i < m_buttons.size(); ++i) {
        const bool match = m_pBank->matches(i, m_group);
        m_buttons[i]->setActive(match);
        if (match && (lit.isEmpty() || m_pBank->presets()[i].name == m_selected)) {
            lit = m_pBank->presets()[i].name;
        }
    }
    if (!lit.isEmpty()) {
        m_selected = lit;
    }
}

void SpatialPresetBar::slotSave() {
    QInputDialog dialog(this);
    dialog.setStyleSheet(kDialogStyle);
    dialog.setWindowTitle(tr("Save preset"));
    dialog.setLabelText(tr("Preset name:"));
    const bool userSelected = !m_selected.isEmpty() && !m_pBank->isFactory(m_selected);
    dialog.setTextValue(userSelected ? m_selected : m_pBank->suggestName());
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString name = dialog.textValue().trimmed().toUpper();
    if (name.isEmpty()) {
        return;
    }
    if (m_pBank->isFactory(name)) {
        QMessageBox box(QMessageBox::Information,
                tr("Save preset"),
                tr("\"%1\" is a factory preset. Choose another name.").arg(name),
                QMessageBox::Ok,
                this);
        box.setStyleSheet(kDialogStyle);
        box.exec();
        return;
    }
    if (m_pBank->indexOf(name) >= 0 && name != m_selected) {
        QMessageBox box(QMessageBox::Question,
                tr("Save preset"),
                tr("Replace the preset \"%1\"?").arg(name),
                QMessageBox::Yes | QMessageBox::No,
                this);
        box.setStyleSheet(kDialogStyle);
        if (box.exec() != QMessageBox::Yes) {
            return;
        }
    }
    if (m_beforeSave) {
        m_beforeSave();
    }
    m_pBank->saveCurrent(name, m_group);
    m_selected = name;
}

void SpatialPresetBar::slotDelete() {
    if (m_selected.isEmpty() || m_pBank->isFactory(m_selected) ||
            m_pBank->indexOf(m_selected) < 0) {
        QMessageBox box(QMessageBox::Information,
                tr("Delete preset"),
                tr("Load one of your own presets first (factory presets stay)."),
                QMessageBox::Ok,
                this);
        box.setStyleSheet(kDialogStyle);
        box.exec();
        return;
    }
    QMessageBox box(QMessageBox::Question,
            tr("Delete preset"),
            tr("Delete the preset \"%1\"?").arg(m_selected),
            QMessageBox::Yes | QMessageBox::No,
            this);
    box.setStyleSheet(kDialogStyle);
    if (box.exec() == QMessageBox::Yes) {
        m_pBank->remove(m_selected);
        m_selected.clear();
    }
}
