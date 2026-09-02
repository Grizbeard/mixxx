#pragma once

#include <QObject>

#include "library/trackset/crate/crateid.h"
#include "preferences/usersettings.h"

class TrackCollection;
class Crate;

class CrateFeatureHelper : public QObject {
    Q_OBJECT

  public:
    CrateFeatureHelper(
            TrackCollection* pTrackCollection,
            UserSettingsPointer pConfig);
    ~CrateFeatureHelper() override = default;

    /// Ask for a name and create an empty crate, nested inside parentId if a
    /// valid id is given, otherwise at the top level.
    CrateId createEmptyCrate(CrateId parentId = CrateId());
    CrateId duplicateCrate(const Crate& oldCrate);

    /// Return initialName if no crate uses it, otherwise the same name with a
    /// " 2", " 3", ... suffix appended until it is free. Crate names are
    /// unique across the whole collection, not just among siblings.
    QString proposeNameForNewCrate(
            const QString& initialName = QString()) const;

  private:
    TrackCollection* m_pTrackCollection;

    UserSettingsPointer m_pConfig;
};
