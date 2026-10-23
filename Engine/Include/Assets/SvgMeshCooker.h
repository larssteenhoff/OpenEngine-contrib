#pragma once

#include "Types/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

/// One solid-color triangle produced by the SVG cooker. EdgeFlags bits 0..2
/// mark silhouette edges (v0,v1), (v1,v2), and (v2,v0); bits 3..5 assign
/// ownership of shared interior edges so translucent triangles neither gap
/// nor double-blend along the tessellation.
struct SvgMeshTriangle
{
    float32 X0 = 0.0f;
    float32 Y0 = 0.0f;
    float32 X1 = 0.0f;
    float32 Y1 = 0.0f;
    float32 X2 = 0.0f;
    float32 Y2 = 0.0f;
    uint32 Color = 0;
    uint8 EdgeFlags = 0;
};

struct SvgCookedMesh
{
    float32 Width = 0.0f;
    float32 Height = 0.0f;
    std::vector<SvgMeshTriangle> Triangles;
};

enum class SvgMeshCookStatus : uint8
{
    Success,
    Unsupported,
    Invalid
};

struct SvgMeshCookOptions
{
    /// Maximum source-space deviation used while flattening cubic curves.
    /// Zero selects an adaptive value derived from the SVG's longest axis.
    float32 CurveTolerance = 0.0f;
    uint32 MaxTriangles = 4096;
};

/// Parse and tessellate solid SVG fills and strokes into reusable geometry.
/// Gradients, images, text, masks, clip paths, dashed strokes, and opacity
/// requiring isolated group or fill/stroke compositing report
/// Unsupported so callers can preserve their raster fallback.
SvgMeshCookStatus CookSvgToMesh(std::string_view svgUtf8,
                                SvgCookedMesh& outMesh,
                                std::string* outReason = nullptr,
                                const SvgMeshCookOptions& options = {});

} // namespace GameEngine
