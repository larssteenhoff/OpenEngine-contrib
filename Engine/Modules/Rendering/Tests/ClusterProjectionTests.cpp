// The cluster bounds of Shaders/Includes/cluster_projection.glsl, compiled as C++ through
// GlslShim.h so these tests execute the shipped functions. Every case starts from the projection
// the camera builds (MatrixOps.h) and reads the same inverse-projection elements the cull shader
// reads, so a wrong index or a wrong reconstruction fails here.

#include <gtest/gtest.h>

#include "GlslShim.h"
#include "Mathematics/MatrixOps.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::GlslShim::ClusterProjection
{
#include "../Shaders/Includes/cluster_projection.glsl"
} // namespace GameEngine::GlslShim::ClusterProjection

namespace
{
namespace Glsl = GameEngine::GlslShim;
namespace Cluster = GameEngine::GlslShim::ClusterProjection;
namespace Math = GameEngine::Mathematics;
using Glsl::vec3;
using Glsl::vec4;

constexpr float kNearZ = 0.1f;
constexpr float kFarZ = 100.0f;
constexpr int kDepthSlices = 24;

// The inverse-projection elements GE_ClusterViewZ takes. Data() is column-major, so
// Data()[10], [14], [11], [15] are the GLSL [column,row] elements [2,2], [3,2], [2,3], [3,3].
struct ViewZCoefficients
{
    float ZDepth = 0.0f;
    float ZConstant = 0.0f;
    float WDepth = 0.0f;
    float WConstant = 0.0f;
};

ViewZCoefficients CoefficientsOf(const Math::Matrix4x4& projection)
{
    const Math::Matrix4x4 inverse = Math::Inverse(projection);
    const float* inv = inverse.Data();
    return {inv[10], inv[14], inv[11], inv[15]};
}

// The reverse-Z depth the projection writes for a point at view distance viewZ.
float DepthOf(const Math::Matrix4x4& projection, float viewZ)
{
    const glm::vec4 clip = projection.GetGLM() * glm::vec4(0.0f, 0.0f, viewZ, 1.0f);
    return clip.z / clip.w;
}

float ViewZOf(const ViewZCoefficients& c, float depth)
{
    return Cluster::GE_ClusterViewZ(depth, c.ZDepth, c.ZConstant, c.WDepth, c.WConstant);
}

// The logarithmic slice the cull shader assigns view distance viewZ to.
int SliceOf(float viewZ, float nearZ, float farZ)
{
    const float t = std::log(viewZ / nearZ) / std::log(farZ / nearZ);
    return static_cast<int>(std::floor(t * static_cast<float>(kDepthSlices)));
}

// A tile's four side planes, built from the inverse projection as the cull shader builds them:
// far corners at depth 0, near corners at depth 1, each plane anchored on a near corner.
struct TilePlanes
{
    vec4 Left;
    vec4 Right;
    vec4 Top;
    vec4 Bottom;
};

vec3 Unproject(const glm::mat4& invProj, float x, float y, float depth)
{
    const glm::vec4 p = invProj * glm::vec4(x, y, depth, 1.0f);
    const float w = std::max(std::abs(p.w), 1e-6f);
    return {p.x / w, p.y / w, p.z / w};
}

TilePlanes TilePlanesOf(const Math::Matrix4x4& projection, float x0, float x1, float y0, float y1)
{
    const glm::mat4 invProj = Math::Inverse(projection).GetGLM();
    const vec3 pTL = Unproject(invProj, x0, y1, 0.0f);
    const vec3 pTR = Unproject(invProj, x1, y1, 0.0f);
    const vec3 pBL = Unproject(invProj, x0, y0, 0.0f);
    const vec3 pBR = Unproject(invProj, x1, y0, 0.0f);
    const vec3 pNearTL = Unproject(invProj, x0, y1, 1.0f);
    const vec3 pNearBR = Unproject(invProj, x1, y0, 1.0f);
    const vec3 inside = (pTL + pTR + pBL + pBR) * 0.125f + (pNearTL + pNearBR) * 0.25f;
    return {Cluster::GE_ClusterInwardPlane(pNearTL, pBL, pTL, inside),
            Cluster::GE_ClusterInwardPlane(pNearBR, pTR, pBR, inside),
            Cluster::GE_ClusterInwardPlane(pNearTL, pTL, pTR, inside),
            Cluster::GE_ClusterInwardPlane(pNearBR, pBR, pBL, inside)};
}
} // namespace

TEST(ClusterProjection, OrthographicDepthReconstructsTheViewZOfEverySlice)
{
    const Math::Matrix4x4 projection =
        Math::MakeOrthographicLH_ZO_ReverseZ(-10.0f, 10.0f, -8.0f, 8.0f, kNearZ, kFarZ);
    const ViewZCoefficients c = CoefficientsOf(projection);
    for (const float viewZ : {kNearZ, 1.0f, 20.0f, 75.0f, kFarZ})
        EXPECT_NEAR(ViewZOf(c, DepthOf(projection, viewZ)), viewZ, 1e-4f) << "viewZ " << viewZ;
    // The reconstructed Z lies in the light's slice (samples away from the slice boundaries).
    for (const float viewZ : {0.15f, 2.0f, 20.0f, 75.0f, 99.0f})
        EXPECT_EQ(SliceOf(ViewZOf(c, DepthOf(projection, viewZ)), kNearZ, kFarZ), SliceOf(viewZ, kNearZ, kFarZ))
            << "viewZ " << viewZ;
}

TEST(ClusterProjection, PerspectiveDepthReconstructsViewZ)
{
    const Math::Matrix4x4 projection = Math::MakePerspectiveLH_ZO_ReverseZ(1.0f, 16.0f / 9.0f, kNearZ, kFarZ);
    const ViewZCoefficients c = CoefficientsOf(projection);
    for (const float viewZ : {kNearZ, 1.0f, 20.0f, kFarZ})
        EXPECT_NEAR(ViewZOf(c, DepthOf(projection, viewZ)), viewZ, viewZ * 1e-4f) << "viewZ " << viewZ;
}

TEST(ClusterProjection, PerspectiveDepthReconstructsViewZWithAFarPlaneInTheMillions)
{
    constexpr float kHugeFarZ = 1e7f;
    const Math::Matrix4x4 projection = Math::MakePerspectiveLH_ZO_ReverseZ(1.0f, 16.0f / 9.0f, kNearZ, kHugeFarZ);
    const ViewZCoefficients c = CoefficientsOf(projection);
    for (const float viewZ : {1.0f, 1e3f, 1e6f, 5e6f, kHugeFarZ})
        EXPECT_NEAR(ViewZOf(c, DepthOf(projection, viewZ)), viewZ, viewZ * 1e-3f) << "viewZ " << viewZ;
}

TEST(ClusterProjection, OrthographicTilePlanesKeepTheTileWidthAtEveryDepth)
{
    // Tile x = [4, 6] of a view 20 wide: ndc x = [0.4, 0.6].
    const Math::Matrix4x4 projection =
        Math::MakeOrthographicLH_ZO_ReverseZ(-10.0f, 10.0f, -8.0f, 8.0f, kNearZ, kFarZ);
    const TilePlanes planes = TilePlanesOf(projection, 0.4f, 0.6f, -0.125f, 0.125f);
    for (const float z : {kNearZ, 20.0f, kFarZ})
    {
        EXPECT_NEAR(Cluster::GE_ClusterPlaneDistance(planes.Left, {5, 0, z}), 1, 1e-4f);
        EXPECT_NEAR(Cluster::GE_ClusterPlaneDistance(planes.Right, {5, 0, z}), 1, 1e-4f);
        EXPECT_LT(Cluster::GE_ClusterPlaneDistance(planes.Left, {3, 0, z}), -0.5f);
        EXPECT_LT(Cluster::GE_ClusterPlaneDistance(planes.Right, {7, 0, z}), -0.5f);
        EXPECT_GT(Cluster::GE_ClusterPlaneDistance(planes.Top, {5, 0, z}), 0);
        EXPECT_GT(Cluster::GE_ClusterPlaneDistance(planes.Bottom, {5, 0, z}), 0);
    }
}

TEST(ClusterProjection, PerspectiveTilePlanesMeetAtTheEyeWithAFarPlaneInTheMillions)
{
    for (const float farZ : {kFarZ, 1e7f})
    {
        const Math::Matrix4x4 projection = Math::MakePerspectiveLH_ZO_ReverseZ(1.0f, 1.0f, kNearZ, farZ);
        const TilePlanes planes = TilePlanesOf(projection, -0.5f, -0.25f, 0.25f, 0.5f);
        for (const vec4& plane : {planes.Left, planes.Right, planes.Top, planes.Bottom})
            EXPECT_NEAR(Cluster::GE_ClusterPlaneDistance(plane, {0, 0, 0}), 0, 1e-4f) << "far " << farZ;
        // A light on the tile's centre ray, close to the eye and far down the view, is inside.
        const float tanHalf = std::tan(0.5f);
        for (const float z : {1.0f, 1000.0f})
        {
            const vec3 centre{-0.375f * tanHalf * z, 0.375f * tanHalf * z, z};
            for (const vec4& plane : {planes.Left, planes.Right, planes.Top, planes.Bottom})
                EXPECT_GT(Cluster::GE_ClusterPlaneDistance(plane, centre), 0) << "far " << farZ << " z " << z;
        }
    }
}
