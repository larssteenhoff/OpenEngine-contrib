#include "DebugServer/GraphDebugHandlers.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"

#include "Graph/GraphCanvas.h"
#include "Graph/GraphModel.h"
#include "Panels/GraphPanel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace GameEngine
{

using json = nlohmann::json;

static GraphPanel* ResolveDebugGraphPanel(std::string_view kindId = {})
{
    if (!kindId.empty())
    {
        if (GraphPanel* live = GraphPanel::FindLiveByKind(kindId))
            return live;
    }
    if (GraphPanel* last = GraphPanel::FindLiveLastOpened())
        return last;
    GraphPanel* first = nullptr;
    GraphPanel::ForEachLive([&first](GraphPanel& panel) {
        if (!first)
            first = &panel;
    });
    return first;
}

void RegisterGraphDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    server.RegisterHandler("graph_set_viewport", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        GraphPanel* nodeGraphPanel = ResolveDebugGraphPanel();
        if (!nodeGraphPanel)
            return Editor::RefuseRequest("Node Graph panel not found");
        auto* canvas = dynamic_cast<GraphCanvas*>(nodeGraphPanel->FindById("NodeGraphCanvas"_sid));
        if (!canvas)
            return Editor::RefuseRequest("NodeGraphCanvas not found");
        float panX = 0.f;
        float panY = 0.f;
        float zoom = 1.f;
        canvas->GetPanZoom(panX, panY, zoom);
        panX = ctx.params.value("panX", panX);
        panY = ctx.params.value("panY", panY);
        zoom = ctx.params.value("zoom", zoom);
        canvas->SetPanZoom(panX, panY, zoom);
        canvas->GetPanZoom(panX, panY, zoom);
        return json{{"ok", true}, {"panX", panX}, {"panY", panY}, {"zoom", zoom}}; });

    server.RegisterHandler("graph_spike_dummy_nodes", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        GraphPanel* nodeGraphPanel = ResolveDebugGraphPanel();
        if (!nodeGraphPanel)
            return Editor::RefuseRequest("Node Graph panel not found");
        auto* canvas = dynamic_cast<GraphCanvas*>(nodeGraphPanel->FindById("NodeGraphCanvas"_sid));
        if (!canvas)
            return Editor::RefuseRequest("NodeGraphCanvas not found");
        const int count = ctx.params.value("count", 60);
        canvas->SpikeSpawnDummyNodes(count);
        return json{{"ok", true}, {"count", count}}; });

    server.RegisterHandler("graph_get_nodes", [](const EditorDebugServer::RequestContext&) -> json
                           {
        GraphPanel* nodeGraphPanel = ResolveDebugGraphPanel();
        if (!nodeGraphPanel)
            return Editor::RefuseRequest("Node Graph panel not found");
        const Graph::Model* model = nodeGraphPanel->ActiveGraphModel();
        if (!model)
            return Editor::RefuseRequest("No graph model");
        json nodes = json::array();
        for (const Graph::Node& node : model->Nodes)
        {
            nodes.push_back(json{{"id", node.Id},
                                 {"typeId", node.TypeId},
                                 {"x", node.PositionX},
                                 {"y", node.PositionY}});
        }
        return json{{"ok", true}, {"nodes", nodes}}; });

    server.RegisterHandler("graph_delete_node", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string nodeId = ctx.params.value("nodeId", std::string{});
        if (nodeId.empty())
            return Editor::RefuseRequest("Provide nodeId");
        GraphPanel* nodeGraphPanel = ResolveDebugGraphPanel();
        if (!nodeGraphPanel)
            return Editor::RefuseRequest("Node Graph panel not found");
        auto* canvas = dynamic_cast<GraphCanvas*>(nodeGraphPanel->FindById("NodeGraphCanvas"_sid));
        if (!canvas)
            return Editor::RefuseRequest("NodeGraphCanvas not found");
        const Graph::Model* model = nodeGraphPanel->ActiveGraphModel();
        if (!model)
            return Editor::RefuseRequest("No graph model");
        const auto& nodes = model->Nodes;
        const bool exists = std::any_of(nodes.begin(), nodes.end(),
                                        [&nodeId](const Graph::Node& n) { return n.Id == nodeId; });
        if (!exists)
            return Editor::RefuseRequest("No node with id: " + nodeId);
        /* Same path as the Delete key: selection + DeleteSelectedNodes keeps
           undo, link cleanup and widget recycling on the one implementation. */
        canvas->SetSelectedNodeId(nodeId);
        canvas->DeleteSelectedNodes();
        return json{{"ok", true}, {"deleted", nodeId}}; });
}

} // namespace GameEngine
