#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QWidget>

class QHBoxLayout;
class SpatialActionButton;

/// A named set of control values for one Spatial tool.
struct SpatialPreset {
    QString name;
    QMap<QString, double> values; // control key -> value
    bool factory = false;
};

/// The presets of one kind of tool (auto-pan, delay, reverb): factory presets
/// built in, user presets stored as JSON in the settings folder
/// (spatial_presets/<kind>.json). A preset holds the tool's sound settings,
/// not its on/off state, and applies to any group of that kind (both
/// auto-pan units share one bank).
class SpatialPresetBank : public QObject {
    Q_OBJECT
  public:
    SpatialPresetBank(const QString& kind,
            const QStringList& keys,
            const QList<SpatialPreset>& factory,
            const QString& settingsPath,
            QObject* pParent = nullptr);

    const QList<SpatialPreset>& presets() const {
        return m_presets;
    }
    int indexOf(const QString& name) const;
    bool isFactory(const QString& name) const;

    /// Sets `group`'s controls to the preset's values.
    void apply(int index, const QString& group) const;
    /// Whether `group`'s controls currently hold the preset's values.
    bool matches(int index, const QString& group) const;
    /// Stores `group`'s current values as a user preset (replacing one of
    /// the same name) and saves the file.
    void saveCurrent(const QString& name, const QString& group);
    bool remove(const QString& name);

    QString suggestName() const;

  signals:
    void changed();

  private:
    void load();
    void store() const;

    const QString m_kind;
    const QStringList m_keys;
    const QString m_filePath;
    QList<SpatialPreset> m_presets; // factory first, then user
};

/// A preset strip for a tool panel: a scrolling row of preset buttons (tap
/// to load; the one matching the current settings lights), SAVE (asks for a
/// name) and DELETE (user presets only).
class SpatialPresetBar : public QWidget {
    Q_OBJECT
  public:
    SpatialPresetBar(SpatialPresetBank* pBank, const QString& group, QWidget* pParent = nullptr);
    /// Call regularly: lights the matching preset.
    void poll();

  private slots:
    void slotRebuild();
    void slotSave();
    void slotDelete();

  private:
    SpatialPresetBank* m_pBank;
    const QString m_group;
    QHBoxLayout* m_pChips;
    QList<SpatialActionButton*> m_buttons;
    SpatialActionButton* m_pDelete;
    QString m_selected;
    int m_pollCount = 0;
};

/// The factory banks.
namespace spatialpresets {
QList<SpatialPreset> autopanFactory();
QList<SpatialPreset> delayFactory();
QList<SpatialPreset> reverbFactory();
QStringList autopanKeys();
QStringList delayKeys();
QStringList reverbKeys();
} // namespace spatialpresets
