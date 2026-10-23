#include "PromptFileDropZone.h"
#include "PromptFileTray.h"

#include "Assets/AssetManager.h"
#include "Editor/DragDropPayloads.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Platform/Shell.h"
#include "UI/Controls/Label.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"
#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <chrono>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{
class PromptFileTrayTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() /
                 ("prompt-file-tray-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(m_Root);
        std::filesystem::create_directories(m_Root);
    }
    void TearDown() override { std::filesystem::remove_all(m_Root); }

    std::filesystem::path Write(const std::string& name, const std::string& bytes) const
    {
        const std::filesystem::path path = m_Root / name;
        std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return path;
    }

    std::filesystem::path m_Root;
};

// A tray in a live UI that reads the package's item layout from the staged fixtures, its
// reads on a pool of its own; Settle drains the posted reads.
struct TrayHost
{
    std::unique_ptr<Rendering::IDevice> Device = MakeHeadlessDevice();
    JobSystem::WorkStealingThreadPool Pool{2};
    AssetManager Assets;
    std::unique_ptr<UIManager> Ui;
    UIElement* Strip = nullptr;
    UIElement* Items = nullptr;
    std::string Status;
    std::unique_ptr<PromptFileTray> Tray;

    TrayHost()
    {
        if (!Device)
            return;
        UIRegistration::RegisterBuiltInControls();
        const std::filesystem::path assetRoot = std::filesystem::temp_directory_path() / "PromptFileTrayTests-assets";
        std::filesystem::create_directories(assetRoot);
        AssetSourceDesc package;
        package.Alias = "ai-assistant";
        package.Root = Platform::GetExecutablePath().parent_path() / "AiAssistantPanelFixtures" / "ai-assistant";
        if (!Assets.Initialize(assetRoot, &Pool) || !Assets.RegisterSource(package))
            return;
        Ui = std::make_unique<UIManager>(Device.get(), &Assets);
        Ui->SetRoot(std::make_unique<UIElement>());
        auto strip = std::make_unique<UIElement>();
        Strip = strip.get();
        auto items = std::make_unique<UIElement>();
        Items = items.get();
        Strip->AddChild(std::move(items));
        Ui->GetRootElement()->AddChild(std::move(strip));
        Tray = std::make_unique<PromptFileTray>(*Strip, *Items, Pool, [this](const std::string& text) { Status = text; });
    }

    bool Ready() const { return Tray != nullptr; }

    // Runs UI frames, 5 ms apart, until no file is loading, for up to 2 s; false when one still is.
    bool Settle()
    {
        for (int frame = 0; frame < 400 && Tray->IsLoading(); ++frame)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            Ui->Update(0.0f, /*interactive=*/false);
        }
        return !Tray->IsLoading();
    }
};

// The file name each item of the strip shows.
std::vector<std::string> ItemNames(const UIElement& items)
{
    std::vector<std::string> names;
    for (const auto& item : items.GetChildren())
        for (const auto& part : item->GetChildren())
            if (const auto* label = dynamic_cast<const Label*>(part.get()))
                names.push_back(label->GetText());
    return names;
}
} // namespace

TEST_F(PromptFileTrayTests, AFileShowsAsLoadingUntilItsReadLands)
{
    TrayHost host;
    if (!host.Ready())
        GTEST_SKIP() << "No device";
    const std::filesystem::path notes = Write("notes.md", "# Notes\n");
    const std::filesystem::path paths[] = {notes, notes, Write("empty.txt", "")};
    host.Tray->Add(paths);

    // Nothing lands before the UI drains the posted reads.
    EXPECT_TRUE(host.Tray->IsLoading());
    EXPECT_TRUE(host.Tray->Files().empty());
    EXPECT_EQ(ItemNames(*host.Items), (std::vector<std::string>{"notes.md", "empty.txt"}));
    EXPECT_TRUE(host.Items->GetChildren().front()->HasClass("prompt-panel-file-item-loading"));
    EXPECT_FALSE(host.Strip->HasClass("hidden"));

    ASSERT_TRUE(host.Settle());
    ASSERT_EQ(host.Tray->Files().size(), 1u);
    EXPECT_EQ(host.Tray->Files()[0].Name, "notes.md");
    EXPECT_EQ(ItemNames(*host.Items), std::vector<std::string>{"notes.md"});
    EXPECT_FALSE(host.Items->GetChildren().front()->HasClass("prompt-panel-file-item-loading"));
    EXPECT_EQ(host.Status, "empty.txt is empty");

    host.Tray->Clear();
    EXPECT_TRUE(host.Tray->Files().empty());
    EXPECT_TRUE(host.Items->GetChildren().empty());
    EXPECT_TRUE(host.Strip->HasClass("hidden"));
}

TEST_F(PromptFileTrayTests, FilesPastTheCapAreNotAdded)
{
    TrayHost host;
    if (!host.Ready())
        GTEST_SKIP() << "No device";
    std::vector<std::filesystem::path> paths;
    for (size_t i = 0; i <= PromptFiles::kMaxFiles; ++i)
        paths.push_back(Write("f" + std::to_string(i) + ".txt", "text"));
    host.Tray->Add(paths);
    EXPECT_EQ(host.Status, "") << "the refusal waits for the call's reads";

    ASSERT_TRUE(host.Settle());
    EXPECT_EQ(host.Tray->Files().size(), PromptFiles::kMaxFiles);
    EXPECT_EQ(host.Status, "f20.txt was not added: a prompt carries at most 20 files");
}

TEST_F(PromptFileTrayTests, TheRefusalsOfOneAddAreJoinedInOneStatus)
{
    TrayHost host;
    if (!host.Ready())
        GTEST_SKIP() << "No device";
    const std::filesystem::path paths[] = {Write("a.txt", ""), Write("notes.md", "# Notes\n"),
                                           Write("b.bin", std::string("\x7F" "ELF\0\0\x01", 7))};
    host.Tray->Add(paths);
    ASSERT_TRUE(host.Settle());
    EXPECT_EQ(host.Status, "a.txt is empty; b.bin is not text or an image (PNG, JPEG, GIF or WebP), so it cannot be sent");
    EXPECT_EQ(ItemNames(*host.Items), std::vector<std::string>{"notes.md"});
}

TEST_F(PromptFileTrayTests, TheInputPaneTakesDroppedFilesButNotFolders)
{
    PromptFileDropZone zone;
    std::vector<std::filesystem::path> dropped;
    zone.SetOnFilesDropped([&dropped](std::span<const std::filesystem::path> paths)
                           { dropped.assign(paths.begin(), paths.end()); });

    const std::filesystem::path folder = m_Root / "folder";
    std::filesystem::create_directories(folder);
    const std::filesystem::path notes = Write("notes.md", "# Notes\n");

    UI::Interaction::DropRequest request;
    request.payload = UI::Interaction::DragPayload::Create(Editor::AssetPathsDragPayload{{folder}, "folder", true});
    EXPECT_FALSE(zone.CanDrop(request).Allowed);

    request.payload =
        UI::Interaction::DragPayload::Create(Editor::AssetPathsDragPayload{{folder, notes}, "folder + 1", true});
    EXPECT_TRUE(zone.CanDrop(request).Allowed);
    zone.PerformDrop(request);
    EXPECT_EQ(dropped, std::vector<std::filesystem::path>{notes});
}

TEST_F(PromptFileTrayTests, TheAttachButtonShowsWhetherADragCanDrop)
{
    PromptFileDropZone zone;
    UIElement attach;
    zone.SetFeedbackElement(&attach);

    UI::Interaction::DropPreviewState state;
    state.Visible = true;
    state.Allowed = true;
    zone.SetDropPreview(state);
    EXPECT_TRUE(attach.HasClass("prompt-panel-drop-allowed-icon"));
    EXPECT_FALSE(attach.HasClass("prompt-panel-drop-invalid-icon"));
    EXPECT_TRUE(zone.HasClass("prompt-panel-drop-active"));

    state.Allowed = false;
    zone.SetDropPreview(state);
    EXPECT_FALSE(attach.HasClass("prompt-panel-drop-allowed-icon"));
    EXPECT_TRUE(attach.HasClass("prompt-panel-drop-invalid-icon"));
    EXPECT_FALSE(zone.HasClass("prompt-panel-drop-active"));

    zone.SetDropPreview({});
    EXPECT_FALSE(attach.HasClass("prompt-panel-drop-invalid-icon"));
}
