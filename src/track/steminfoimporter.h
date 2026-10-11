#pragma once

#include <QMimeType>

#include "track/steminfo.h"

namespace mixxx {

/// Importer class for StemInfo objects that contains metadata about the stems of a track.
class StemInfoImporter {
  public:
    static QList<StemInfo> importStemInfos(
            const QString& filePath);

    /// Spatial Mixxx's stem names and colours, whatever the file says:
    /// DRUMS, BASS (folded into INST), INST, VOCAL, with VOCAL yellow.
    static void applySpatialStemLayout(QList<StemInfo>* pStemInfos);

    static bool maybeStemFile(
            const QString& aFileName, QMimeType mimeType = QMimeType());

    static bool hasStemAtom(
            const QString& aFileName);
};

} // namespace mixxx
