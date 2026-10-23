#include "Assets/SvgMeshCooker.h"
#include "Platform/Shell.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

using namespace GameEngine;

namespace
{

SvgCookedMesh Cook(std::string_view source)
{
    SvgCookedMesh mesh;
    std::string reason;
    EXPECT_EQ(CookSvgToMesh(source, mesh, &reason), SvgMeshCookStatus::Success) << reason;
    return mesh;
}

float TriangleArea(const SvgMeshTriangle& triangle)
{
    return std::abs((triangle.X1 - triangle.X0) * (triangle.Y2 - triangle.Y0) -
                    (triangle.Y1 - triangle.Y0) * (triangle.X2 - triangle.X0)) * 0.5f;
}

float MeshArea(const SvgCookedMesh& mesh)
{
    float area = 0.0f;
    for (const SvgMeshTriangle& triangle : mesh.Triangles)
        area += TriangleArea(triangle);
    return area;
}

std::array<float, 4> MeshBounds(const SvgCookedMesh& mesh)
{
    std::array<float, 4> bounds{
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const SvgMeshTriangle& triangle : mesh.Triangles)
    {
        bounds[0] = std::min({bounds[0], triangle.X0, triangle.X1, triangle.X2});
        bounds[1] = std::min({bounds[1], triangle.Y0, triangle.Y1, triangle.Y2});
        bounds[2] = std::max({bounds[2], triangle.X0, triangle.X1, triangle.X2});
        bounds[3] = std::max({bounds[3], triangle.Y0, triangle.Y1, triangle.Y2});
    }
    return bounds;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(SvgMeshCooker, TessellatesSolidFillAndMarksOnlyTheOutline)
{
    const SvgCookedMesh mesh = Cook(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="10" height="20">
          <rect width="10" height="20" fill="#336699"/>
        </svg>)svg");

    ASSERT_EQ(mesh.Triangles.size(), 2u);
    EXPECT_FLOAT_EQ(mesh.Width, 10.0f);
    EXPECT_FLOAT_EQ(mesh.Height, 20.0f);
    EXPECT_NEAR(MeshArea(mesh), 200.0f, 0.001f);
    int ownedInteriorEdges = 0;
    for (const SvgMeshTriangle& triangle : mesh.Triangles)
    {
        EXPECT_EQ(std::popcount(static_cast<unsigned>(triangle.EdgeFlags & 0x7u)), 2);
        ownedInteriorEdges += std::popcount(
            static_cast<unsigned>((triangle.EdgeFlags >> 3u) & 0x7u));
        EXPECT_EQ(triangle.Color, 0xFF996633u);
    }
    EXPECT_EQ(ownedInteriorEdges, 1);
}

TEST(SvgMeshCooker, PreservesEvenOddHoles)
{
    const SvgCookedMesh mesh = Cook(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">
          <path fill="#fff" fill-rule="evenodd"
                d="M0 0H10V10H0Z M3 3H7V7H3Z"/>
        </svg>)svg");

    ASSERT_FALSE(mesh.Triangles.empty());
    EXPECT_NEAR(MeshArea(mesh), 84.0f, 0.001f);
}

TEST(SvgMeshCooker, ExpandsRoundStrokeGeometry)
{
    const SvgCookedMesh mesh = Cook(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
          <path d="M13 4L6 12L3 9" fill="none" stroke="white" stroke-width="2"
                stroke-linecap="round" stroke-linejoin="round"/>
        </svg>)svg");

    ASSERT_FALSE(mesh.Triangles.empty());
    const auto bounds = MeshBounds(mesh);
    EXPECT_LT(bounds[0], 3.0f);
    EXPECT_LT(bounds[1], 4.0f);
    EXPECT_GT(bounds[2], 13.0f);
    EXPECT_GT(bounds[3], 12.0f);
    for (const SvgMeshTriangle& triangle : mesh.Triangles)
    {
        EXPECT_TRUE(std::isfinite(triangle.X0));
        EXPECT_TRUE(std::isfinite(triangle.Y0));
        EXPECT_GT(TriangleArea(triangle), 0.0f);
    }
}

TEST(SvgMeshCooker, AppliesGroupTransformAndOpacity)
{
    const SvgCookedMesh mesh = Cook(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
          <g transform="translate(4 5)" opacity="0.5">
            <rect x="1" y="2" width="3" height="4" fill="#336699"/>
          </g>
        </svg>)svg");

    const auto bounds = MeshBounds(mesh);
    EXPECT_NEAR(bounds[0], 5.0f, 0.001f);
    EXPECT_NEAR(bounds[1], 7.0f, 0.001f);
    EXPECT_NEAR(bounds[2], 8.0f, 0.001f);
    EXPECT_NEAR(bounds[3], 11.0f, 0.001f);
    ASSERT_FALSE(mesh.Triangles.empty());
    EXPECT_EQ(mesh.Triangles.front().Color, 0x80996633u);
}

TEST(SvgMeshCooker, RequestsRasterFallbackForOverlappingGroupOpacity)
{
    for (const auto source : {
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
          <g opacity="0.5"><rect width="20" height="20" fill="red"/>
          <rect x="10" y="10" width="20" height="20" fill="blue"/></g></svg>)svg",
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
          <g opacity="0.5"><g><rect width="20" height="20" fill="red"/>
          <rect x="10" y="10" width="20" height="20" fill="blue"/></g></g></svg>)svg"})
    {
        SvgCookedMesh mesh;
        std::string reason;
        EXPECT_EQ(CookSvgToMesh(source, mesh, &reason), SvgMeshCookStatus::Unsupported);
        EXPECT_TRUE(mesh.Triangles.empty());
        EXPECT_NE(reason.find("opacity"), std::string::npos) << reason;
    }
}

TEST(SvgMeshCooker, RequestsRasterFallbackForFillAndStrokeObjectOpacity)
{
    SvgCookedMesh mesh;
    std::string reason;
    EXPECT_EQ(CookSvgToMesh(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
          <rect x="4" y="4" width="20" height="20" fill="red" stroke="blue"
                stroke-width="4" opacity="0.5"/>
        </svg>)svg", mesh, &reason), SvgMeshCookStatus::Unsupported);
    EXPECT_TRUE(mesh.Triangles.empty());
    EXPECT_NE(reason.find("opacity"), std::string::npos) << reason;
}

TEST(SvgMeshCooker, KeepsOpaqueGroupsAndIndependentFillOpacityOnMeshPath)
{
    const auto mesh = Cook(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
          <g><rect width="20" height="20" fill="red" fill-opacity="0.5"/>
          <rect x="10" y="10" width="20" height="20" fill="blue" fill-opacity="0.5"/></g>
        </svg>)svg");
    EXPECT_FALSE(mesh.Triangles.empty());
}

TEST(SvgMeshCooker, RequestsRasterFallbackForGradientPaint)
{
    constexpr std::string_view source = R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
          <defs><linearGradient id="g"><stop stop-color="red"/><stop offset="1" stop-color="blue"/></linearGradient></defs>
          <rect width="16" height="16" fill="url(#g)"/>
        </svg>)svg";
    SvgCookedMesh mesh;
    std::string reason;

    EXPECT_EQ(CookSvgToMesh(source, mesh, &reason), SvgMeshCookStatus::Unsupported);
    EXPECT_TRUE(mesh.Triangles.empty());
    EXPECT_NE(reason.find("gradient"), std::string::npos);
}

TEST(SvgMeshCooker, RequestsRasterFallbackForAuthoredClipPath)
{
    constexpr std::string_view source = R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
          <defs><clipPath id="clip"><circle cx="8" cy="8" r="4"/></clipPath></defs>
          <rect width="16" height="16" fill="white" clip-path="url(#clip)"/>
        </svg>)svg";
    SvgCookedMesh mesh;
    std::string reason;

    EXPECT_EQ(CookSvgToMesh(source, mesh, &reason), SvgMeshCookStatus::Unsupported);
    EXPECT_TRUE(mesh.Triangles.empty());
    EXPECT_NE(reason.find("clip"), std::string::npos);
}

TEST(SvgMeshCooker, CooksShippedEditorLineIcons)
{
    const auto iconDir = Platform::GetExecutablePath().parent_path() / "TestData" / "SvgMeshIcons";
    const std::array icons{
        "checkbox-checkmark.svg",
        "DockLog.svg",
        "DockScriptErrors.svg",
        "DockShaderErrors.svg",
        "DockClipEditor.svg",
        "SettingsUI.svg",
    };

    for (const char* icon : icons)
    {
        SCOPED_TRACE(icon);
        const std::string source = ReadFile(iconDir / icon);
        ASSERT_FALSE(source.empty());
        SvgCookedMesh mesh;
        std::string reason;
        EXPECT_EQ(CookSvgToMesh(source, mesh, &reason), SvgMeshCookStatus::Success) << reason;
        EXPECT_FALSE(mesh.Triangles.empty());
        EXPECT_LE(mesh.Triangles.size(), 4096u);
    }
}


TEST(SvgMeshCooker, ClipsOverflowingGeometryToTheIntrinsicViewport)
{
    const auto mesh = Cook(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">
          <rect x="-10" y="-5" width="30" height="20" fill="#ff0000"/>
        </svg>)svg");
    const auto bounds = MeshBounds(mesh);
    EXPECT_FLOAT_EQ(bounds[0], 0.0f);
    EXPECT_FLOAT_EQ(bounds[1], 0.0f);
    EXPECT_FLOAT_EQ(bounds[2], 10.0f);
    EXPECT_FLOAT_EQ(bounds[3], 10.0f);
    EXPECT_NEAR(MeshArea(mesh), 100.0f, 0.001f);
}

TEST(SvgMeshCooker, ClipsAfterTheViewBoxTransform)
{
    const auto mesh = Cook(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="20" height="10"
             viewBox="0 0 10 10" preserveAspectRatio="none">
          <rect x="-5" y="2" width="20" height="6" fill="#ff0000"/>
        </svg>)svg");
    const auto bounds = MeshBounds(mesh);
    EXPECT_FLOAT_EQ(bounds[0], 0.0f);
    EXPECT_FLOAT_EQ(bounds[1], 2.0f);
    EXPECT_FLOAT_EQ(bounds[2], 20.0f);
    EXPECT_FLOAT_EQ(bounds[3], 8.0f);
    EXPECT_NEAR(MeshArea(mesh), 120.0f, 0.001f);
}

TEST(SvgMeshCooker, ComposesNestedAffineTransformsInAuthoredOrder)
{
    const auto mesh = Cook(R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
          <g transform="translate(10 20)"><g transform="matrix(2 0 0 3 4 5)">
            <rect x="1" y="2" width="3" height="4" fill="red"/>
          </g></g>
        </svg>)svg");
    const auto bounds = MeshBounds(mesh);
    EXPECT_NEAR(bounds[0], 16.0f, .001f);
    EXPECT_NEAR(bounds[1], 31.0f, .001f);
    EXPECT_NEAR(bounds[2], 22.0f, .001f);
    EXPECT_NEAR(bounds[3], 43.0f, .001f);
    EXPECT_NEAR(MeshArea(mesh), 72.0f, .001f);
}
