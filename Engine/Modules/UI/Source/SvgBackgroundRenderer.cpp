#include "UI/SvgBackgroundRenderer.h"
#include "Assets/AssetManager.h"
#include "Assets/SvgMeshCooker.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>

namespace GameEngine
{
namespace
{
uint32_t ModulateSvgMeshColor(uint32_t color, uint32_t tint, float saturation)
{
    auto channel = [](uint32_t value, uint32_t shift) {
        return static_cast<float>((value >> shift) & 0xFFu);
    };
    float r = channel(color, 0u);
    float g = channel(color, 8u);
    float b = channel(color, 16u);

    const float sat = std::clamp(saturation, 0.0f, 1.0f);
    const float luma = r * 0.2126f + g * 0.7152f + b * 0.0722f;
    r = luma + (r - luma) * sat;
    g = luma + (g - luma) * sat;
    b = luma + (b - luma) * sat;
    r *= channel(tint, 0u) / 255.0f;
    g *= channel(tint, 8u) / 255.0f;
    b *= channel(tint, 16u) / 255.0f;
    const float a = channel(color, 24u) * channel(tint, 24u) / 255.0f;

    const auto pack = [](float value) {
        return static_cast<uint32_t>(std::clamp(std::lround(value), 0l, 255l));
    };
    return pack(r) | (pack(g) << 8u) | (pack(b) << 16u) | (pack(a) << 24u);
}

}
std::shared_ptr<const SvgCookedMesh> SvgBackgroundRenderer::GetOrCook(const GUID& guid)
{
    if (guid.IsNull() || !m_AssetManager)
        return {};

    if (const auto cached = m_Cache.find(guid); cached != m_Cache.end())
        return cached->second;
    if (m_Fallback.contains(guid))
        return {};

    AssetMetadata metadata;
    if (!m_AssetManager->GetRegistry().TryGetAssetMetadata(guid, metadata) ||
        metadata.Path.empty())
    {
        m_Fallback.insert(guid);
        return {};
    }

    std::string extension = metadata.Path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".svg")
    {
        m_Fallback.insert(guid);
        return {};
    }

    std::ifstream file(metadata.Path, std::ios::binary);
    if (!file)
    {
        m_Fallback.insert(guid);
        return {};
    }
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

    auto mesh = std::make_shared<SvgCookedMesh>();
    std::string reason;
    const SvgMeshCookStatus status = CookSvgToMesh(source, *mesh, &reason);
    if (status != SvgMeshCookStatus::Success)
    {
        m_Fallback.insert(guid);
        Logger::Log::Debug("[UI SVG Mesh] Raster fallback for '{}': {}",
                           metadata.Path.string(), reason);
        return {};
    }

    Logger::Log::Debug("[UI SVG Mesh] Cooked '{}' into {} triangles",
                       metadata.Path.string(), mesh->Triangles.size());
    m_Cache.emplace(guid, mesh);
    return mesh;
}

void SvgBackgroundRenderer::Emit(const SvgCookedMesh& mesh, float x, float y,
                                float width, float height, uint32_t tint,
                                float saturation, uint16_t clip, float opacity,
                                std::vector<UI::UIPrimitive>& out)
{
    const float scaleX = width / std::max(mesh.Width, 0.001f);
    const float scaleY = height / std::max(mesh.Height, 0.001f);
    for (const SvgMeshTriangle& triangle : mesh.Triangles)
    {
        UI::UIPrimitive primitive = UI::MakeMeshTriangle(
            x + triangle.X0 * scaleX, y + triangle.Y0 * scaleY,
            x + triangle.X1 * scaleX, y + triangle.Y1 * scaleY,
            x + triangle.X2 * scaleX, y + triangle.Y2 * scaleY,
            ModulateSvgMeshColor(triangle.Color, tint, saturation),
            triangle.EdgeFlags, clip);
        UI::SetOpacity(primitive, opacity);
        out.push_back(primitive);
    }
}
}
