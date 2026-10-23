#pragma once

#include "AssetCore/GUID.h"
#include "UI/UIPrimitive.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine
{
class AssetManager;
struct SvgCookedMesh;

// Owns SVG background cooking, raster fallback decisions, and mesh emission.
class SvgBackgroundRenderer
{
public:
    explicit SvgBackgroundRenderer(AssetManager* assets) : m_AssetManager(assets) {}
    std::shared_ptr<const SvgCookedMesh> GetOrCook(const GUID& guid);
    void Evict(const GUID& guid) { m_Cache.erase(guid); m_Fallback.erase(guid); }
    size_t CachedCount() const { return m_Cache.size(); }
    static void Emit(const SvgCookedMesh& mesh, float x, float y, float width, float height,
                     uint32_t tint, float saturation, uint16_t clip, float opacity,
                     std::vector<UI::UIPrimitive>& out);
private:
    AssetManager* m_AssetManager;
    std::unordered_map<GUID, std::shared_ptr<const SvgCookedMesh>> m_Cache;
    std::unordered_set<GUID> m_Fallback;
};
}
