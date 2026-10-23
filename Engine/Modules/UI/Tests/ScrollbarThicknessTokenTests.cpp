// A scroll view's bar thickness comes from the --ui_scrollbar_thickness token, so a layout
// beside a scroll view (a panel header over its fields) can reserve the same gutter from the
// stylesheet. Without the token the bar keeps Scrollbar::kDefaultThicknessPx.

#include "IsolatedUIFixture.h"

#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Scrollbar.h"

#include <gtest/gtest.h>
#include <string>

using GameEngine::ScrollView;
using GameEngine::Scrollbar;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <scrollview id="sv"/>
</uielement>)";

// The vertical bar is forced, so its gutter is reserved with no content to scroll.
constexpr char kGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 200px; height: 120px; }
#sv { display: flex; flex-grow: 1; --ui_scrollbar_force_vertical: 1; }
.scroll-row { display: flex; flex-direction: row; flex-grow: 1; min-width: 0; min-height: 0; }
.scroll-viewport { display: flex; flex-direction: column; flex-grow: 1; min-width: 0; min-height: 0; overflow: hidden; }
)";

// Builds the fixture and reports the scroll view's viewport width in logical px. Returns false
// when the fixture could not be built; Diagnostic() then says whether a device was missing.
bool ViewportWidth(IsolatedUIFixture& fx, const std::string& css, float& outWidth)
{
    if (!fx.Build(1.0f, kXml, css))
        return false;
    const auto* scroll = dynamic_cast<const ScrollView*>(fx.Element("sv"));
    if (!scroll)
        return false;
    outWidth = scroll->GetViewportWidth();
    return true;
}

#define REQUIRE_VIEWPORT_WIDTH(fx, css, out)                                                       \
    do                                                                                             \
    {                                                                                              \
        if (!ViewportWidth((fx), (css), (out)))                                                    \
        {                                                                                          \
            if (!(fx).DeviceAvailable())                                                           \
                GTEST_SKIP() << (fx).Diagnostic();                                                 \
            FAIL() << (fx).Diagnostic();                                                           \
        }                                                                                          \
    } while (false)

} // namespace

TEST(ScrollbarThicknessTokenTests, TheTokenSetsTheReservedGutter)
{
    IsolatedUIFixture fx;
    const std::string css = std::string(kGeometryCss) + "#root { --ui_scrollbar_thickness: 20px; }\n";
    float width = 0.0f;
    REQUIRE_VIEWPORT_WIDTH(fx, css, width);
    EXPECT_NEAR(width, 180.0f, 0.5f);
}

// The control: with no token the gutter is the default thickness, so the case above is not
// passing on a viewport that ignores the bar altogether.
TEST(ScrollbarThicknessTokenTests, WithoutTheTokenTheBarKeepsTheDefaultThickness)
{
    IsolatedUIFixture fx;
    float width = 0.0f;
    REQUIRE_VIEWPORT_WIDTH(fx, kGeometryCss, width);
    EXPECT_NEAR(width, 200.0f - Scrollbar::kDefaultThicknessPx, 0.5f);
}
