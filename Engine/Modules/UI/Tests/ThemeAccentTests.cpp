// The default accent is written twice by necessity: tokens.css defines it for
// every themed window, and kDefaultAccentArgb is the fallback for hosts no
// theme reaches (ResolveAccentArgb, the text selection fill). This reads the
// tokens.css the build stages next to the test exe, so the two cannot drift
// apart again unnoticed.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/ThemeAccent.h"

#include "Core/Application.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

using GameEngine::PathUtils;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

std::string ReadShippedTokens()
{
    const std::filesystem::path path =
        PathUtils::GetExecutableDirectory() / "Assets" / "UI" / "theme" / "tokens.css";
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST(ThemeAccentTests, ShippedTokensDefaultMatchesTheCodeFallback)
{
    EXPECT_EQ(GameEngine::UI::kDefaultAccentArgb, 0xFF3A8FFFu)
        << "The default accent must preserve the existing editor palette";
    const std::string tokens = ReadShippedTokens();
    ASSERT_FALSE(tokens.empty()) << "theme/tokens.css is not staged next to the test exe";

    IsolatedUIFixture fx;
    if (!fx.Build(1.0f, R"(<uielement id="root"/>)", tokens + "\n#root { width: 10px; height: 10px; }\n"))
    {
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << fx.Diagnostic();
        FAIL() << fx.Diagnostic();
    }

    const GameEngine::ResolvedStyle* style = fx.Style("root");
    ASSERT_NE(style, nullptr);
    const std::optional<uint32_t> accent = style->GetCustomColor(GameEngine::UI::kAccentColorVariable);
    ASSERT_TRUE(accent.has_value()) << "tokens.css must define --ui_color_accent on :root";
    EXPECT_EQ(*accent, GameEngine::UI::kDefaultAccentArgb)
        << "tokens.css and UI::kDefaultAccentArgb must name the same default accent";
    EXPECT_EQ(style->GetCustomColor(GameEngine::HashStringId("--ui_color_accent_blue_hover")),
              std::optional<uint32_t>(0xFF75B0FFu));
    EXPECT_EQ(style->GetCustomColor(GameEngine::HashStringId("--ui_color_accent_blue_pressed")),
              std::optional<uint32_t>(0xFF2B6BBFu));
}
