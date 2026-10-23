#include "Panels/BuiltInPanelTypes.h"

#include "Editor/Registries/EditorPanelRegistry.h"
#include "EditorPanelIds.h"
#include "Graph/GraphModel.h"
#include "Panels/AnimationWindowPanel.h"
#include "Panels/AssetViewPanel.h"
#include "Panels/AssetsPanel.h"
#include "Panels/BookmarksPanel.h"
#include "Panels/BuildPanel.h"
#include "Panels/CpuProfilerPanel.h"
#include "Panels/DiffPanel.h"
#include "Panels/GameViewPanel.h"
#include "Panels/GraphPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/LogPanel.h"
#include "Panels/MissingAssetsPanel.h"
#include "Panels/MixerPanel.h"
#include "Panels/MonitorsPanel.h"
#include "Panels/PackageManagerPanel.h"
#include "Panels/RenderGraphPanel.h"
#include "Panels/SceneViewPanel.h"
#include "Panels/ScriptEditorPanel.h"
#include "Panels/ScriptErrorsPanel.h"
#include "Panels/SettingsPanel.h"
#include "Panels/ShaderErrorsPanel.h"
#include "Panels/TilePalettePanel.h"
#include "Panels/TileSetEditorPanel.h"
#include "Panels/TodoPanel.h"
#include "Panels/UIDemoPanel.h"
#include "Panels/UndoHistoryPanel.h"
#include "Panels/VisualProfilerPanel.h"
#include "Panels/VramPanel.h"
#include "Panels/WebPanel.h"

#include <memory>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

using PanelFactory = std::unique_ptr<UIElement> (*)();
using PanelUpdate = void (*)(UIElement& panel, const PanelUpdateContext& context);
using PanelShown = void (*)(UIElement& panel);

// One row of the table: the EditorPanelTypeDescriptor fields a built-in type sets.
struct BuiltInPanelType
{
    const char* Key;
    PanelFactory Factory;
    PanelUpdate Update = nullptr;
    bool UpdateWhenHidden = false;
    const char* Anchor = nullptr;
    float SplitBelowAnchor = 0.0f;
    PanelShown OnShown = nullptr;
};

// Build and Mixer open in their own split below the Hierarchy; the Hierarchy keeps
// this fraction of the height. The Mixer's split opens about 30px taller.
constexpr float kBuildSplitBelowHierarchy = 0.70f;
constexpr float kMixerSplitBelowHierarchy = 0.7125f;

template <typename T>
std::unique_ptr<UIElement> MakePanel()
{
    return std::make_unique<T>();
}

template <typename T>
void UpdatePanel(UIElement& panel, const PanelUpdateContext& /*context*/)
{
    static_cast<T&>(panel).Update();
}

void UpdateRenderGraphPanel(UIElement& panel, const PanelUpdateContext& context)
{
    static_cast<RenderGraphPanel&>(panel).Update(context.RenderDoc, context.RenderGraphFrame);
}

void UpdateVisualProfilerPanel(UIElement& panel, const PanelUpdateContext& context)
{
    static_cast<VisualProfilerPanel&>(panel).Update(context.RenderGraphFrame);
}

void UpdateVramPanel(UIElement& panel, const PanelUpdateContext& context)
{
    static_cast<VramPanel&>(panel).Update(context.Device, context.RenderGraphFrame);
}

// Build versions live in the project settings, which a project switch replaces.
void RefreshBuildVersions(UIElement& panel)
{
    static_cast<BuildPanel&>(panel).RefreshVersionsFromSettings();
}

std::unique_ptr<UIElement> MakeMaterialGraphPanel()
{
    return GraphPanel::CreateForKind(GameEngine::Graph::kKindIdMaterial);
}

std::unique_ptr<UIElement> MakeAnimationGraphPanel()
{
    return GraphPanel::CreateForKind(GameEngine::Graph::kKindIdAnimation);
}

std::unique_ptr<UIElement> MakeGameLogicGraphPanel()
{
    return GraphPanel::CreateForKind(GameEngine::Graph::kKindIdGameLogic);
}

// Every panel type a dock-config row may name. Tests/Editor/PanelDefaultTabIconTests
// reads the `.Key` and `.Factory` of these rows to check each type's tab icon.
// Graph panel types set no Update: EditorApplication::BindGraphPanel registers it for
// every graph panel, including the per-asset ones no panel type builds.
// Anchors: the left column (Hierarchy), the right column (Inspector), the centre
// workspace (SceneView); no anchor is the bottom workspace stack (Assets).
constexpr BuiltInPanelType kBuiltInPanelTypes[] = {
    {.Key = "AssetsPanel", .Factory = &MakePanel<AssetsPanel>, .Anchor = EditorPanelIds::ScriptErrors},
    {.Key = "AssetViewPanel", .Factory = &MakePanel<AssetViewPanel>, .Anchor = EditorPanelIds::AssetView},
    {.Key = "TodoPanel", .Factory = &MakePanel<TodoPanel>, .Anchor = EditorPanelIds::Hierarchy},
    {.Key = "BookmarksPanel", .Factory = &MakePanel<BookmarksPanel>, .Anchor = EditorPanelIds::Hierarchy},
    // A version poll: rebuilds the tree when the world's entity set changes through
    // a path that sends no EditorChangeNotifications (IPC bulk spawn, scripts,
    // gameplay). A hidden hierarchy catches up the frame it is shown.
    {.Key = "HierarchyPanel",
     .Factory = &MakePanel<HierarchyPanel>,
     .Update = &UpdatePanel<HierarchyPanel>,
     .Anchor = EditorPanelIds::Hierarchy},
    {.Key = "InspectorPanel", .Factory = &MakePanel<InspectorPanel>, .Anchor = EditorPanelIds::Inspector},
    {.Key = "InspectorPanel2", .Factory = &MakePanel<InspectorPanel>, .Anchor = EditorPanelIds::Inspector},
    {.Key = "GameViewPanel", .Factory = &MakePanel<GameViewPanel>, .Anchor = EditorPanelIds::SceneView},
    {.Key = "SceneViewPanel", .Factory = &MakePanel<SceneViewPanel>, .Anchor = EditorPanelIds::SceneView},
    {.Key = "UIDemoPanel", .Factory = &MakePanel<UIDemoPanel>, .Anchor = EditorPanelIds::SceneView},
    {.Key = "MissingAssetsPanel", .Factory = &MakePanel<MissingAssetsPanel>},
    {.Key = "PackageManagerPanel", .Factory = &MakePanel<PackageManagerPanel>},
    {.Key = "LogPanel",
     .Factory = &MakePanel<LogPanel>,
     .Update = &UpdatePanel<LogPanel>,
     .Anchor = EditorPanelIds::Inspector},
    {.Key = "ScriptErrorsPanel", .Factory = &MakePanel<ScriptErrorsPanel>, .Update = &UpdatePanel<ScriptErrorsPanel>},
    // Updates while hidden: its version poll is what reveals the panel on a failed
    // shader compile.
    {.Key = "ShaderErrorsPanel",
     .Factory = &MakePanel<ShaderErrorsPanel>,
     .Update = &UpdatePanel<ShaderErrorsPanel>,
     .UpdateWhenHidden = true},
    // Updates while hidden: chart history keeps accumulating, so reopening the panel
    // shows continuous data.
    {.Key = "MonitorsPanel",
     .Factory = &MakePanel<MonitorsPanel>,
     .Update = &UpdatePanel<MonitorsPanel>,
     .UpdateWhenHidden = true},
    {.Key = "RenderGraphPanel",
     .Factory = &MakePanel<RenderGraphPanel>,
     .Update = &UpdateRenderGraphPanel,
     .Anchor = EditorPanelIds::SceneView},
    {.Key = "CpuProfilerPanel", .Factory = &MakePanel<CpuProfilerPanel>, .Update = &UpdatePanel<CpuProfilerPanel>},
    {.Key = "VisualProfilerPanel",
     .Factory = &MakePanel<VisualProfilerPanel>,
     .Update = &UpdateVisualProfilerPanel,
     .Anchor = EditorPanelIds::SceneView},
    {.Key = "VramPanel",
     .Factory = &MakePanel<VramPanel>,
     .Update = &UpdateVramPanel,
     .Anchor = EditorPanelIds::SceneView},
    {.Key = "ScriptEditorPanel", .Factory = &MakePanel<ScriptEditorPanel>, .Anchor = EditorPanelIds::SceneView},
    {.Key = "SettingsPanel", .Factory = &MakePanel<SettingsPanel>, .Anchor = EditorPanelIds::Hierarchy},
    {.Key = "DiffPanel", .Factory = &MakePanel<DiffPanel>, .Anchor = EditorPanelIds::SceneView},
    {.Key = "WebPanel", .Factory = &MakePanel<WebPanel>, .Anchor = EditorPanelIds::SceneView},
    {.Key = "BuildPanel",
     .Factory = &MakePanel<BuildPanel>,
     .Anchor = EditorPanelIds::Hierarchy,
     .SplitBelowAnchor = kBuildSplitBelowHierarchy,
     .OnShown = &RefreshBuildVersions},
    {.Key = "AnimationWindowPanel", .Factory = &MakePanel<AnimationWindowPanel>},
    {.Key = "AnimationTimelinePanel", .Factory = &MakePanel<AnimationTimelinePanel>},
    {.Key = "AnimationClipEditorPanel", .Factory = &MakePanel<AnimationClipEditorPanel>},
    {.Key = "MixerPanel",
     .Factory = &MakePanel<MixerPanel>,
     .Anchor = EditorPanelIds::Hierarchy,
     .SplitBelowAnchor = kMixerSplitBelowHierarchy},
    {.Key = "UndoHistoryPanel", .Factory = &MakePanel<UndoHistoryPanel>, .Anchor = EditorPanelIds::Inspector},
    {.Key = "NodeGraphPanel", .Factory = &MakeMaterialGraphPanel, .Anchor = EditorPanelIds::SceneView},
    {.Key = "ShaderGraphPanel", .Factory = &MakeMaterialGraphPanel, .Anchor = EditorPanelIds::SceneView},
    {.Key = "AnimationGraphPanel", .Factory = &MakeAnimationGraphPanel, .Anchor = EditorPanelIds::SceneView},
    {.Key = "GameLogicGraphPanel", .Factory = &MakeGameLogicGraphPanel, .Anchor = EditorPanelIds::SceneView},
    {.Key = "TilePalettePanel", .Factory = &MakePanel<TilePalettePanel>},
    {.Key = "TileSetEditorPanel", .Factory = &MakePanel<TileSetEditorPanel>},
};

constexpr FallbackDockPanel kFallbackDockPanels[] = {
    {EditorPanelIds::Assets, "AssetsPanel"},
    {EditorPanelIds::Hierarchy, "HierarchyPanel"},
    {EditorPanelIds::Inspector, "InspectorPanel"},
    {EditorPanelIds::Inspector2, "InspectorPanel"},
    {EditorPanelIds::GameView, "GameViewPanel"},
    {EditorPanelIds::SceneView, "SceneViewPanel"},
    {EditorPanelIds::UiDemo, "UIDemoPanel"},
    {EditorPanelIds::Log, "LogPanel"},
    {EditorPanelIds::ScriptErrors, "ScriptErrorsPanel"},
    {EditorPanelIds::Web, "WebPanel"},
    {EditorPanelIds::NodeGraph, "NodeGraphPanel"},
    {EditorPanelIds::Build, "BuildPanel"},
    {EditorPanelIds::Animation, "AnimationWindowPanel"},
    {EditorPanelIds::Timeline, "AnimationTimelinePanel"},
    {EditorPanelIds::ClipEditor, "AnimationClipEditorPanel"},
    {EditorPanelIds::ShaderErrors, "ShaderErrorsPanel"},
    {EditorPanelIds::AnimationGraph, "AnimationGraphPanel"},
    {EditorPanelIds::GameLogicGraph, "GameLogicGraphPanel"},
    {EditorPanelIds::TilePalette, "TilePalettePanel"},
    {EditorPanelIds::TileSetEditor, "TileSetEditorPanel"},
};

} // namespace

void RegisterBuiltInPanelTypes()
{
    auto& registry = EditorPanelRegistry::Get();
    for (const BuiltInPanelType& type : kBuiltInPanelTypes)
    {
        EditorPanelTypeDescriptor descriptor;
        descriptor.TypeKey = type.Key;
        descriptor.Factory = type.Factory;
        descriptor.Update = type.Update;
        descriptor.UpdateWhenHidden = type.UpdateWhenHidden;
        descriptor.DefaultAnchorPanelId = type.Anchor ? type.Anchor : "";
        descriptor.DefaultSplitBelowAnchorRatio = type.SplitBelowAnchor;
        descriptor.OnShown = type.OnShown;
        registry.RegisterPanelType(std::move(descriptor));
    }
}

std::span<const FallbackDockPanel> FallbackDockPanels()
{
    return kFallbackDockPanels;
}

} // namespace GameEngine::Editor
