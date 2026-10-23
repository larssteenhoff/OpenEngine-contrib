#include <gtest/gtest.h>

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/RenderSorting.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Transform.h"
#include "Components/Hierarchy.h"
#include "InspectorRegistry.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Editor/Hierarchy/HierarchyLayerControls.h"
#include "Engine/Rendering/SortingLayers.h"
#include "Rendering/Sorting/RenderSortingEditor.h"
#include "Rendering/Sorting/SortingLayerSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "TestTempDir.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/MaskField.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/Internal/MaskFieldAccess.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

namespace GameEngine::Editor
{
namespace
{
using Sorting = Components::RenderSorting;

void SendMouse(UIElement& element, EventId id)
{
    UIEvent event{};
    event.Id = id;
    event.X = 5.0f;
    event.Y = 5.0f;
    event.Button = 0;
    event.Target = &element;
    event.CurrentTarget = &element;
    element.DispatchEvent(event);
}

void Click(UIElement& element)
{
    UILayoutAccess::SetLastLayoutRect(element, 0.0f, 0.0f, 100.0f, 20.0f);
    SendMouse(element, kEventMouseDown);
    SendMouse(element, kEventMouseUp);
}

class RenderSortingEditorTests : public ::testing::Test
{
protected:
    ECS::World World;
    UndoRedoService Undo;
    std::filesystem::path ProjectRoot;

    void SetUp() override
    {
        ProjectRoot = TestUtils::MakeUniqueTempDirectory("RenderSortingEditorTests");
        std::filesystem::create_directories(ProjectRoot);
        RegisterRenderSortingComponent(ECS::GetComponentTypeId<Components::MeshRenderer>());
        RegisterRenderSortingComponent(ECS::GetComponentTypeId<Components::SkinnedMeshRenderer>());
        std::string error;
        ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(Rendering::SortingLayers::Defaults(), error));
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(ProjectRoot, error);
    }

    ECS::EntityHandle Mesh()
    {
        const auto entity = World.CreateEntity();
        World.AddComponentImmediate(entity, Components::Transform{});
        World.AddComponentImmediate(entity, Components::MeshRenderer{});
        return entity;
    }
};

TEST_F(RenderSortingEditorTests, MultiSelectionUndoRestoresMissingComponentAndExistingOrder)
{
    const auto first = Mesh();
    const auto second = Mesh();
    Sorting initial{};
    initial.Layer = 10;
    initial.OrderInLayer = 17;
    initial.GroupChildren = true;
    World.AddComponentImmediate(second, initial);
    ApplyRenderSortingLayer(World, {first, second}, 30, &Undo, nullptr);
    ASSERT_TRUE(World.HasComponent<Sorting>(first));
    ASSERT_TRUE(World.HasComponent<Sorting>(second));
    EXPECT_EQ(World.GetComponent<Sorting>(first)->Layer, 30);
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 30);
    EXPECT_EQ(World.GetComponent<Sorting>(second)->OrderInLayer, 17);
    Undo.Undo();
    EXPECT_FALSE(World.HasComponent<Sorting>(first));
    ASSERT_TRUE(World.HasComponent<Sorting>(second));
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 10);
    EXPECT_TRUE(World.GetComponent<Sorting>(second)->GroupChildren);
    EXPECT_FALSE(Undo.CanUndo());
    Undo.Redo();
    EXPECT_EQ(World.GetComponent<Sorting>(first)->Layer, 30);
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 30);
}

TEST_F(RenderSortingEditorTests, DefaultAssignmentAndUnsupportedEntitiesDoNotCreateUndo)
{
    const auto mesh = Mesh();
    const auto empty = World.CreateEntity();
    World.AddComponentImmediate(empty, Components::Transform{});
    ApplyRenderSortingLayer(World, {mesh, empty}, 0, &Undo, nullptr);
    EXPECT_FALSE(World.HasComponent<Sorting>(mesh));
    EXPECT_FALSE(World.HasComponent<Sorting>(empty));
    EXPECT_FALSE(Undo.CanUndo());
    ApplyRenderSortingLayer(World, {empty}, 10, &Undo, nullptr);
    EXPECT_FALSE(Undo.CanUndo());
}

TEST_F(RenderSortingEditorTests, SkinnedMeshAndSortingOnlyParentAreEligible)
{
    const auto skinned = World.CreateEntity();
    World.AddComponentImmediate(skinned, Components::SkinnedMeshRenderer{});
    const auto parent = World.CreateEntity();
    Sorting group{};
    group.GroupChildren = true;
    World.AddComponentImmediate(parent, group);
    ApplyRenderSortingLayer(World, {skinned, parent}, 20, &Undo, nullptr);
    ASSERT_TRUE(World.HasComponent<Sorting>(skinned));
    ASSERT_TRUE(World.HasComponent<Sorting>(parent));
    EXPECT_EQ(World.GetComponent<Sorting>(skinned)->Layer, 20);
    EXPECT_EQ(World.GetComponent<Sorting>(parent)->Layer, 20);
    EXPECT_TRUE(World.GetComponent<Sorting>(parent)->GroupChildren);
}

TEST_F(RenderSortingEditorTests, PooledPickerUsesCurrentEntityAndCustomLayerIdentity)
{
    auto layers = Rendering::SortingLayers::Defaults();
    layers.Layers.push_back({1234, "Meadow Canopy", 99});
    std::string error;
    ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(layers, error));
    const auto first = Mesh();
    const auto second = Mesh();
    UIElement row;
    Label title;
    HierarchyLayerControls controls;
    ASSERT_TRUE(controls.HandleCommand(0x4177));
    controls.Present(World, first, row, title, 350.0f, [first]() { return std::vector{first}; }, &Undo, nullptr);
    controls.Present(World, second, row, title, 350.0f, [second]() { return std::vector{second}; }, &Undo, nullptr);
    Dropdown* picker = nullptr;
    for (const auto& child : row.GetChildren())
        if (child->HasClass("hierarchy-sorting-picker"))
            picker = static_cast<Dropdown*>(child.get());
    ASSERT_NE(picker, nullptr);
    EXPECT_TRUE(picker->HasClass("hierarchy-layer-picker-narrow"));
    EXPECT_TRUE(title.HasClass("hierarchy-title-one-layer-narrow"));
    picker->SetSelectedIndex(static_cast<int>(layers.Layers.size()) - 1);
    EXPECT_FALSE(World.HasComponent<Sorting>(first));
    ASSERT_TRUE(World.HasComponent<Sorting>(second));
    EXPECT_EQ(World.GetComponent<Sorting>(second)->Layer, 1234);
    EXPECT_EQ(picker->GetSelectedValue(), "1234");
    EXPECT_TRUE(controls.NeedsRefresh(World));
    EXPECT_FALSE(controls.NeedsRefresh(World));
    auto edited = *World.GetComponent<Sorting>(second);
    edited.Layer = 10;
    World.AddComponentImmediate(second, edited);
    EXPECT_TRUE(controls.NeedsRefresh(World));
    ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(Rendering::SortingLayers::Defaults(), error));
    EXPECT_TRUE(controls.NeedsRefresh(World));
}
TEST_F(RenderSortingEditorTests, SavesCustomLayersWithoutChangingOtherProjectSettings)
{
    auto store = OpenProjectSettings(ProjectRoot);
    std::string error;
    ASSERT_TRUE(store.Load(&error));
    store.SetString("unrelated", "preserved");
    ASSERT_TRUE(store.Save(&error));
    auto layers = Rendering::SortingLayers::Defaults();
    layers.Layers.push_back({765, "Canopy", 97});
    ASSERT_TRUE(SaveSortingLayerSettings(ProjectRoot, layers, error)) << error;
    EXPECT_EQ(Rendering::SortingLayers::Load(ProjectRoot).Layers, layers.Layers);
    EXPECT_EQ(Rendering::SortingLayers::Current().Layers, layers.Layers);
    ASSERT_TRUE(store.Load(&error));
    std::string unrelated;
    ASSERT_TRUE(store.TryGetString("unrelated", unrelated));
    EXPECT_EQ(unrelated, "preserved");
    layers.Layers.back().Name = "Default";
    EXPECT_FALSE(SaveSortingLayerSettings(ProjectRoot, layers, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(Rendering::SortingLayers::Load(ProjectRoot).Layers.back().Name, "Canopy");
    EXPECT_EQ(Rendering::SortingLayers::Current().Layers.back().Name, "Canopy");
}

TEST_F(RenderSortingEditorTests, LoadingAnotherProjectClearsOutgoingCustomLayers)
{
    auto layers = Rendering::SortingLayers::Defaults();
    layers.Layers.push_back({765, "Canopy", 97});
    std::string error;
    ASSERT_TRUE(SaveSortingLayerSettings(ProjectRoot, layers, error));
    const auto other = ProjectRoot / "OtherProject";
    std::filesystem::create_directories(other);
    auto store = OpenProjectSettings(other);
    ASSERT_TRUE(store.Load(&error));
    ASSERT_TRUE(Rendering::SortingLayers::SetCurrent(Rendering::SortingLayers::FromJson(store.Json()), error));
    EXPECT_EQ(Rendering::SortingLayers::Current().Layers, Rendering::SortingLayers::Defaults().Layers);
}

TEST_F(RenderSortingEditorTests, VisibilityPickerRemainsIndependentAndUndoesTheSelection)
{
    const auto first = Mesh();
    const auto second = Mesh();
    const auto original = World.GetComponent<Components::MeshRenderer>(first)->RenderLayers;
    UIElement row;
    Label title;
    HierarchyLayerControls controls;
    ASSERT_TRUE(controls.HandleCommand(0x4175));
    controls.Present(World, first, row, title, 350.0f,
        [first, second]() { return std::vector{first, second}; }, &Undo, nullptr);
    // The render-layer picker is a MaskField: a row click turns that one layer on or off for every
    // entity of the selection and leaves their other layers as they are.
    MaskField* picker = nullptr;
    for (const auto& child : row.GetChildren())
        if (child->HasClass("hierarchy-visibility-picker"))
            picker = dynamic_cast<MaskField*>(child.get());
    ASSERT_NE(picker, nullptr);
    constexpr uint32 kLayer3 = 8u;
    ASSERT_EQ(original.Bits & kLayer3, 0u);
    MaskFieldAccess::OpenPopup(*picker);
    ASSERT_GT(MaskFieldAccess::OptionRows(*picker).size(), 3u);
    Click(*MaskFieldAccess::OptionRows(*picker)[3]);
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(first)->RenderLayers.Bits, original.Bits | kLayer3);
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(second)->RenderLayers.Bits, original.Bits | kLayer3);
    EXPECT_FALSE(World.HasComponent<Sorting>(first));
    EXPECT_FALSE(World.HasComponent<Sorting>(second));
    Undo.Undo();
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(first)->RenderLayers, original);
    EXPECT_EQ(World.GetComponent<Components::MeshRenderer>(second)->RenderLayers, original);
    EXPECT_FALSE(Undo.CanUndo());
}

TEST_F(RenderSortingEditorTests, RemovingLayerReservesItsIdForOldSceneReferences)
{
    auto layers = Rendering::SortingLayers::Defaults();
    layers.Layers.push_back({765, "Canopy", 97});
    std::string error;
    ASSERT_TRUE(SaveSortingLayerSettings(ProjectRoot, layers, error));
    layers.Layers.pop_back();
    ASSERT_TRUE(SaveSortingLayerSettings(ProjectRoot, layers, error));
    auto store = OpenProjectSettings(ProjectRoot);
    ASSERT_TRUE(store.Load(&error));
    int64_t nextId = 0;
    ASSERT_TRUE(store.TryGetInt64("sortingLayerNextId", nextId));
    EXPECT_EQ(nextId, 766);
}

TEST_F(RenderSortingEditorTests, DiagnosticReportsEffectiveParentOrderAndRefreshes)
{
    const auto parent = Mesh();
    const auto child = Mesh();
    Sorting group{};
    group.Layer = 30;
    group.OrderInLayer = 7;
    group.GroupChildren = true;
    World.AddComponentImmediate(parent, group);
    World.AddComponentImmediate(child, Components::Parent{parent});
    UIElement content;
    std::vector<std::function<void()>> refresh;
    InspectorContext context;
    context.World = &World;
    context.Entity = child;
    context.Parent = &content;
    context.SimulationRefreshCallbacks = &refresh;
    AddRenderSortingDiagnostic(context);
    ASSERT_EQ(content.GetChildren().size(), 1u);
    const auto* text = dynamic_cast<Label*>(content.GetChildren().front().get());
    ASSERT_NE(text, nullptr);
    EXPECT_NE(text->GetText().find("Foreground / 7 / Group:"), std::string::npos);
    group.Layer = -30;
    group.OrderInLayer = -2;
    World.AddComponentImmediate(parent, group);
    ASSERT_EQ(refresh.size(), 1u);
    refresh.front()();
    EXPECT_NE(text->GetText().find("Background / -2 / Group:"), std::string::npos);
}

}
}
