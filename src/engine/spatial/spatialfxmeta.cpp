#include "engine/spatial/spatialfxmeta.h"

#include "engine/spatial/spatialcontrols.h"

namespace mixxx::spatial {

SpatialFxMetaControls::SpatialFxMetaControls()
        : m_pSpace(makePot(kGroup, "space", 0, 1, 0.5)),
          m_pEcho(makePot(kGroup, "echo", 0, 1, 0.5)),
          m_pTone(makePot(kGroup, "tone", 0, 1, 0.5)),
          m_pLevel(makePot(kGroup, "level", 0, 1, 1)) {
}

SpatialFxMetaControls::~SpatialFxMetaControls() = default;

FxMeta SpatialFxMetaControls::values() const {
    FxMeta meta;
    meta.space = m_pSpace->get();
    meta.echo = m_pEcho->get();
    meta.tone = m_pTone->get();
    meta.level = std::clamp(m_pLevel->get(), 0.0, 1.0);
    return meta;
}

} // namespace mixxx::spatial
