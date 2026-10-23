#include "DebugServer/DebugHandlers.h"

#include "DebugServer/AssetDebugHandlers.h"
#include "DebugServer/AssistantSessionHandlers.h"
#include "DebugServer/BenchSceneHandlers.h"
#include "DebugServer/BuildDebugHandlers.h"
#include "DebugServer/CameraDebugHandlers.h"
#include "DebugServer/CaptureDebugHandlers.h"
#include "DebugServer/CrashDebugHandlers.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/DisplayOutputDebugHandlers.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/EditorStateDebugHandlers.h"
#include "DebugServer/ElementPointerTarget.h"
#include "DebugServer/FramePhaseDebugHandlers.h"
#include "DebugServer/GpuToolingHandlers.h"
#include "DebugServer/GraphDebugHandlers.h"
#include "DebugServer/GroundQueryDebugHandlers.h"
#include "DebugServer/JobSystemDebugHandlers.h"
#include "DebugServer/MarkupDebugHandlers.h"
#include "DebugServer/MemoryDebugHandlers.h"
#include "DebugServer/MeshLodDebugHandlers.h"
#include "DebugServer/ModelSpawnDebugHandlers.h"
#include "DebugServer/MovieRecordDebugHandlers.h"
#include "DebugServer/NavigationDebugHandlers.h"
#include "DebugServer/OpenAssetPath.h"
#include "DebugServer/ParallaxDebugHandlers.h"
#include "DebugServer/PerformDropRequest.h"
#include "DebugServer/PhysicsDebugHandlers.h"
#include "DebugServer/PointerCoordsRequest.h"
#include "DebugServer/ProfilerDebugHandlers.h"
#include "DebugServer/RenderGraphDebugHandlers.h"
#include "DebugServer/RenderStatsDebugHandlers.h"
#include "DebugServer/SceneEntityDebugHandlers.h"
#include "DebugServer/SceneLifecycleDebugHandlers.h"
#include "DebugServer/SceneToolDebugHandlers.h"
#include "DebugServer/ShadowDebugHandlers.h"
#include "DebugServer/SkyDebugHandlers.h"
#include "DebugServer/TerrainDebugHandlers.h"
#include "DebugServer/TileMapDebugHandlers.h"
#include "DebugServer/UiAutomationDebugHandlers.h"
#include "DebugServer/UndoDebugHandlers.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "EditorApplication.h"
#include "EditorPanelIds.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"
#include "Panels/SceneViewPanel.h"
#include "SceneViewController.h"
#include "UI/Controls/Mount.h"
#include "UI/Layout/Docking.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

bool ApplyMainSceneViewPose(EditorApplication& app, const SceneViewCameraPose& pose)
{
    if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
        return false;
    app.m_Windows[0]->scene->SetCameraPose(pose);

    // Push the new angles into the SceneViewPanel — see set_camera.
    if (auto* root = app.m_Windows[0]->ui->GetRootElement())
        if (auto* mount = dynamic_cast<Mount*>(root->FindById(HashStringId(EditorPanelIds::MountSceneView))))
            if (auto* svPanel = dynamic_cast<SceneViewPanel*>(mount->GetTarget()))
                svPanel->SetYawPitch(pose.YawDeg, pose.PitchDeg);
    return true;
}

void RegisterDebugHandlers(EditorDebugServer& server,
                           EditorApplication& app,
                           Logger::RingBufferSink* ringBufferSink)
{
    // 1-2, 14-15. editor state, window placement, log, scroll, undock, named commands, view focus, shutdown
    RegisterEditorStateDebugHandlers(server, app, ringBufferSink);

    // 3-5, 8-11. scene hierarchy, entity components, ECS queries, create/set/enable/delete/select entity
    RegisterSceneEntityDebugHandlers(server, app);

    // open_scene, respond_modal, save_scene and set_play_mode
    RegisterSceneLifecycleDebugHandlers(server, app);

    // 4b. terrain-specific diagnostics + sculpt stroke driver
    RegisterTerrainDebugHandlers(server, app);

    // 4c. renderer scale-bench spawner (Tools/Benchmarks/Rendering/perf-bench)
    RegisterBenchSceneHandlers(server);

    // 4d. physics world queries (the collision oracle for automated checks)
    RegisterPhysicsDebugHandlers(server);

    // tile map painting, selection, rules and import
    RegisterTileMapDebugHandlers(server, app);

    // 4e. composed-ground queries (the ground oracle for placement checks)
    RegisterGroundQueryDebugHandlers(server);

    // 4e2. the sky's per-frame inputs (the readback for comparing two runs input by input)
    RegisterSkyDebugHandlers(server);

    // 4e3. the Parallax steps debug view (height samples per pixel of each relief march)
    RegisterParallaxDebugHandlers(server);

    // 4e3b. world mark-ups: the agent's side of the visual conversation
    RegisterMarkupDebugHandlers(server, app);

    // 4f. GPU vendor, which graphics tools are attached to the live device, and
    //     Nsight Graphics capture triggering
    RegisterGpuToolingHandlers(server, app);

    // 4g. an assistant session's binding and its permission for actions outside the
    //     editor, answered by the gate that claims them
    RegisterAssistantSessionHandlers(server);

    // 4h. the engine job system: what every pool thread runs, lane and channel backlogs
    RegisterJobSystemDebugHandlers(server);

    // 4i. the editor's per-frame phase history: which phase each recent frame spent its time in
    RegisterFramePhaseDebugHandlers(server, app.GetFramePhaseHistory(), app);

    // 4j. navigation: what the path solve budget solved, queued and left waiting
    RegisterNavigationDebugHandlers(server);

    // 4k. the Memory module's stats registry: arenas, tracked bytes, each thread's scratch
    RegisterMemoryDebugHandlers(server, app);

    // 6-7. UI tree dumps, pointer/click/drag/key/text injection, context-menu automation
    RegisterUiAutomationDebugHandlers(server, app);

    // perform_drop
    // Drops asset files at a point in a window, through that window's drag-and-drop manager
    // (PerformAssetFileDrop): the drop target under the point runs its own CanDrop and
    // PerformDrop, exactly as for a drop made with the mouse, without a pointer gesture to
    // start the drag. Files outside the asset sources are refused, never moved in.
    server.RegisterHandler("perform_drop", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui || !wndCtx->ui->GetRootElement())
            return Editor::RefuseRequest("No UI manager available");

        if (!ctx.params.contains("paths") || !ctx.params["paths"].is_array() || ctx.params["paths"].empty())
            return Editor::RefuseRequest("Provide paths: an array of asset paths, absolute or project-relative");
        const AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
        std::vector<std::filesystem::path> paths;
        for (const json& entry : ctx.params["paths"])
        {
            if (!entry.is_string())
                return Editor::RefuseRequest("paths must hold strings");
            const std::string requested = entry.get<std::string>();
            const std::filesystem::path resolved = Editor::ResolveOpenAssetPath(assetManager, requested);
            if (resolved.empty())
                return Editor::RefuseRequest("Cannot resolve asset path: " + requested);
            paths.push_back(resolved);
        }
        std::vector<std::filesystem::path> sourceRoots;
        for (const AssetSourceDesc& source : assetManager.GetRegisteredSources())
            sourceRoots.push_back(source.Root);

        float x = 0.0f;
        float y = 0.0f;
        if (ctx.params.contains("elementId"))
        {
            if (!ctx.params["elementId"].is_string())
                return Editor::RefuseRequest("elementId must be a string: an element id from get_ui_tree");
            json targetError;
            if (!Editor::ResolveElementPointerTarget(wndCtx->ui->GetRootElement(),
                                                     ctx.params["elementId"].get<std::string>(), x, y, targetError))
                return targetError;
        }
        else if (ctx.params.contains("x") && ctx.params.contains("y"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "x", "y", x, y, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId or x/y");
        }

        const Editor::DropOutcome outcome = Editor::PerformAssetFileDrop(*wndCtx->ui, paths, sourceRoots, x, y);
        if (!outcome.Refusal.empty())
            return Editor::RefuseRequest(outcome.Refusal);
        json pathsOut = json::array();
        for (const auto& path : paths)
            pathsOut.push_back(path.lexically_normal().string());
        // "accepted": the target took the payload and its PerformDrop ran; PerformDrop reports
        // no result, so whether the drop did what it should is read from its effect.
        return json{{"accepted", outcome.Accepted}, {"reason", outcome.Reason},
                    {"target", outcome.TargetElement}, {"x", x}, {"y", y}, {"paths", pathsOut}}; });

    // 8b-8e. spawn_model, spawn_humanoid_test_pair, export_model_materials, enumerate_provenance
    RegisterModelSpawnDebugHandlers(server, app);

    // 10b-10c. undo / redo and the undo-stack dump
    RegisterUndoDebugHandlers(server, app);

    // 12-12b. validation stats, render stats, UI clip stats, UI update-profile history
    RegisterRenderStatsDebugHandlers(server, app);

    // 13, 26-27. screenshots, shadow diagnostics, render-graph resource capture, RenderDoc capture and pass descriptors
    RegisterCaptureDebugHandlers(server, app);

    // import_proflares, open_asset, Assets panel selection and preview, asset sources and
    // listings, skeleton store state
    RegisterAssetDebugHandlers(server, app);

    // graph panel: viewport, node listing and node deletion
    RegisterGraphDebugHandlers(server, app);

    // movie_record: Game View movie capture start/stop/status
    RegisterMovieRecordDebugHandlers(server, app);

    // 16-17. scene tool, terrain brush and gizmo visibility
    RegisterSceneToolDebugHandlers(server, app);

    // 16-21. render-graph introspection: overview, passes, resources, pass detail, dependencies, validation
    RegisterRenderGraphDebugHandlers(server, app);

    // Probe diagnostics are available without opening an inspector, allowing
    // capture publication, work budgets and static-edit refreshes to be tested.
    server.RegisterHandler("get_reflection_probe_state", [](const EditorDebugServer::RequestContext&) -> json {
        using Ibl = Engine::Renderer::ImageBasedLightingFeature;
        using Source = Engine::Renderer::SceneReflectionProbeEnvironmentSource;
        auto& engine = EngineCore::GetInstance();
        auto* world = engine.GetPrimaryWorld();
        auto* services = engine.GetRenderServices();
        auto* ibl = services ? services->GetFeature<Ibl>() : nullptr;
        json probes = json::array();
        if (world && ibl)
            for (uint32_t i = 0; i < Ibl::kMaxLocalProbes; ++i)
            {
                const auto* capture = ibl->GetLocalProbeCapture(world->GetWorldId(), i);
                const auto* desc = ibl->GetLocalProbeDesc(world->GetWorldId(), i);
                if (!capture || !desc) continue;
                const auto* source = static_cast<const Source*>(capture->GetEnvironmentSource());
                const auto d = source->GetDiagnostics();
                probes.push_back({
                    {"slot", i}, {"entityId", desc->SourceKey & 0xffffffffu},
                    {"sourceKey", std::to_string(desc->SourceKey)}, {"priority", desc->Priority},
                    {"ready", capture->IsInitialized() && capture->GetLastBakedDigest() != 0},
                    {"baking", source->IsBaking()}, {"needsRefresh", source->NeedsBake()},
                    {"stage", d.Stage}, {"step", d.Step}, {"completedBakes", d.CompletedBakes},
                    {"ageSeconds", d.AgeSeconds}, {"waitSeconds", d.WaitSeconds},
                    {"capturePrepareCpuMs", d.CaptureRecordMs}, {"filterRecordCpuMs", d.FilterRecordMs},
                    {"lastFaceGpuSpanMs", d.LastCaptureGpuSpanMs}, {"lastFilterGpuSpanMs", d.LastFilterGpuSpanMs},
                    {"gpuSpansShared", d.GpuSpansShared},
                    {"inputDigest", std::to_string(source->InputDigest())},
                    {"bakedDigest", std::to_string(capture->GetLastBakedDigest())},
                    {"captureResolution", capture->GetEnvCaptureSize()}, {"prefilterMaxMip", capture->GetPrefilterMaxMip()},
                    {"capturePosition", {source->GetCapturedPosition()[0], source->GetCapturedPosition()[1], source->GetCapturedPosition()[2]}},
                    {"requestedPosition", {desc->Position[0], desc->Position[1], desc->Position[2]}},
                    {"influenceShape", desc->InfluenceShape}, {"boxProjection", desc->BoxProjection}});
            }
        return json{{"worldId", world ? world->GetWorldId() : 0}, {"capacity", Ibl::kMaxLocalProbes},
            {"globalWorkUnitsPerFrame", 1}, {"workUnit", "one capture face or one filtering dispatch"}, {"probes", probes}};
    });
    server.RegisterHandler("rebake_reflection_probes", [](const EditorDebugServer::RequestContext& ctx) -> json {
        using Ibl = Engine::Renderer::ImageBasedLightingFeature;
        auto& engine = EngineCore::GetInstance();
        auto* world = engine.GetPrimaryWorld();
        auto* services = engine.GetRenderServices();
        auto* ibl = services ? services->GetFeature<Ibl>() : nullptr;
        if (!world || !ibl) return json{{"queued", 0}};
        const uint64_t entityId = ctx.params.value("entityId", uint64_t(0));
        uint32_t count = 0;
        for (uint32_t i = 0; i < Ibl::kMaxLocalProbes; ++i)
            if (const auto* desc = ibl->GetLocalProbeDesc(world->GetWorldId(), i);
                desc && (entityId == 0 || (desc->SourceKey & 0xffffffffu) == entityId))
            {
                ibl->RequestLocalProbeRebake(world->GetWorldId(), desc->SourceKey);
                ++count;
            }
        return json{{"queued", count}};
    });

    // 22-25. scene view camera: get_camera, set_scene_view_mode, set_camera, look_at, move_camera
    RegisterCameraDebugHandlers(server, app);

    // 28-30d. shadow debug views, thumbnails, filter quality, PCSS and MSM knobs
    RegisterShadowDebugHandlers(server, app);

    // 29a2-29b. mesh GPU registry dump and GPU LOD selection knobs
    RegisterMeshLodDebugHandlers(server);

    // player builds: trigger, status and the resolved build settings
    RegisterBuildDebugHandlers(server, app);

    // CPU profiler, GPU render-graph pass timings and DebugMetrics monitors
    RegisterProfilerDebugHandlers(server, app);

    // HDR output, MSAA, anti-aliasing mode, TAA render scale and dynamic resolution
    RegisterDisplayOutputDebugHandlers(server, app);

    // debug_crash: deliberate crash through a chosen path
    RegisterCrashDebugHandlers(server);
}

} // namespace GameEngine
