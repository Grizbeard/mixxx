#pragma once

#include "library/trackset/crate/crateid.h"
#include "library/trackset/tracksettablemodel.h"

class CrateTableModel final : public TrackSetTableModel {
    Q_OBJECT

  public:
    CrateTableModel(QObject* parent, TrackCollectionManager* pTrackCollectionManager);
    ~CrateTableModel() final = default;

    /// Show the tracks of a crate. With includeSubcrateTracks the tracks of
    /// every crate nested below it are listed as well, each track once.
    void selectCrate(CrateId crateId = CrateId(), bool includeSubcrateTracks = false);
    CrateId selectedCrate() const {
        return m_selectedCrate;
    }

    bool addTrack(const QModelIndex& index, const QString& location);

    void removeTracks(const QModelIndexList& indices) final;
    /// Returns the number of unsuccessful additions.
    int addTracksWithTrackIds(const QModelIndex& index,
            const QList<TrackId>& tracks,
            int* pOutInsertionPos) final;
    bool isLocked() final;

    Capabilities getCapabilities() const final;
    QString modelKey(bool noSearch) const override;

  private:
    CrateId m_selectedCrate;
    // Whether the current selection includes the tracks of nested crates.
    // Kept so that toggling the setting re-selects instead of being mistaken
    // for a repeated selection of the same crate.
    bool m_selectedCrateIncludesSubcrateTracks = false;
    QHash<CrateId, QString> m_searchTexts;
};
