#include "DebugServer/MeshLodDebugHandlers.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"

#include "Assets/MeshLODGenerator.h"
#include "Core/Engine.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/GPUScene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace GameEngine
{

using json = nlohmann::json;

void RegisterMeshLodDebugHandlers(EditorDebugServer& server)
{
    // 29a2. get_mesh_registry — dump every live MeshGPURegistry entry with its
    // source identity (asset GUID + submesh) and LOD chain. This is the mapping
    // a GPU capture cannot recover on its own: GPUScene mesh-row index -> asset,
    // per-LOD index counts, achieved simplify errors, sloppy flags, and the
    // derived screen-coverage switch points the scatter consumes. Read-only.
    server.RegisterHandler("get_mesh_registry", [](const EditorDebugServer::RequestContext&) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");
        const auto& reg = rs->GetMeshGPURegistry();
        // Report the thresholds the scatter actually reads — the uploaded
        // GPUMesh row — rather than re-deriving them here. A second derivation
        // silently stops matching the moment the upload path gains a step.
        const Rendering::GPUScene* scene = reg.GetGPUScene();
        json rows = json::array();
        reg.ForEachKeyedEntry(
            [&rows, scene](const Rendering::MeshGPUKey& key, const Rendering::MeshGPUEntry& e)
            {
                const Rendering::GPUMesh* row = nullptr;
                if (scene && e.gpuMeshIndex != ~0u && e.gpuMeshIndex < scene->GetMeshes().size())
                    row = &scene->GetMeshes()[e.gpuMeshIndex];
                json lods = json::array();
                for (uint32_t k = 0; k < e.lodCount && k < Rendering::MeshGPUEntry::kMaxEntryLODs; ++k)
                {
                    json lod{{"indexCount", e.lodIndexCount[k]},
                             {"firstIndex", e.lodFirstIndex[k]},
                             {"error", e.lodError[k]},
                             {"sloppy", e.lodSloppy[k] != 0}};
                    // Absent when the entry holds no GPU row (nothing uploaded yet).
                    if (row && k < Rendering::kMaxMeshLODs)
                    {
                        lod["threshold"] = row->lodThreshold[k];
                        // SSE-normalized slot (MeshLODThresholds.h): threshold
                        // compares against sse-scaled coverage, and the switch
                        // happens at spherePx = 2 * budgetPx * threshold.
                        lod["sse"] = (row->lodFlags & (1u << k)) != 0u;
                    }
                    lods.push_back(std::move(lod));
                }
                // boundingRadius/maxExtent describe the CULLING envelope, which
                // encloses every admitted LOD. The switch points above are built
                // from the LOD0 reference metric instead, so both are reported:
                // reading a threshold against the envelope would not reproduce it.
                json entry{{"guid", key.assetGuid.ToString()},
                           {"submesh", key.submeshIndex},
                           {"lodCount", e.lodCount},
                           {"authored", e.lodAuthored},
                           {"indexCount", e.indexCount},
                           {"boundingRadius", e.bounds.Radius()},
                           {"maxExtent", 2.0f * std::max({e.bounds.halfExtents.x,
                                                          e.bounds.halfExtents.y,
                                                          e.bounds.halfExtents.z})},
                           {"lodReferenceRadius", e.lodReferenceRadius},
                           {"lodReferenceMaxExtent", e.lodReferenceMaxExtent},
                           {"lods", std::move(lods)}};
                if (row)
                {
                    entry["gpuMeshIndex"] = e.gpuMeshIndex;
                    entry["lodFlags"] = row->lodFlags;
                    // The scatter multiplies its coverage by this to undo the
                    // envelope; 1 on every mesh whose bounds never grew.
                    entry["lodCoverageScale"] = row->lodCoverageScale;
                    entry["tightClass"] =
                        (row->lodFlags & Rendering::kGPUMeshLodTightClassBit) != 0u;
                }
                rows.push_back(std::move(entry));
            });
        // Key iteration order is arbitrary (unordered_map); sort so successive
        // dumps of the same scene diff cleanly.
        std::sort(rows.begin(), rows.end(),
                  [](const json& a, const json& b)
                  {
                      const auto& ga = a.at("guid").get_ref<const std::string&>();
                      const auto& gb = b.at("guid").get_ref<const std::string&>();
                      if (ga != gb)
                          return ga < gb;
                      return a.at("submesh").get<uint32_t>() < b.at("submesh").get<uint32_t>();
                  });
        return json{{"entryCount", rows.size()}, {"entries", std::move(rows)}}; });

    // 29b. set_lod — drive the GPU LOD selection knobs for diagnosing coverage
    // thresholds and simplified-mesh seams.
    // {forceLevel?: int, bias?: float, shadowBias?: float, cullCoverage?: float,
    //  budgetPx?: float, skinnedScale?: float, crossfadeDuration?: float,
    //  mode?: string}.
    //   forceLevel:  0..3 pins every instance to that LOD; -1 (or omit) restores
    //                screen-coverage auto-selection.
    //   bias:        log2 coverage bias; positive keeps more detail, negative
    //                switches to coarser LODs earlier. Applies to the auto path.
    //   shadowBias:  extra log2 bias added on shadow buckets only (cascades,
    //                spot/point/area), so shadow maps can run coarser levels
    //                than the view that casts them. Camera slices — including
    //                the depth prepass — never take it: prepass geometry must
    //                stay identical to the forward pass it primes.
    //                TRAP: the bias multiplies COVERAGE, not the threshold
    //                (draw_command_scatter.comp ge_SelectLOD), so a negative
    //                value scales every switch point by 2^-bias — including
    //                kLodSloppyThresholdCap (MeshLODThresholds.h), the floor
    //                that holds sloppy levels back. Sloppy levels drop whole
    //                regions instead of eroding evenly, and submeshes simplify
    //                independently (ModelAsset::GenerateLODs), so admitting
    //                them earlier in shadow reads as missing caster geometry
    //                and as light leaking between the submeshes of a solid
    //                object. Judge a negative value on the shadow, not on the
    //                lit image the coverage thresholds were tuned against.
    //   cullCoverage: small-object cull threshold (prototype; 0 disables) —
    //                camera-slice instances below this projected coverage drop.
    //   budgetPx:    SSE projected-error budget in render-target pixels
    //                (generated chains); <= 0 disables SSE coarsening.
    //   skinnedScale: budget multiplier for skinned/character chains.
    //   gameViewBudget / sceneViewBudget: per-view-class override of budgetPx,
    //                {"enabled": bool, "percent": 10..400}. Omitted sub-keys keep
    //                their current value. Disabled = that class spends budgetPx
    //                unchanged. Scene View covers every scene viewport.
    //   crossfadeDuration: dithered LOD crossfade length in seconds (0
    //                disables). A camera-slice level change dissolves over
    //                this window instead of popping. Shadow buckets never take
    //                it, for the same reason they never take cullCoverage.
    //                Prepass-safe: a fading instance writes its record PAIR to
    //                the row's TAIL region and no head record, and BOTH raster
    //                passes draw that tail through the same dither, so the
    //                prepass writes depth for exactly the fragments the colour
    //                pass keeps — each level on its own half, neither writing
    //                where the other does.
    //                NOTE: a non-zero duration makes a frame's records depend on
    //                the fade CLOCK, so captures stop being reproducible from
    //                pose alone — set 0 before any A/B that assumes they are.
    //   mode:        selection mapping — "sse", "coverage" (the pre-SSE
    //                reference) or "off" (every mesh draws LOD0). Validated
    //                before any knob is applied, so a typo changes nothing.
    //   hysteresis:  LOD dwell band as a fraction of the switch coverage (0
    //                disables). Gaining detail needs coverage >= threshold*(1+band);
    //                holding or losing it needs only the threshold. Camera slices
    //                only. NOTE: a non-zero band makes selection depend on the
    //                previous frame, so captures stop being reproducible from
    //                pose alone — set 0 before any A/B that assumes they are.
    // Returns the resolved knobs so a caller can A/B a fixed pose across levels.
    server.RegisterHandler("set_lod", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");

        // Parsed up front: a rejected mode must not leave the other knobs
        // half-applied under an ok:false reply.
        std::optional<Rendering::LodSelectionMode> requestedMode;
        if (ctx.params.contains("mode"))
        {
            Rendering::LodSelectionMode parsed{};
            const std::string mode = ctx.params["mode"].get<std::string>();
            if (!Rendering::TryParseLodSelectionMode(mode, parsed))
                return Editor::RefuseRequest("mode must be \"off\", \"coverage\" or \"sse\"");
            requestedMode = parsed;
        }

        constexpr uint32_t kAutoLevel = 0xFFFFFFFFu;
        if (ctx.params.contains("forceLevel"))
        {
            int level = ctx.params["forceLevel"].get<int>();
            rs->SetLODForceLevel(level < 0 ? kAutoLevel
                                           : static_cast<uint32_t>(std::min(
                                                 level, static_cast<int>(MeshLODConfig::kMaxLODs) - 1)));
        }
        if (ctx.params.contains("bias"))
            rs->SetLODGlobalBias(ctx.params["bias"].get<float>());
        if (ctx.params.contains("shadowBias"))
            rs->SetShadowLODBias(ctx.params["shadowBias"].get<float>());
        if (ctx.params.contains("cullCoverage"))
            rs->SetSmallObjectCullCoverage(
                std::max(0.0f, ctx.params["cullCoverage"].get<float>()));
        // SSE-budget knobs (MeshLODThresholds.h). budgetPx <= 0 disables SSE
        // coarsening (keep-detail fail-safe); skinnedScale multiplies the
        // budget for skinned/character chains. Runtime-only, like every knob
        // here: these write the same RenderServices/registry state the project
        // settings (rendering.lodErrorBudgetPx / lodSkinnedBudgetScale /
        // lodMode) seed at startup, but nothing here is written back to the
        // project, so a restart returns to the persisted values.
        if (ctx.params.contains("budgetPx"))
            rs->SetLODErrorBudgetPx(ctx.params["budgetPx"].get<float>());
        if (ctx.params.contains("skinnedScale"))
            rs->SetLODSkinnedBudgetScale(ctx.params["skinnedScale"].get<float>());
        // Per-view-class budget overrides, keyed by ViewPurpose. Each accepts
        // {"enabled": bool, "percent": float}; a missing sub-key keeps the
        // current value, so a lane can flip "enabled" without restating the
        // percent. Only the two classes the settings page exposes are settable:
        // preview/thumbnail views frame each model to fill the view, so they sit
        // at LOD0 whatever the budget.
        auto applyViewBudget = [&ctx, rs](const char* key, Rendering::ViewPurpose purpose)
        {
            if (!ctx.params.contains(key) || !ctx.params[key].is_object())
                return;
            const auto& node = ctx.params[key];
            Rendering::LodViewBudgetOverride budget = rs->GetLODViewBudgetOverride(purpose);
            if (node.contains("enabled") && node["enabled"].is_boolean())
                budget.Enabled = node["enabled"].get<bool>();
            if (node.contains("percent") && node["percent"].is_number())
                budget.BudgetPercent = std::clamp(node["percent"].get<float>(),
                                                  Rendering::kMinLodBudgetPercent,
                                                  Rendering::kMaxLodBudgetPercent);
            rs->SetLODViewBudgetOverride(purpose, budget);
        };
        applyViewBudget("gameViewBudget", Rendering::ViewPurpose::Game);
        applyViewBudget("sceneViewBudget", Rendering::ViewPurpose::EditorScene);
        if (ctx.params.contains("crossfadeDuration"))
            rs->SetLODCrossfadeDuration(ctx.params["crossfadeDuration"].get<float>());
        // Switching the mapping re-derives every registered GPUMesh row in
        // place, so both arms of an A/B run in one process at one GPU clock
        // state. rowsRewritten is reported because a silent 0 would mean the
        // arm never actually changed.
        size_t lodRowsRewritten = 0;
        if (requestedMode.has_value())
            lodRowsRewritten = rs->GetMeshGPURegistry().SetLodSelectionMode(*requestedMode);
        if (ctx.params.contains("hysteresis"))
            rs->SetLODHysteresisBand(ctx.params["hysteresis"].get<float>());

        const uint32_t force = rs->GetLODForceLevel();
        const float globalBudgetPx = rs->GetLODErrorBudgetPx();
        auto viewBudgetJson = [rs, globalBudgetPx](Rendering::ViewPurpose purpose)
        {
            const Rendering::LodViewBudgetOverride budget = rs->GetLODViewBudgetOverride(purpose);
            return json{{"enabled", budget.Enabled},
                        {"percent", budget.BudgetPercent},
                        {"effectiveBudgetPx",
                         Rendering::ResolveLodBudgetPx(globalBudgetPx, budget)}};
        };
        return json{
            {"ok", true},
            {"forceLevel", force == kAutoLevel ? -1 : static_cast<int>(force)},
            {"bias", rs->GetLODGlobalBias()},
            {"shadowBias", rs->GetShadowLODBias()},
            {"cullCoverage", rs->GetSmallObjectCullCoverage()},
            {"budgetPx", rs->GetLODErrorBudgetPx()},
            {"skinnedScale", rs->GetLODSkinnedBudgetScale()},
            {"crossfadeDuration", rs->GetLODCrossfadeDuration()},
            {"mode", std::string(Rendering::ToString(
                         rs->GetMeshGPURegistry().GetLodSelectionMode()))},
            {"rowsRewritten", lodRowsRewritten},
            // Per-view-class overrides, with the budget each class actually
            // spends resolved for the reader — a disabled override reports the
            // global budget, which is what its views really select against.
            {"gameViewBudget", viewBudgetJson(Rendering::ViewPurpose::Game)},
            {"sceneViewBudget", viewBudgetJson(Rendering::ViewPurpose::EditorScene)},
            {"hysteresis", rs->GetLODHysteresisBand()}
        }; });
}

} // namespace GameEngine
