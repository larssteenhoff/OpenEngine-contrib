#pragma once

#include "Rendering/Core/RenderGraph/RGTypes.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{

class DockingManager;
class EditorApplication;
class UIElement;

namespace ECS
{
class World;
struct EntityHandle;
}

namespace Rendering::RenderGraph
{
class RGFrame;
}

// Helpers shared by more than one debug-handler domain. Private to the DebugServer
// handler files; not an editor API.

// Render-graph introspection reads the MAIN window's live RGFrame at request time, which
// holds the previous frame's compiled graph (the next BeginFrame, which clears it, runs
// later in the frame loop) — a stable snapshot.
inline constexpr const char* kRgNoFrameMsg = "No live render graph (headless, or no frame rendered yet)";

Rendering::RenderGraph::RGFrame* MainRgFrame(EditorApplication& app);

const char* RgQueueName(Rendering::RenderGraph::RGQueue q);

// Material texture bindings still waiting on a decode, world-wide. Reported on
// every capture and by get_editor_state: a frame taken while this is non-zero
// shows bindless defaults (white albedo) for those materials, which on screen is
// indistinguishable from a material that never binds. A first material bind tags
// the texture's cook usage, so on a cold derived cache the window is as wide as
// the block-compression encode — seconds to minutes. Zero when the renderer is
// not up, which is the honest answer for a frame with no material binding at all.
size_t PendingMaterialTextureBinds();

// The entity's Name component value, or empty when it has none.
std::string EntityNameFromWorld(ECS::World* world, ECS::EntityHandle entity);

// View activation, shared by focus_view and set_play_mode's activateGameView so
// the two can never drift into different notions of "that view is up".
struct ViewActivation
{
    // False only when the main window or its UI manager is unavailable; the
    // caller reports that rather than claiming a view it never raised.
    bool ok = false;
    // Read back from the dock model and the UI manager after the fact, never
    // assumed from the request.
    bool active = false;
    bool viewportFocused = false;
    std::string focusedElementId;
};

// Bring `panelId` to the front of its dock tab group and apply the UI focus that
// view wants. The two viewports want opposite things from UI focus — the Game
// View's viewport must hold it for input to reach the running game, the Scene
// View's must not or UIManager consumes its camera keys before the editor input
// actions see them — so the policy travels with the request, not with the panel.
ViewActivation ActivateDockedView(EditorApplication& app,
                                  DockingManager& docking,
                                  const std::string& panelId,
                                  bool wantsViewportUiFocus);

// Find a panel of type T in the editor's panel storage.
template <typename T>
T* FindPanel(const std::vector<std::unique_ptr<UIElement>>& panels)
{
    for (auto& el : panels)
    {
        if (auto* p = dynamic_cast<T*>(el.get()))
            return p;
    }
    return nullptr;
}

} // namespace GameEngine
