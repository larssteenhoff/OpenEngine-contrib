#include "Editor/Hierarchy/HierarchyLayerControls.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/RenderSorting.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Editor/Hierarchy/HierarchyRenderLayerEdit.h"
#include "Engine/Rendering/SortingLayers.h"
#include "Inspectors/RenderLayerField.h"
#include "Panels/HierarchyCommandIds.h"
#include "Platform/ContextMenu.h"
#include "Rendering/Sorting/RenderSortingEditor.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/MaskField.h"
#include "UI/EditorIcons.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <type_traits>

namespace GameEngine::Editor
{
namespace
{
constexpr uint32_t kVisibilityCommand = HierarchyCommandIds::kCmdShowRenderLayer;
constexpr uint32_t kSortingCommand = HierarchyCommandIds::kCmdShowSortingLayer;

// The row's picker of type `Picker`, created once per pooled row (a recycled row keeps it).
template <typename Picker>
Picker& RowPicker(UIElement& row, const char* className)
{
    for (const auto& child : row.GetChildren())
        if (child && child->HasClass(className))
            return *static_cast<Picker*>(child.get());
    auto picker = std::make_unique<Picker>();
    picker->AddClass("hierarchy-layer-picker");
    picker->AddClass(className);
    if constexpr (std::is_same_v<Picker, Dropdown>)
        picker->SetAutoWidthPopup(true);
    else
    {
        // The project's layer names, kept current by RefreshRenderLayerFields after a rename.
        FeedRenderLayerNames(*picker);
        picker->SetNothingTooltip(kRenderLayersNothingTooltip);
    }
    for (const auto& child : picker->GetChildren())
        if (child->HasClass("dropdown-header"))
            child->AddClass("hierarchy-layer-picker-header");
    auto* result = picker.get();
    row.AddChild(std::move(picker));
    return *result;
}

void SetClass(UIElement& element, const char* name, bool enabled)
{
    if (enabled)
        element.AddClass(name);
    else
        element.RemoveClass(name);
}
}

void HierarchyLayerControls::Present(ECS::World& world, ECS::EntityHandle entity,
    UIElement& row, UIElement& title, float panelWidth,
    const std::function<std::vector<ECS::EntityHandle>()>& targets,
    UndoRedoService* undo, EditorChangeNotifications* notifications)
{
    auto& visibility = RowPicker<MaskField>(row, "hierarchy-visibility-picker");
    auto& sorting = RowPicker<Dropdown>(row, "hierarchy-sorting-picker");
    const auto* mesh = world.GetComponent<Components::MeshRenderer>(entity);
    const bool showVisibility = m_ShowVisibility && mesh;
    const bool showSorting = m_ShowSorting && SupportsRenderSorting(world, entity);
    UI::Layout::SetElementHidden(visibility, !showVisibility);
    UI::Layout::SetElementHidden(sorting, !showSorting);
    const bool narrow = panelWidth < 400.0f;
    SetClass(visibility, "hierarchy-layer-picker-narrow", narrow);
    SetClass(sorting, "hierarchy-layer-picker-narrow", narrow);
    SetClass(sorting, "hierarchy-layer-picker-second", showVisibility && !narrow);
    SetClass(sorting, "hierarchy-layer-picker-second-narrow", showVisibility && narrow);
    SetClass(title, "hierarchy-title-one-layer", (showVisibility != showSorting) && !narrow);
    SetClass(title, "hierarchy-title-two-layers", showVisibility && showSorting && !narrow);
    SetClass(title, "hierarchy-title-one-layer-narrow", (showVisibility != showSorting) && narrow);
    SetClass(title, "hierarchy-title-two-layers-narrow", showVisibility && showSorting && narrow);
    SetClass(row, "hierarchy-row-with-layers", showVisibility || showSorting);
    if (showVisibility)
    {
        visibility.SetValueWithoutNotify(mesh->RenderLayers.Bits);
        // A row inside the selection applies the layer to the whole selection; each entity keeps
        // its other layers.
        visibility.SetOnBitsChanged([&world, targets, undo, notifications](uint32 bits, bool on)
        { ApplyRenderLayerBits(world, targets(), bits, on, undo, notifications); });
    }
    if (showSorting)
    {
        const auto* component = world.GetComponent<Components::RenderSorting>(entity);
        const int32 current = component ? component->Layer : 0;
        std::vector<Dropdown::Option> options;
        int selected = -1;
        auto layers = Rendering::SortingLayers::Current();
        std::sort(layers.Layers.begin(), layers.Layers.end(),
            [](const auto& left, const auto& right) { return left.Order < right.Order; });
        for (const auto& layer : layers.Layers)
        {
            if (layer.Id == current)
                selected = static_cast<int>(options.size());
            options.push_back({std::to_string(layer.Id), layer.Name});
        }
        if (selected < 0)
        {
            selected = static_cast<int>(options.size());
            options.push_back({std::to_string(current), "Missing layer (" + std::to_string(current) + ")"});
        }
        sorting.SetOnValueChanged({});
        sorting.SetOptions(options, selected);
        sorting.SetTooltip("Sorting Layer: draw order for alpha-blended artwork");
        sorting.SetOnValueChanged([&world, targets, undo, notifications](const std::string& text)
        {
            int32 layer = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), layer);
            if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
                ApplyRenderSortingLayer(world, targets(), layer, undo, notifications);
        });
    }
}

void HierarchyLayerControls::BuildMenu(ContextMenuBuilder& builder) const
{
    builder.AddItem("Hierarchy Options/Show Render Layer", kVisibilityCommand,
        m_ShowVisibility ? MenuItemFlag_Checked : MenuItemFlag_None, 70, EditorIcons::kEye);
    builder.AddItem("Hierarchy Options/Show Sorting Layer", kSortingCommand,
        m_ShowSorting ? MenuItemFlag_Checked : MenuItemFlag_None, 71, EditorIcons::kEye);
}

void HierarchyLayerControls::Hide(UIElement& row)
{
    row.RemoveClass("hierarchy-row-with-layers");
    for (const auto& child : row.GetChildren())
    {
        if (child->HasClass("hierarchy-layer-picker"))
            UI::Layout::SetElementHidden(*child, true);
        child->RemoveClass("hierarchy-title-one-layer");
        child->RemoveClass("hierarchy-title-two-layers");
        child->RemoveClass("hierarchy-title-one-layer-narrow");
        child->RemoveClass("hierarchy-title-two-layers-narrow");
    }
}

bool HierarchyLayerControls::NeedsRefresh(ECS::World& world)
{
    const auto revision = Rendering::SortingLayers::Revision();
    const auto reset = world.GetLifecycleResetGeneration();
    bool changed = m_LayersRevision != revision || m_World != &world || m_ResetGeneration != reset;
    if (m_World != &world || m_ResetGeneration != reset)
        m_Changes.LastRunVersion = 0;
    m_World = &world;
    m_ResetGeneration = reset;
    m_LayersRevision = revision;
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    if (m_ShowSorting && !changed)
    {
        auto query = world.Query<ECS::Read<Components::RenderSorting>>();
        query.IncludeDisabled();
        query.Changed<Components::RenderSorting>(m_Changes);
        query.BatchEach([&changed](const Components::RenderSorting*, std::size_t) { changed = true; });
    }
    m_Changes.LastRunVersion = entryVersion;
    return changed;
}

bool HierarchyLayerControls::HandleCommand(uint32_t command)
{
    if (command == kVisibilityCommand)
        m_ShowVisibility = !m_ShowVisibility;
    else if (command == kSortingCommand)
        m_ShowSorting = !m_ShowSorting;
    else
        return false;
    return true;
}
}
