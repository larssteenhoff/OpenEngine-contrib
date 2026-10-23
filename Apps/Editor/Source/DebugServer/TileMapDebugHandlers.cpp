#include "DebugServer/TileMapDebugHandlers.h"
#include "DebugServer/TileMapRulesWindow.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "EditorApplication.h"

#include "Panels/TilePalettePanel.h"
#include "Panels/TileSetEditorPanel.h"
#include "SceneView/TileMapPaintTool.h"
#include "SceneView/TileMapPaintToolStripEntry.h"
#include "SceneViewController.h"
#include "TileMap/TileMapEditContext.h"
#include "TileMap/TileMapEditorIntegration.h"
#include "TileMap/TileReplaceEdit.h"
#include "UndoRedo/UndoRedoService.h"
#include "TileMap/TileMapSourceImport.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "TileMap/RuleWizard.h"
#include "TileMap/TerrainPeering.h"
#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileRuleEvaluator.h"
#include "TileMap/TileRules.h"
#include "TileMap/TileSetAsset.h"

#include <nlohmann/json.hpp>

#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

// A stroke driven over IPC is one undo entry, same as a pointer stroke, so a
// caller asking for many cells must not be able to flood the undo stack.
constexpr int32 kMaxStrokeCells = 4096;
constexpr const char* kNoPaintTargetError =
    "No paint target — select an entity with a TileMapLayerRef whose TileMap and LayerId are set";

/** Reads a JSON number as a uint32; false when it is not a whole number in 0..UINT32_MAX. */
bool ReadUInt32(const json& value, uint32& out)
{
    if (!value.is_number_integer())
        return false;
    const int64 number = value.get<int64>();
    if (number < 0 || number > static_cast<int64>(std::numeric_limits<uint32>::max()))
        return false;
    out = static_cast<uint32>(number);
    return true;
}

Editor::TileMapTool ParseTool(const std::string& name, bool& outValid)
{
    outValid = true;
    if (name == "brush")   return Editor::TileMapTool::Brush;
    if (name == "eraser")  return Editor::TileMapTool::Eraser;
    if (name == "picker")  return Editor::TileMapTool::Picker;
    if (name == "fill")    return Editor::TileMapTool::Fill;
    if (name == "rect")    return Editor::TileMapTool::Rect;
    if (name == "marquee") return Editor::TileMapTool::Marquee;
    if (name == "terrain") return Editor::TileMapTool::Terrain;
    if (name == "intgrid") return Editor::TileMapTool::IntGrid;
    outValid = false;
    return Editor::TileMapTool::Brush;
}

TileMap::TerrainMatchMode ParseMatchMode(const std::string& name, bool& outValid)
{
    outValid = true;
    if (name == "cornersAndSides") return TileMap::TerrainMatchMode::CornersAndSides;
    if (name == "corners")         return TileMap::TerrainMatchMode::Corners;
    if (name == "sides")           return TileMap::TerrainMatchMode::Sides;
    outValid = false;
    return TileMap::TerrainMatchMode::CornersAndSides;
}

Editor::SceneTools::TileMapPaintTool* ResolvePaintTool(EditorApplication& app)
{
    if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
        return nullptr;
    return Editor::GetTileMapPaintTool(*app.m_Windows[0]->scene);
}


/** Parses one pattern token: "." don't care, "*" any, "=N" require, "!N" forbid. */
bool ParsePatternToken(const std::string& token, TileMap::RulePatternCell& outCell)
{
    if (token == ".")
    {
        outCell = TileMap::RulePatternCell{};
        return true;
    }
    if (token == "*")
    {
        outCell = TileMap::RulePatternCell{TileMap::RulePatternOp::RequireAny, 0};
        return true;
    }
    if (token.size() < 2 || (token[0] != '=' && token[0] != '!'))
        return false;
    const int64 value = std::strtoll(token.c_str() + 1, nullptr, 10);
    if (value < 0 || value > 0xFF)
        return false;
    outCell = TileMap::RulePatternCell{token[0] == '=' ? TileMap::RulePatternOp::RequireValue
                                                       : TileMap::RulePatternOp::ForbidValue,
                                       static_cast<uint8>(value)};
    return true;
}

/** Splits a comma-separated pattern row into its tokens. */
std::vector<std::string> SplitPatternRow(const std::string& row)
{
    std::vector<std::string> tokens;
    size_t start = 0;
    while (true)
    {
        const size_t comma = row.find(',', start);
        tokens.push_back(row.substr(start, comma == std::string::npos ? comma : comma - start));
        if (comma == std::string::npos)
            return tokens;
        start = comma + 1;
    }
}

/** Builds one rule from its JSON spec; returns false with a reason on malformed input. */
bool ParseRuleSpec(const json& ruleJson, uint32 id, TileMap::TileRule& outRule, std::string& outError)
{
    outRule.Id = id;
    outRule.Name = ruleJson.value("name", std::string());
    outRule.Chance = ruleJson.value("chance", 1.0f);
    outRule.BreakOnMatch = ruleJson.value("breakOnMatch", true);
    outRule.MirrorX = ruleJson.value("mirrorX", false);
    outRule.MirrorY = ruleJson.value("mirrorY", false);
    outRule.ModuloX = ruleJson.value("moduloX", 1);
    outRule.ModuloY = ruleJson.value("moduloY", 1);
    outRule.OffsetX = ruleJson.value("offsetX", 0);
    outRule.OffsetY = ruleJson.value("offsetY", 0);

    if (!ruleJson.contains("pattern") || !ruleJson["pattern"].is_array())
    {
        outError = "each rule needs a pattern array of row strings";
        return false;
    }
    const int32 size = static_cast<int32>(ruleJson["pattern"].size());
    if (!TileMap::IsValidRulePatternSize(size))
    {
        outError = "pattern must have 1, 3 or 5 rows";
        return false;
    }
    outRule.PatternSize = size;
    outRule.Pattern.assign(static_cast<size_t>(size) * static_cast<size_t>(size),
                           TileMap::RulePatternCell{});
    for (int32 y = 0; y < size; ++y)
    {
        const std::vector<std::string> tokens =
            SplitPatternRow(ruleJson["pattern"][y].get<std::string>());
        if (static_cast<int32>(tokens.size()) != size)
        {
            outError = "every pattern row must have as many cells as there are rows";
            return false;
        }
        for (int32 x = 0; x < size; ++x)
        {
            if (!ParsePatternToken(tokens[static_cast<size_t>(x)],
                                   outRule.Pattern[static_cast<size_t>(y) *
                                                       static_cast<size_t>(size) +
                                                   static_cast<size_t>(x)]))
            {
                outError = "pattern tokens must be one of . * =N !N";
                return false;
            }
        }
    }

    if (ruleJson.contains("stamp"))
    {
        outRule.OutputMode = TileMap::RuleOutputMode::Stamp;
        outRule.Stamp.assign(static_cast<size_t>(size) * static_cast<size_t>(size),
                             TileMap::kEmptyCell);
        if (!ruleJson["stamp"].is_array() ||
            ruleJson["stamp"].size() != outRule.Stamp.size())
        {
            outError = "stamp must hold one tile id per pattern cell";
            return false;
        }
        for (size_t i = 0; i < outRule.Stamp.size(); ++i)
            outRule.Stamp[i] = TileMap::MakeCell(ruleJson["stamp"][i].get<uint32>());
        return true;
    }

    if (!ruleJson.contains("tiles") || !ruleJson["tiles"].is_array() ||
        ruleJson["tiles"].empty())
    {
        outError = "each rule needs a non-empty tiles array (or a stamp)";
        return false;
    }
    for (const auto& tileJson : ruleJson["tiles"])
    {
        const uint32 tileId =
            tileJson.is_object() ? tileJson.value("tileId", 0u) : tileJson.get<uint32>();
        const float32 weight = tileJson.is_object() ? tileJson.value("weight", 1.0f) : 1.0f;
        outRule.Tiles.push_back(TileMap::RuleTileOutput{TileMap::MakeCell(tileId), weight});
    }
    return true;
}

/** tileset_author_terrain: replaces the tile set's terrain set and per-tile matches, then saves through the panel. */
json AuthorTileSetTerrain(TileSetEditorPanel* panel, const json& params)
{
    if (!params.contains("tileSetGuid"))
        return Editor::RefuseRequest("tileSetGuid required");
    const GUID tileSetGuid(params["tileSetGuid"].get<std::string>());
    if (tileSetGuid.IsNull())
        return Editor::RefuseRequest("tileSetGuid is not a valid GUID");
    if (!params.contains("terrains") || !params["terrains"].is_array() ||
        params["terrains"].empty())
    {
        return Editor::RefuseRequest("terrains must be a non-empty array of names");
    }
    if (!params.contains("tiles") || !params["tiles"].is_array())
        return Editor::RefuseRequest("tiles must be an array of peering entries");

    bool modeValid = true;
    const TileMap::TerrainMatchMode matchMode =
        ParseMatchMode(params.value("matchMode", std::string("cornersAndSides")), modeValid);
    if (!modeValid)
        return Editor::RefuseRequest("matchMode must be cornersAndSides|corners|sides");

    if (!panel)
        return Editor::RefuseRequest("Tile Set Editor panel not instantiated");

    panel->OpenTileSet(tileSetGuid);
    TileMap::TileSetAsset* tileSet = panel->EditedTileSet();
    if (!tileSet)
        return Editor::RefuseRequest("Tile set could not be opened (see the panel status line)");

    TileMap::TerrainSetDef set;
    set.Name = params.value("name", std::string("Terrain Set 1"));
    set.MatchMode = matchMode;
    for (const auto& terrainJson : params["terrains"])
    {
        set.Terrains.push_back(TileMap::TerrainDef{
            terrainJson.get<std::string>(),
            TileSetEditorPanel::DefaultTerrainColor(set.Terrains.size())});
    }
    tileSet->TerrainSets().assign(1, std::move(set));

    int32 annotated = 0;
    for (const auto& tileJson : params["tiles"])
    {
        if (!tileJson.is_object() || !tileJson.contains("id"))
            return Editor::RefuseRequest("each tiles entry needs an id");
        uint32 tileId = 0;
        if (!ReadUInt32(tileJson["id"], tileId))
            return Editor::RefuseRequest("each tiles entry id must be a non-negative integer");
        TileMap::TileSetTile* tile = tileSet->FindTile(tileId);
        if (!tile)
            return Editor::RefuseRequest("tile id " + std::to_string(tileId) + " is not in this tile set");

        TileMap::TerrainPeering peering;
        peering.TerrainSet = 0;
        peering.Center = static_cast<int16>(tileJson.value("center", -1));
        if (tileJson.contains("neighbors"))
        {
            const auto& neighbors = tileJson["neighbors"];
            if (!neighbors.is_array() || neighbors.size() != TileMap::kPeeringSlotCount)
                return Editor::RefuseRequest("neighbors must be an array of 8 terrain indices");
            for (uint32 slot = 0; slot < TileMap::kPeeringSlotCount; ++slot)
                peering.Neighbors[slot] = static_cast<int16>(neighbors[slot].get<int32>());
        }
        tile->Peering = peering;
        ++annotated;
    }

    if (!panel->ApplyAndSave())
        return Editor::RefuseRequest("Save failed (see the panel status line and log)");
    return json{{"ok", true},
                {"path", tileSet->GetPath().string()},
                {"annotated", annotated}};
}

/** tilemap_paint_intgrid: paints one IntGrid value over the given cells as one stroke. */
json PaintIntGridCells(Editor::SceneTools::TileMapPaintTool* tool, Editor::TileMapEditContext* editContext,
                       const json& params)
{
    if (!tool)
        return Editor::RefuseRequest("No tilemap paint tool");
    if (!editContext)
        return Editor::RefuseRequest("No tilemap edit context");

    if (!params.contains("cells") || !params["cells"].is_array() ||
        params["cells"].empty())
    {
        return Editor::RefuseRequest("cells must be a non-empty array of [x,y] pairs");
    }
    if (static_cast<int32>(params["cells"].size()) > kMaxStrokeCells)
        return Editor::RefuseRequest("cells exceeds the per-stroke cap of 4096");
    if (!params.contains("value") || !params["value"].is_number_integer())
        return Editor::RefuseRequest("value is a required integer in 0..255");
    const int64 value = params["value"].get<int64>();
    if (value < 0 || value > 0xFF)
        return Editor::RefuseRequest("value must be in 0..255");

    std::vector<std::pair<int32, int32>> cells;
    cells.reserve(params["cells"].size());
    for (const auto& entry : params["cells"])
    {
        if (!entry.is_array() || entry.size() != 2 || !entry[0].is_number_integer() ||
            !entry[1].is_number_integer())
        {
            return Editor::RefuseRequest("each cell must be [x,y] integers");
        }
        cells.emplace_back(entry[0].get<int32>(), entry[1].get<int32>());
    }

    editContext->SetTool(Editor::TileMapTool::IntGrid);
    editContext->SetIntGridValue(static_cast<uint8>(value));
    for (size_t i = 0; i < cells.size(); ++i)
    {
        if (!tool->PaintCellForAutomation(cells[i].first, cells[i].second, i == 0,
                                          i + 1 == cells.size()))
        {
            return Editor::RefuseRequest(kNoPaintTargetError, json{{"cellsPainted", i}});
        }
    }
    return json{{"ok", true}, {"cells", cells.size()}, {"value", value}};
}

/** tilemap_author_rules: writes IntGrid values and a Tiles layer's rule set, then updates its generated tiles. */
json AuthorTileMapRules(Editor::TileMapEditContext* editContext, const json& params)
{
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (!editContext || !store)
        return Editor::RefuseRequest("No tilemap edit context or runtime store");

    const GUID mapGuid = params.contains("mapGuid")
                             ? GUID(params["mapGuid"].get<std::string>())
                             : editContext->TargetMap();
    if (mapGuid.IsNull())
        return Editor::RefuseRequest("No target tilemap — select a tilemap layer entity first");
    TileMap::TileMapData* data = store->GetMap(mapGuid);
    if (!data)
        return Editor::RefuseRequest("Tilemap could not be resolved");

    json out{{"ok", true}};

    if (params.contains("values"))
    {
        const uint32 intGridLayerId = params.value("intGridLayer", 0u);
        TileMap::TileMapLayerData* layer = data->FindLayer(intGridLayerId);
        if (!layer || layer->Type != TileMap::TileMapLayerType::IntGrid)
            return Editor::RefuseRequest("intGridLayer must name an IntGrid layer of this map");
        layer->IntGridValues.clear();
        for (const auto& valueJson : params["values"])
        {
            const uint32 value = valueJson.value("value", 0u);
            if (value == 0u || value > 0xFFu)
                return Editor::RefuseRequest("IntGrid values must be in 1..255");
            layer->IntGridValues.push_back(
                TileMap::IntGridValueDef{static_cast<uint8>(value),
                                         valueJson.value("name", std::string()),
                                         valueJson.value("colorRGBA", 0xFFFFFFFFu)});
        }
        out["values"] = layer->IntGridValues.size();
    }

    if (!params.contains("rulesLayer"))
    {
        store->MarkStructuralChange(mapGuid);
        return out;
    }

    uint32 rulesLayerId = 0;
    if (!ReadUInt32(params["rulesLayer"], rulesLayerId))
        return Editor::RefuseRequest("rulesLayer must be a non-negative integer layer id");
    TileMap::TileMapLayerData* rulesLayer = data->FindLayer(rulesLayerId);
    if (!rulesLayer || rulesLayer->Type != TileMap::TileMapLayerType::Tiles)
        return Editor::RefuseRequest("rulesLayer must name a Tiles layer of this map");

    uint32 sourceLayerId = rulesLayer->Rules.SourceLayerId;
    if (params.contains("sourceLayer") && !ReadUInt32(params["sourceLayer"], sourceLayerId))
        return Editor::RefuseRequest("sourceLayer must be a non-negative integer layer id");
    uint32 seed = rulesLayer->Rules.Seed;
    if (params.contains("seed") && !ReadUInt32(params["seed"], seed))
        return Editor::RefuseRequest("seed must be a non-negative integer");

    if (params.value("clearRules", true))
        rulesLayer->Rules.Groups.clear();
    rulesLayer->Rules.SourceLayerId = sourceLayerId;
    rulesLayer->Rules.Seed = seed;

    const TileMap::TileMapLayerData* source = data->FindLayer(rulesLayer->Rules.SourceLayerId);
    if (!source || source->Type != TileMap::TileMapLayerType::IntGrid)
        return Editor::RefuseRequest("sourceLayer must name an IntGrid layer of this map");

    if (params.contains("groups"))
    {
        for (const auto& groupJson : params["groups"])
        {
            TileMap::TileRuleGroup group;
            group.Id = rulesLayer->Rules.NextGroupId++;
            group.Name = groupJson.value("name", std::string());
            group.Enabled = groupJson.value("enabled", true);
            if (!groupJson.contains("rules") || !groupJson["rules"].is_array())
                return Editor::RefuseRequest("each group needs a rules array");
            for (const auto& ruleJson : groupJson["rules"])
            {
                TileMap::TileRule rule;
                std::string error;
                if (!ParseRuleSpec(ruleJson, rulesLayer->Rules.NextRuleId, rule, error))
                    return Editor::RefuseRequest(error);
                ++rulesLayer->Rules.NextRuleId;
                group.Rules.push_back(std::move(rule));
            }
            rulesLayer->Rules.Groups.push_back(std::move(group));
        }
    }

    if (params.contains("wizard"))
    {
        const auto& wizardJson = params["wizard"];
        const GUID tileSetGuid = wizardJson.contains("tileSetGuid")
                                     ? GUID(wizardJson["tileSetGuid"].get<std::string>())
                                     : rulesLayer->TileSetGuid;
        const TileMap::TileSetAsset* tileSet = store->GetTileSet(tileSetGuid);
        if (!tileSet)
            return Editor::RefuseRequest("wizard needs a resolvable tileset on the rules layer");

        TileMap::RuleWizardRequest request;
        request.TerrainSet = static_cast<int16>(wizardJson.value("terrainSet", 0));
        request.Terrain = static_cast<int16>(wizardJson.value("terrain", 0));
        request.IntGridValue = static_cast<uint8>(wizardJson.value("intGridValue", 1));
        request.Seed = wizardJson.value("seed", rulesLayer->Rules.Seed);
        request.Template = wizardJson.value("template", std::string("corners16")) == "blob47"
                               ? TileMap::RuleTemplate::Blob47
                               : TileMap::RuleTemplate::Corners16;
        TileMap::TileRuleGroup group =
            TileMap::GenerateTerrainRuleGroup(*tileSet, request, rulesLayer->Rules);
        if (group.Rules.empty())
            return Editor::RefuseRequest("wizard produced no rules — check terrainSet/terrain");
        out["wizardRules"] = group.Rules.size();
        rulesLayer->Rules.Groups.push_back(std::move(group));
    }

    std::vector<TileMapECS::TileMapDressedChange> changes;
    if (!store->DressLayer(mapGuid, rulesLayerId, changes))
        return Editor::RefuseRequest("Rule evaluation refused — check sourceLayer");
    store->MarkStructuralChange(mapGuid);
    out["dressedCells"] = changes.size();
    out["groups"] = rulesLayer->Rules.Groups.size();
    return out;
}

/** tilemap_rules_state: reports a map's IntGrid values, rule set shape and an optional cell window. */
json DescribeTileMapRules(const Editor::TileMapEditContext* editContext, const json& params)
{
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (!editContext || !store)
        return Editor::RefuseRequest("No tilemap edit context or runtime store");

    const GUID mapGuid = params.contains("mapGuid")
                             ? GUID(params["mapGuid"].get<std::string>())
                             : editContext->TargetMap();
    const TileMap::TileMapData* data = mapGuid.IsNull() ? nullptr : store->GetMap(mapGuid);
    if (!data)
        return Editor::RefuseRequest("No target tilemap");

    json layers = json::array();
    for (const TileMap::TileMapLayerData& layer : data->Layers())
    {
        json layerJson{{"id", layer.Id},
                       {"name", layer.Name},
                       {"type", layer.Type == TileMap::TileMapLayerType::IntGrid ? "intgrid"
                                                                                 : "tiles"}};
        json values = json::array();
        for (const TileMap::IntGridValueDef& def : layer.IntGridValues)
            values.push_back(json{{"value", def.Value}, {"name", def.Name}});
        layerJson["intGridValues"] = std::move(values);
        layerJson["ruleSourceLayer"] = layer.Rules.SourceLayerId;
        layerJson["ruleSeed"] = layer.Rules.Seed;
        size_t ruleCount = 0;
        for (const TileMap::TileRuleGroup& group : layer.Rules.Groups)
            ruleCount += group.Rules.size();
        layerJson["ruleGroups"] = layer.Rules.Groups.size();
        layerJson["rules"] = ruleCount;
        layers.push_back(std::move(layerJson));
    }

    json out{{"mapGuid", mapGuid.ToString()}, {"layers", std::move(layers)}};

    if (params.contains("window"))
    {
        const auto& windowJson = params["window"];
        const uint32 layerId = params.value("layer", editContext->TargetLayer());
        const TileMap::TileMapLayerData* layer = data->FindLayer(layerId);
        if (!layer)
            return Editor::RefuseRequest("window needs a layer id that exists");
        json window;
        if (!Editor::BuildTileMapRulesWindow(*layer, windowJson, window))
            return Editor::RefuseRequest("window needs ordered int32 integer coordinates and no more than 4096 cells");
        out["window"] = std::move(window);
    }
    return out;
}

} // namespace

void RegisterTileMapDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    // A static local so the handler lambdas below can call it without capturing
    // it; its body sits inside this friend function, which is what grants the
    // access to the application's private member.
    static const auto TileMapEditContextOf = [](EditorApplication& a) -> Editor::TileMapEditContext*
    { return a.m_TileMapEditor ? &a.m_TileMapEditor->EditContext() : nullptr; };

    // The panel storage is private to EditorApplication and reachable from this
    // friend function; a namespace-scope helper would not be.
    const auto resolveTileSetEditorPanel = [&app]() -> TileSetEditorPanel* {
        for (const auto& stored : app.m_PanelStorage)
        {
            if (auto* candidate = dynamic_cast<TileSetEditorPanel*>(stored.get()))
                return candidate;
        }
        return nullptr;
    };

    // tilemap_paint_cells — drive the Scene View paint tool over a list of cells,
    // as one stroke and therefore one undo entry.
    //
    // This is the automation entry point for tilemap authoring: it calls the same
    // TileMapPaintTool the viewport calls, so it exercises the real cell write,
    // the real dirty-chunk ledger and the real undo boundary. It deliberately does
    // NOT reimplement any of them — a harness that wrote cells directly could pass
    // while interactive painting was broken.
    //
    // The tool paints the layer of whichever selected entity carries a
    // TileMapLayerRef, so select that entity first (select_entity).
    //
    // Params:
    //   cells  [[x,y], ...] cell coordinates to paint, in order (required, non-empty)
    //   tool   brush|eraser|picker|fill|rect|marquee|terrain (optional; leaves the armed tool alone)
    //   terrain / terrainSet  terrain index to paint with the terrain tool (-1 erases)
    //   tileSetGuid  tileset the ids/terrains index into (optional; clears stamp + terrain)
    //   tileId 1-based tile id to stamp (optional; sets a single-tile stamp first)
    //   flipH/flipV/transpose  orientation bits for that stamp (optional, default false)
    server.RegisterHandler("tilemap_paint_cells",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
    {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view controller");
        auto* tool = Editor::GetTileMapPaintTool(*app.m_Windows[0]->scene);
        if (!tool)
            return Editor::RefuseRequest("No tilemap paint tool");
        auto* editContext = TileMapEditContextOf(app);
        if (!editContext)
            return Editor::RefuseRequest("No tilemap edit context");

        if (!ctx.params.contains("cells") || !ctx.params["cells"].is_array() ||
            ctx.params["cells"].empty())
        {
            return Editor::RefuseRequest("cells must be a non-empty array of [x,y] pairs");
        }
        const auto& cellsJson = ctx.params["cells"];
        if (static_cast<int32>(cellsJson.size()) > kMaxStrokeCells)
            return Editor::RefuseRequest("cells exceeds the per-stroke cap of 4096");

        std::vector<std::pair<int32, int32>> cells;
        cells.reserve(cellsJson.size());
        for (const auto& entry : cellsJson)
        {
            if (!entry.is_array() || entry.size() != 2 || !entry[0].is_number_integer() ||
                !entry[1].is_number_integer())
            {
                return Editor::RefuseRequest("each cell must be [x,y] integers");
            }
            cells.emplace_back(entry[0].get<int32>(), entry[1].get<int32>());
        }

        // The tileset must be armed before the stamp or terrain choice: switching
        // it clears both, since their indices only mean anything against one set.
        if (ctx.params.contains("tileSetGuid"))
        {
            const GUID tileSetGuid(ctx.params["tileSetGuid"].get<std::string>());
            if (tileSetGuid.IsNull())
                return Editor::RefuseRequest("tileSetGuid is not a valid GUID");
            editContext->SetTileSet(tileSetGuid);
        }

        if (ctx.params.contains("tool"))
        {
            bool valid = false;
            const Editor::TileMapTool tool_ = ParseTool(ctx.params["tool"].get<std::string>(), valid);
            if (!valid)
                return Editor::RefuseRequest("tool must be brush|eraser|picker|fill|rect|marquee|terrain|intgrid");
            editContext->SetTool(tool_);
        }

        if (ctx.params.contains("scatter"))
            editContext->SetRandomScatter(ctx.params["scatter"].get<bool>());

        // Terrain painting has no stamp: the tile is resolved per cell from the
        // terrain field, so what the caller picks is a terrain, not a tile id.
        if (ctx.params.contains("terrain"))
        {
            editContext->SetTerrain(
                static_cast<int16>(ctx.params.value("terrainSet", 0)),
                static_cast<int16>(ctx.params["terrain"].get<int32>()));
        }

        // A multi-tile stamp is what exercises footprint tiling and the weighted
        // scatter pick; tileIds is row-major from the top-left, like the palette.
        if (ctx.params.contains("tileIds"))
        {
            const auto& ids = ctx.params["tileIds"];
            if (!ids.is_array() || ids.empty())
                return Editor::RefuseRequest("tileIds must be a non-empty array");
            const int32 stampWidth = ctx.params.value("stampWidth", static_cast<int32>(ids.size()));
            if (stampWidth <= 0 || static_cast<int32>(ids.size()) % stampWidth != 0)
                return Editor::RefuseRequest("stampWidth must divide tileIds evenly");

            Editor::TileStamp stamp;
            stamp.Width = stampWidth;
            stamp.Height = static_cast<int32>(ids.size()) / stampWidth;
            stamp.Cells.reserve(ids.size());
            for (const auto& id : ids)
            {
                const int64 tileId = id.get<int64>();
                if (tileId < 0 || tileId > static_cast<int64>(TileMap::kMaxTileId))
                    return Editor::RefuseRequest("tileIds entries must be valid tile ids");
                stamp.Cells.push_back(TileMap::MakeCell(static_cast<uint32>(tileId)));
            }
            editContext->SetStamp(std::move(stamp));
        }
        else if (ctx.params.contains("tileId"))
        {
            const int64 tileId = ctx.params["tileId"].get<int64>();
            if (tileId <= 0 || tileId > static_cast<int64>(TileMap::kMaxTileId))
                return Editor::RefuseRequest("tileId must be a positive 1-based tile id");
            Editor::TileStamp stamp;
            stamp.Width = 1;
            stamp.Height = 1;
            stamp.Cells.assign(1, TileMap::MakeCell(static_cast<uint32>(tileId),
                                                    ctx.params.value("flipH", false),
                                                    ctx.params.value("flipV", false),
                                                    ctx.params.value("transpose", false)));
            editContext->SetStamp(std::move(stamp));
        }

        if (ctx.params.contains("orient"))
        {
            const std::string orient = ctx.params["orient"].get<std::string>();
            if (orient == "flipH")       editContext->FlipStampHorizontal();
            else if (orient == "flipV")  editContext->FlipStampVertical();
            else if (orient == "rotate") editContext->RotateStampClockwise();
            else return Editor::RefuseRequest("orient must be flipH|flipV|rotate");
        }

        // The whole list is one stroke: begin on the first cell, end on the last.
        // The tool resolves its target from the selection on every cell, so a
        // refusal means nothing was written; the edit context's own target is
        // stale from an earlier row click and must not vouch for the stroke.
        const size_t last = cells.size() - 1;
        for (size_t i = 0; i < cells.size(); ++i)
        {
            if (!tool->PaintCellForAutomation(cells[i].first, cells[i].second,
                                              /*strokeBegin=*/i == 0, /*strokeEnd=*/i == last,
                                              ctx.params.value("erase", false)))
            {
                return Editor::RefuseRequest(kNoPaintTargetError, json{{"cellsPainted", i}});
            }
        }
        return json{{"ok", true},
                    {"cells", cells.size()},
                    {"mapGuid", editContext->TargetMap().ToString()},
                    {"layerId", editContext->TargetLayer()}};
    });

    // tilemap_save — write the resident map back to its .tilemap asset, compacting
    // chunks an erase emptied. Same call the Tile Palette's Save button makes.
    //
    // Params:
    //   mapGuid  GUID of the .tilemap (optional; defaults to the active paint target)
    server.RegisterHandler("tilemap_save",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
    {
        auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
        if (!store)
            return Editor::RefuseRequest("No tilemap runtime store");

        GUID mapGuid;
        if (ctx.params.contains("mapGuid"))
            mapGuid = GUID(ctx.params["mapGuid"].get<std::string>());
        else if (auto* editContext = TileMapEditContextOf(app))
            mapGuid = editContext->TargetMap();
        if (mapGuid.IsNull())
            return Editor::RefuseRequest("No mapGuid given and no active paint target");

        if (!store->SaveMap(mapGuid))
            return Editor::RefuseRequest("Save failed — unresolved GUID or write error (see log)");
        return json{{"ok", true}, {"mapGuid", mapGuid.ToString()}};
    });

    // tileset_create_from_texture — run the Tile Set Editor's "tile set for this
    // texture" action, which opens the .tileset beside the texture and writes one
    // when there is none. Goes through the panel so the authored file is the one
    // the panel's own save path produces (flat u0/v0/u1/v1), not a hand-rolled
    // JSON. An existing file is read, never replaced: its authored collision,
    // modes, materials and terrain bits survive.
    //
    // Params:
    //   textureGuid  GUID of the atlas texture asset (required)
    server.RegisterHandler("tileset_create_from_texture",
                           [resolveTileSetEditorPanel](const EditorDebugServer::RequestContext& ctx) -> json
    {
        if (!ctx.params.contains("textureGuid"))
            return Editor::RefuseRequest("textureGuid required");
        const GUID textureGuid(ctx.params["textureGuid"].get<std::string>());
        if (textureGuid.IsNull())
            return Editor::RefuseRequest("textureGuid is not a valid GUID");

        TileSetEditorPanel* panel = resolveTileSetEditorPanel();
        if (!panel)
            return Editor::RefuseRequest("Tile Set Editor panel not instantiated");

        panel->OpenOrCreateForTexture(textureGuid);
        const TileMap::TileSetAsset* tileSet = panel->EditedTileSet();
        if (!tileSet)
            return Editor::RefuseRequest("Tile set was not written (see the panel status line and log)");
        return json{{"ok", true},
                    {"textureGuid", textureGuid.ToString()},
                    {"path", tileSet->GetPath().string()},
                    {"tiles", tileSet->Tiles().size()}};
    });

    // tileset_author_terrain — define a tile set's terrain set and per-tile peering
    // bits, then write the file. The authoring counterpart of clicking the Tile Set
    // Editor's terrain canvas: it edits the panel's own asset and saves through the
    // panel, so the file is the one the panel's save path produces.
    //
    // The terrain set REPLACES whatever the tile set had, at index 0, because
    // peering indices are only meaningful against the set they were authored in.
    //
    // Params:
    //   tileSetGuid  GUID of the .tileset to author (required)
    //   name         terrain set name (optional)
    //   matchMode    cornersAndSides|corners|sides (optional, default cornersAndSides)
    //   terrains     [name, ...] terrains of the set (required, non-empty)
    //   tiles        [{id, center, neighbors:[8]}, ...] peering per tile (required)
    server.RegisterHandler("tileset_author_terrain",
                           [resolveTileSetEditorPanel](const EditorDebugServer::RequestContext& ctx) -> json
                           { return AuthorTileSetTerrain(resolveTileSetEditorPanel(), ctx.params); });

    // tilemap_select — set or clear the marquee selection the paint tool holds.
    // The selection is what turns the bucket into a bounded region fill and what
    // copy/move operate on.
    //
    // Params:
    //   clear          true to drop the selection (all other params ignored)
    //   x0,y0,x1,y1    inclusive corner cells of the selection
    server.RegisterHandler("tilemap_select",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
    {
        auto* tool = ResolvePaintTool(app);
        if (!tool)
            return Editor::RefuseRequest("No tilemap paint tool");

        if (ctx.params.value("clear", false))
        {
            tool->ClearSelection();
            return json{{"ok", true}, {"selection", nullptr}};
        }
        for (const char* key : {"x0", "y0", "x1", "y1"})
        {
            if (!ctx.params.contains(key) || !ctx.params[key].is_number_integer())
                return Editor::RefuseRequest("x0, y0, x1 and y1 are required integers");
        }
        tool->SetSelectionForAutomation(ctx.params["x0"].get<int32>(), ctx.params["y0"].get<int32>(),
                                        ctx.params["x1"].get<int32>(), ctx.params["y1"].get<int32>());
        const auto& selection = tool->Selection();
        return json{{"ok", true},
                    {"selection", {{"minX", selection.MinX}, {"minY", selection.MinY},
                                   {"maxX", selection.MaxX}, {"maxY", selection.MaxY}}}};
    });

    // tilemap_clipboard — copy the selection, or paste it at a cell. Paste records
    // its own undo entry, so a harness can undo a paste independently of the
    // stroke that produced the copied region.
    //
    // Params:
    //   action  "copy" or "paste" (required)
    //   x, y    paste anchor cell (required for paste)
    server.RegisterHandler("tilemap_clipboard",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
    {
        auto* tool = ResolvePaintTool(app);
        if (!tool)
            return Editor::RefuseRequest("No tilemap paint tool");

        const std::string action = ctx.params.value("action", "");
        if (action == "copy")
        {
            if (!tool->CopySelection())
                return Editor::RefuseRequest("Nothing to copy — no selection, or no paint target");
            return json{{"ok", true}, {"action", "copy"}};
        }
        if (action == "paste")
        {
            if (!ctx.params.contains("x") || !ctx.params.contains("y"))
                return Editor::RefuseRequest("paste requires x and y");
            if (!tool->PasteAt(ctx.params["x"].get<int32>(), ctx.params["y"].get<int32>()))
                return Editor::RefuseRequest("Nothing to paste — clipboard empty, or no paint target");
            return json{{"ok", true}, {"action", "paste"}};
        }
        return Editor::RefuseRequest("action must be copy or paste");
    });

    // tilemap_move_selection — move the selected region by a cell delta as one
    // undo entry, the same operation a drag inside the marquee performs.
    //
    // Params: dx, dy (required integers)
    server.RegisterHandler("tilemap_move_selection",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
    {
        auto* tool = ResolvePaintTool(app);
        if (!tool)
            return Editor::RefuseRequest("No tilemap paint tool");
        if (!ctx.params.contains("dx") || !ctx.params.contains("dy"))
            return Editor::RefuseRequest("dx and dy are required");
        if (!tool->MoveSelectionBy(ctx.params["dx"].get<int32>(), ctx.params["dy"].get<int32>()))
            return Editor::RefuseRequest("Move rejected — no selection, no paint target, or zero delta");
        const auto& selection = tool->Selection();
        return json{{"ok", true},
                    {"selection", {{"minX", selection.MinX}, {"minY", selection.MinY},
                                   {"maxX", selection.MaxX}, {"maxY", selection.MaxY}}}};
    });

    // tilemap_fill_rect — fill a cell rectangle with the active stamp, tiled (or
    // scattered), as one undo entry. The Rect tool's gesture without a viewport.
    //
    // Params: x0, y0, x1, y1 (required inclusive corner cells)
    // tilemap_replace_tile — rewrite every cell holding one tile id to another,
    // on the target layer or (scope = "map") on every unlocked layer, as one
    // undo entry. Params: fromTileId (required), toTileId (0 clears), scope.
    server.RegisterHandler("tilemap_replace_tile",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
    {
        auto* editContext = TileMapEditContextOf(app);
        auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
        if (!editContext || !store)
            return Editor::RefuseRequest("No tilemap edit context");
        if (!ctx.params.contains("fromTileId") || !ctx.params["fromTileId"].is_number_integer())
            return Editor::RefuseRequest("fromTileId is required");
        const int64 fromTile = ctx.params["fromTileId"].get<int64>();
        const int64 toTile = ctx.params.value("toTileId", 0);
        if (fromTile <= 0 || fromTile > static_cast<int64>(TileMap::kMaxTileId) || toTile < 0 ||
            toTile > static_cast<int64>(TileMap::kMaxTileId))
            return Editor::RefuseRequest("tile ids must be within the addressable range");
        const GUID mapGuid = editContext->TargetMap();
        const TileMap::TileMapData* map = mapGuid.IsNull() ? nullptr : store->GetMap(mapGuid);
        if (!map)
            return Editor::RefuseRequest(kNoPaintTargetError);
        std::vector<uint32> layers;
        if (ctx.params.value("scope", std::string("layer")) == "map")
        {
            for (const TileMap::TileMapLayerData& layer : map->Layers())
                layers.push_back(layer.Id);
        }
        else if (editContext->TargetLayer() != 0)
        {
            layers.push_back(editContext->TargetLayer());
        }
        size_t cells = 0;
        std::unique_ptr<Editor::TileMapEditCommand> command = Editor::ReplaceTileEverywhere(
            *store, mapGuid, layers, static_cast<uint32>(fromTile), static_cast<uint32>(toTile),
            cells);
        if (command && app.m_UndoRedo)
            app.m_UndoRedo->CommitAlreadyApplied(std::move(command));
        return json{{"ok", true}, {"cells", cells}, {"layers", layers.size()}};
    });

    server.RegisterHandler("tilemap_fill_rect",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
    {
        auto* tool = ResolvePaintTool(app);
        if (!tool)
            return Editor::RefuseRequest("No tilemap paint tool");
        for (const char* key : {"x0", "y0", "x1", "y1"})
        {
            if (!ctx.params.contains(key) || !ctx.params[key].is_number_integer())
                return Editor::RefuseRequest("x0, y0, x1 and y1 are required integers");
        }
        if (!tool->FillRectForAutomation(ctx.params["x0"].get<int32>(), ctx.params["y0"].get<int32>(),
                                         ctx.params["x1"].get<int32>(), ctx.params["y1"].get<int32>()))
        {
            return Editor::RefuseRequest(kNoPaintTargetError);
        }
        return json{{"ok", true}};
    });

    // tilemap_tool_state — what the tilemap authoring surface currently holds.
    // The Scene View only dispatches pointer events and gizmos to the *active*
    // tool, so "is TileMapPaint armed" is the first thing to check when a paint
    // gesture or the selection overlay appears to do nothing.
    server.RegisterHandler("tilemap_tool_state",
                           [&app](const EditorDebugServer::RequestContext&) -> json
    {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view controller");
        auto* controller = app.m_Windows[0]->scene.get();
        auto* tool = Editor::GetTileMapPaintTool(*controller);
        if (!tool)
            return Editor::RefuseRequest("No tilemap paint tool");

        static const char* kToolKindNames[] = {"Selection", "Transform", "NavGridBrush",
                                               "Registered"};
        const auto kind = static_cast<size_t>(controller->GetActiveToolKind());
        json out;
        out["activeToolKind"] =
            kind < std::size(kToolKindNames) ? kToolKindNames[kind] : "Unknown";
        out["activeRegisteredToolId"] = controller->GetActiveRegisteredToolId();
        out["tileMapToolArmed"] =
            controller->GetActiveRegisteredToolId() == Editor::kTileMapPaintToolId;

        const auto& selection = tool->Selection();
        if (selection.Valid)
            out["selection"] = {{"minX", selection.MinX}, {"minY", selection.MinY},
                                {"maxX", selection.MaxX}, {"maxY", selection.MaxY}};
        else
            out["selection"] = nullptr;

        if (auto* editContext = TileMapEditContextOf(app))
        {
            out["stampWidth"] = editContext->Stamp().Width;
            out["stampHeight"] = editContext->Stamp().Height;
            out["scatter"] = editContext->RandomScatter();
            out["targetLayer"] = editContext->TargetLayer();
        }
        return out;
    });

    // tilemap_paint_intgrid — paint semantic values into the selected IntGrid
    // layer as one stroke, driving the same paint tool the viewport drives, so
    // the live rule re-evaluation and the undo boundary are the real ones.
    //
    // Params:
    //   cells [[x,y], ...] cells to paint (required, non-empty)
    //   value 0..255 semantic value to write; 0 erases (required)
    server.RegisterHandler("tilemap_paint_intgrid",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           { return PaintIntGridCells(ResolvePaintTool(app), TileMapEditContextOf(app), ctx.params); });

    // tilemap_author_rules — define an IntGrid layer's values and the rule set a
    // Tiles layer dresses itself with, then re-dress that layer.
    //
    // Rules can come either as an explicit spec or from the terrain wizard, which
    // reads the tileset's phase-4 peering bits. Both paths write through the
    // runtime store, so the renderer picks the result up through its dirty ledger.
    //
    // Params:
    //   intGridLayer  layer id whose value table to replace (optional)
    //   values        [{value,name,colorRGBA}] for that layer (optional)
    //   rulesLayer    Tiles layer id the rules attach to (optional)
    //   sourceLayer   IntGrid layer id the rules read (required with rulesLayer)
    //   seed          rule-set seed (optional, default 0)
    //   clearRules    drop the existing groups first (optional, default true)
    //   groups        [{name, rules:[{pattern:[..], tiles:[..]|stamp:[..], ...}]}]
    //   wizard        {tileSetGuid, terrainSet, terrain, intGridValue, template}
    server.RegisterHandler("tilemap_author_rules",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           { return AuthorTileMapRules(TileMapEditContextOf(app), ctx.params); });

    // tilemap_rules_state — what a map's layers hold: IntGrid values, rule set
    // shape, and optionally a window of cells with their provenance. The first
    // check when a dressed layer looks wrong.
    //
    // Params: mapGuid (optional), window {x0,y0,x1,y1} and layer (optional)
    server.RegisterHandler("tilemap_rules_state",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           { return DescribeTileMapRules(TileMapEditContextOf(app), ctx.params); });

    // tilemap_import — run the same Tiled/LDtk import the Assets browser's
    // "Import as Tilemap" context action runs: parse the source, write
    // .tileset/.tilemap assets beside it, register them with the asset
    // registry. The scripted-verification entry point for the importers.
    //
    // Params:
    //   path       absolute path of a .tmx or .ldtk file (required)
    //   overwrite  true to replace .tileset/.tilemap files that already exist
    //              (default false: the request is refused, names them under
    //              details.existing, and writes nothing)
    server.RegisterHandler("tilemap_import",
                           [](const EditorDebugServer::RequestContext& ctx) -> json
    {
        if (!ctx.params.contains("path") || !ctx.params["path"].is_string())
            return Editor::RefuseRequest("path required");
        const std::filesystem::path sourcePath(ctx.params["path"].get<std::string>());
        if (!Editor::IsTileMapSourceFile(sourcePath))
            return Editor::RefuseRequest("path must name a .tmx or .ldtk file");

        if (ctx.params.contains("overwrite") && !ctx.params["overwrite"].is_boolean())
            return Editor::RefuseRequest("overwrite must be true or false");
        const bool overwrite = ctx.params.value("overwrite", false);

        std::string error;
        Editor::TileMapImportSummary summary;
        if (!Editor::ImportTileMapSource(sourcePath, overwrite, error, &summary))
        {
            json existing = json::array();
            for (const auto& path : summary.ExistingOutputs)
            {
                const std::u8string text = path.generic_u8string();
                existing.push_back(std::string(reinterpret_cast<const char*>(text.data()), text.size()));
            }
            return Editor::RefuseRequest(error, json{{"existing", std::move(existing)}});
        }

        json tileSets = json::array();
        for (const auto& path : summary.TileSetPaths)
            tileSets.push_back(path.string());
        json maps = json::array();
        for (const auto& path : summary.MapPaths)
            maps.push_back(path.string());
        return json{{"ok", true},
                    {"tileSets", std::move(tileSets)},
                    {"maps", std::move(maps)},
                    {"warnings", summary.Warnings}};
    });
}

} // namespace GameEngine
