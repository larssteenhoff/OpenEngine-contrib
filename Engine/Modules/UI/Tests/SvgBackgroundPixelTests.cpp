#include "Assets/SvgMeshCooker.h"
#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"
#include "UI/SvgBackgroundRenderer.h"

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{
class ClippedSvgProbe : public UIElement
{
  public:
    explicit ClippedSvgProbe(SvgCookedMesh mesh) : m_Mesh(std::move(mesh)) {}

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                              float x, float y, float width, float height) override
    {
        std::vector<UI::UIPrimitive> primitives;
        SvgBackgroundRenderer::Emit(m_Mesh, x, y, width, height, 0xFFFFFFFFu,
                                    1.0f, UI::kNoClip, 1.0f, primitives);
        for (const auto& primitive : primitives)
            ctx.Emit(primitive);
    }

  private:
    SvgCookedMesh m_Mesh;
};
}

TEST(SvgBackgroundPixels, ClippedFractionalEdgesHaveNeitherHolesNorDoubleBlending)
{
    for (const bool translucent : {false, true})
    {
        SCOPED_TRACE(translucent ? "half-alpha" : "opaque");
        const std::string source = std::string(R"svg(
            <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">
              <rect x="-10" y="0" width="30" height="10" fill="#ff0000" fill-opacity=")svg") +
            (translucent ? "0.5" : "1") + R"svg("/></svg>)svg";
        SvgCookedMesh mesh;
        std::string reason;
        ASSERT_EQ(CookSvgToMesh(source, mesh, &reason), SvgMeshCookStatus::Success) << reason;
        ASSERT_FALSE(mesh.Triangles.empty());

        UITesting::IsolatedUIFixture fixture;
        const bool built = fixture.Build(1.0f, R"(<uielement id="root"/>)", R"css(
            #root { width: 800px; height: 600px; background-color: #000000; }
            #probe { width: 10px; height: 10px; margin-left: 154px; margin-top: 433px; }
        )css");
        if (!fixture.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fixture.Diagnostic();
        auto probe = std::make_unique<ClippedSvgProbe>(std::move(mesh));
        probe->SetId("probe");
        fixture.Element("root")->AddChild(std::move(probe));
        fixture.Settle();
        const auto box = fixture.BorderBox("probe");
        ASSERT_FLOAT_EQ(box.X, 154.0f);
        ASSERT_FLOAT_EQ(box.Y, 433.0f);
        ASSERT_FLOAT_EQ(box.W, 10.0f);
        ASSERT_FLOAT_EQ(box.H, 10.0f);
        ASSERT_FALSE(fixture.Primitives("probe", UI::PrimitiveMode::MeshTriangle).empty());
        const auto pixels = UITesting::RenderUiToBytes(fixture.Manager(), UI::UITargetSpace::EncodedSrgb());
        ASSERT_FALSE(pixels.empty());
        EXPECT_EQ(UITesting::PixelAt(pixels, 153u, 438u).R, 0);
        EXPECT_EQ(UITesting::PixelAt(pixels, 164u, 438u).R, 0);
        for (uint32_t y = 433u; y < 443u; ++y)
        {
            for (uint32_t x = 154u; x < 164u; ++x)
            {
                const auto pixel = UITesting::PixelAt(pixels, x, y);
                EXPECT_NEAR(pixel.R, translucent ? 128 : 255, 1) << x << "," << y;
                EXPECT_EQ(pixel.G, 0) << x << "," << y;
                EXPECT_EQ(pixel.B, 0) << x << "," << y;
            }
        }
    }
}
