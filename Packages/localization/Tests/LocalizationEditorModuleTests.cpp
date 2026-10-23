// The localization package's editor module: the Localization panel is registered with an icon
// class, the sheet the module contributes to the editor's cascade gives that class an image the
// package ships, and the panel has one entry point, the panel menus (Window and the toolbar's
// panel list), like every other panel.

#include "LocalizationPanel.h"

#include "Core/Application.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "UI/ClassBackgroundImageQuery.h"
#include "UI/Parsers/CSSParser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>

using GameEngine::Editor::EditorMenuRegistry;
using GameEngine::Editor::EditorPanelDescriptor;
using GameEngine::Editor::EditorPanelRegistry;
using GameEngine::Editor::LocalizationPanel;

namespace
{

// The package's assets, staged beside the suite under the package's mount alias.
std::filesystem::path StagedPackageAssets()
{
    return GameEngine::PathUtils::GetExecutableDirectory() / "LocalizationEditorFixtures" /
           LocalizationPanel::kAssetSourceAlias;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

TEST(LocalizationEditorModule, RegistersThePanelWithItsIconClass)
{
    EditorPanelDescriptor descriptor;
    ASSERT_TRUE(EditorPanelRegistry::Get().TryGetPanel(LocalizationPanel::kPanelId, descriptor));
    EXPECT_EQ(descriptor.Title, "Localization");
    EXPECT_EQ(descriptor.TabIconClass, LocalizationPanel::kTabIconClass);
    EXPECT_EQ(descriptor.DefaultDockAnchorPanelId, "SceneView");
}

TEST(LocalizationEditorModule, ContributesTheSheetThatDeclaresTheIconClass)
{
    const auto sheets = EditorPanelRegistry::Get().StyleSheetSnapshot();
    const bool contributed = std::any_of(sheets.begin(), sheets.end(), [](const auto& sheet) {
        return sheet.AssetSourceAlias == LocalizationPanel::kAssetSourceAlias &&
               sheet.StyleAssetPath == LocalizationPanel::kChromeStyleAssetPath;
    });
    EXPECT_TRUE(contributed);
}

// The image the panel menus read for the panel's row: the class's background-image in the
// contributed sheet, naming a file the package ships under its own mount.
TEST(LocalizationEditorModule, TheIconClassNamesAnIconThePackageShips)
{
    const std::string css = ReadText(StagedPackageAssets() / LocalizationPanel::kChromeStyleAssetPath);
    ASSERT_FALSE(css.empty()) << "the chrome sheet is not staged beside the suite";
    auto sheet = std::make_shared<GameEngine::Stylesheet>();
    ASSERT_TRUE(GameEngine::UIParsing::CSSParser::ParseStylesFromString(css, *sheet));
    const GameEngine::StylesheetHandle sheets[] = {sheet};

    const std::string url = GameEngine::UIStyleQuery::ResolveClassBackgroundImageUrl(sheets, LocalizationPanel::kTabIconClass);
    const std::string alias = std::string(LocalizationPanel::kAssetSourceAlias) + ":";
    ASSERT_EQ(url.rfind(alias, 0), 0u) << "the icon is not under the package's mount: " << url;
    EXPECT_TRUE(std::filesystem::is_regular_file(StagedPackageAssets() / url.substr(alias.size()))) << url;
}

// One entry point: the panel opens from the panel menus, so the module adds no menu item.
TEST(LocalizationEditorModule, AddsNoMenuItemOfItsOwn)
{
    for (const auto& item : EditorMenuRegistry::Get().Snapshot())
        EXPECT_EQ(item.Path.find("Localization"), std::string::npos) << item.Path;
}

// Every image a package sheet names exists: url("localization:...") under the package's staged
// assets, url("editor:...") under the editor's assets. A missing file draws an empty box.
TEST(LocalizationEditorModule, EverySheetUrlNamesAnExistingFile)
{
    static const std::regex pattern(R"RX(url\("([a-z-]+):([^"]+)"\))RX");
    int checked = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(StagedPackageAssets()))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".css")
            continue;
        const std::string css = ReadText(entry.path());
        for (auto it = std::sregex_iterator(css.begin(), css.end(), pattern); it != std::sregex_iterator(); ++it)
        {
            const std::string alias = (*it)[1].str();
            const std::string relative = (*it)[2].str();
            std::filesystem::path file;
            if (alias == LocalizationPanel::kAssetSourceAlias)
                file = StagedPackageAssets() / relative;
            else if (alias == "editor")
                file = std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets" / relative;
            else
                ADD_FAILURE() << entry.path().filename() << " names an unknown mount: " << (*it)[0].str();
            if (!file.empty())
                EXPECT_TRUE(std::filesystem::is_regular_file(file))
                    << entry.path().filename() << " names " << (*it)[0].str() << ", which does not exist";
            ++checked;
        }
    }
    EXPECT_GT(checked, 0) << "no package sheet is staged beside the suite";
}
