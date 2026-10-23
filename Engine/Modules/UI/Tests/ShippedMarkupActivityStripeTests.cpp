// The Mark-ups panel's Activity stripes, as the editor ships them: every second row sits on the
// theme's secondary list stripe, an opaque plate the first row does not draw. Read from the staged
// sheets.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIStyle.h"

#include "Core/Application.h"

#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using GameEngine::PathUtils;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

std::string ReadShippedCss(const std::string& relative)
{
    std::ifstream in(PathUtils::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="list" class="markups-list">
    <uielement id="row" class="markups-row markups-activity-row"/>
    <uielement id="alt" class="markups-row markups-activity-row markups-activity-row-alt"/>
  </uielement>
</uielement>)";

constexpr char kGeometryCss[] = R"(
#root { display: flex; flex-direction: column; width: 200px; height: 200px; }
)";

} // namespace

TEST(ShippedMarkupActivityStripeTests, TheSecondRowSitsOnTheSecondaryStripe)
{
    std::string css;
    for (const char* sheet : {"theme/tokens.css", "panels/MarkupsPanel.css"})
    {
        const std::string text = ReadShippedCss(sheet);
        ASSERT_FALSE(text.empty()) << "shipped stylesheet not staged next to the test exe: " << sheet;
        css += text;
        css += '\n';
    }
    css += kGeometryCss;

    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, kXml, css))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }
    for (const char* id : {"row", "alt"})
        ASSERT_NE(fx.Style(id), nullptr) << id;

    const uint32_t row = fx.Style("row")->Visual.BackgroundColor;
    const uint32_t alt = fx.Style("alt")->Visual.BackgroundColor;

    EXPECT_EQ(alt >> 24, 0xFFu) << "the second row is on an opaque plate";
    EXPECT_NE(alt, row) << "the second row differs from the first";
}
