#pragma once

#include <QString>

#define CRATE_TABLE "crates"
#define CRATE_TRACKS_TABLE "crate_tracks"

const QString CRATETABLE_ID = QStringLiteral("id");
const QString CRATETABLE_NAME = QStringLiteral("name");

// Id of the crate this crate is nested inside, or NULL for a crate at the
// top level of the crate tree.
const QString CRATETABLE_PARENT_ID = QStringLiteral("parent_id");

// TODO(XXX): Fix AutoDJ database design.
// Crates should have no dependency on AutoDJ stuff. Which
// crates are used as a source for AutoDJ has to be stored
// and managed by the AutoDJ component in a separate table.
// This refactoring should be deferred until consensus on the
// redesign of the AutoDJ feature has been reached. The main
// ideas of the new design should be documented for verification
// before starting to code.
const QString CRATETABLE_AUTODJ_SOURCE = QStringLiteral("autodj_source");

const QString CRATETRACKSTABLE_CRATEID = QStringLiteral("crate_id");
const QString CRATETRACKSTABLE_TRACKID = QStringLiteral("track_id");
