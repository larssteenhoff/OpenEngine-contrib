#include "DebugServer/ModelSpawnDebugHandlers.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"

#include "AssetCore/GUID.h"
#include "AssetDatabase/IAssetDbCache.h"
#include "Assets/AssetCreation.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Editor/Settings/FbxImportSettings.h"
#include "EditorApplication.h"
#include "EditorChangeNotifications.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/RenderServices.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

void RegisterModelSpawnDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    // 8b. spawn_model — instantiate a ModelAsset (FBX/glTF) into the scene
    // and optionally attach an AnimationClip to its skinned entities. Wraps
    // ModelEntityFactory so AI agents can drive the same flow as a manual
    // FBX drop into the scene view. Uses the deferred response pattern so
    // async asset loads don't deadlock the IPC main-thread.
    server.RegisterHandler("spawn_model", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        const std::string modelPath = ctx.params.value("path", std::string{});
        if (modelPath.empty())
            return Editor::RefuseRequest("Missing 'path' (e.g. 'Models/BusinessMale.fbx')");

        auto& am = EngineCore::GetInstance().GetAssetManager();
        const GUID modelGuid = am.ResolveAssetGuid(modelPath);
        if (modelGuid.IsNull())
            return Editor::RefuseRequest("Could not resolve model path: " + modelPath);

        const std::string clipPath = ctx.params.value("clipPath", std::string{});
        GUID clipGuid;
        if (!clipPath.empty())
        {
            clipGuid = am.ResolveAssetGuid(clipPath);
            if (clipGuid.IsNull())
                return Editor::RefuseRequest("Could not resolve clip path: " + clipPath);
        }

        // Kick async load(s) and return DeferredMarker so the main loop can
        // service the loader between IPC poll cycles.
        struct SpawnState
        {
            GUID modelGuid;
            GUID clipGuid;
            std::string rootName;
            bool hasPosition = false;
            float px = 0, py = 0, pz = 0;
            bool loop = true, paused = false;
            float speed = 1.0f;
            bool kicked = false;
            int polls = 0;
        };
        auto state = std::make_shared<SpawnState>();
        state->modelGuid = modelGuid;
        state->clipGuid = clipGuid;
        state->rootName = ctx.params.value("name", std::filesystem::path(modelPath).stem().string());
        if (ctx.params.contains("position") && ctx.params["position"].is_object())
        {
            const auto& p = ctx.params["position"];
            state->hasPosition = true;
            state->px = p.value("x", 0.0f);
            state->py = p.value("y", 0.0f);
            state->pz = p.value("z", 0.0f);
        }
        state->loop   = ctx.params.value("loop", true);
        state->paused = ctx.params.value("paused", false);
        state->speed  = ctx.params.value("speed", 1.0f);

        server.EnqueueDeferredResponse(ctx.id,
            [&app, state](json& outResult) -> bool
            {
                auto& am2 = EngineCore::GetInstance().GetAssetManager();
                if (!state->kicked)
                {
                    if (!am2.GetAsset(state->modelGuid))
                        (void)am2.LoadAssetAsync(state->modelGuid, AssetLoadPriority::High);
                    state->kicked = true;
                }

                SharedPtr<Asset> modelAsset = am2.GetAsset(state->modelGuid);
                if (!modelAsset)
                {
                    // Cap waiting at ~10 seconds (600 polls @ 60fps).
                    if (++state->polls > 600)
                    {
                        outResult = Editor::RefuseRequest("ModelAsset load timed out");
                        return true;
                    }
                    return false; // still loading; poll next frame
                }

                auto* modelPtr = dynamic_cast<ModelAsset*>(modelAsset.get());
                if (!modelPtr || !modelPtr->IsLoaded())
                {
                    outResult = Editor::RefuseRequest("Failed to load ModelAsset");
                    return true;
                }

                auto* world2 = EngineCore::GetInstance().GetPrimaryWorld();
                auto* rs = EngineCore::GetInstance().GetRenderServices();
                if (!world2 || !rs)
                {
                    outResult = Editor::RefuseRequest("World or RenderServices unavailable");
                    return true;
                }

                auto created = Engine::Renderer::ModelEntityFactory::CreateFromModel(
                    *rs, *world2, *modelPtr, state->modelGuid, state->rootName, Editor::GetFbxModelEntityFactoryOptions());
                if (!created.IsValid())
                {
                    outResult = Editor::RefuseRequest("ModelEntityFactory failed");
                    return true;
                }

                if (state->hasPosition)
                {
                    if (auto* xf = world2->GetComponent<Components::Transform>(created.rootEntity))
                    {
                        Components::Transform updated = *xf;
                        updated.matrix[12] = state->px;
                        updated.matrix[13] = state->py;
                        updated.matrix[14] = state->pz;
                        world2->AddComponentImmediate(created.rootEntity, updated);
                    }
                }

                uint32_t clipIndex = 0;
                if (!state->clipGuid.IsNull())
                {
                    clipIndex = Engine::Renderer::ClipStore::Instance().GetOrLoadClipIndex(state->clipGuid, am2);
                    if (clipIndex != 0)
                    {
                        for (auto e : created.submeshEntities)
                        {
                            Components::AnimatorRef anim{};
                            anim.ClipIndex = clipIndex;
                            anim.Time = 0.0f;
                            anim.Speed = state->speed;
                            anim.Flags = (state->loop ? Components::AnimatorRef::kFlag_Loop : 0u)
                                       | (state->paused ? Components::AnimatorRef::kFlag_Paused : 0u);
                            world2->AddComponentImmediate(e, anim);
                        }
                    }
                }

                if (app.m_ChangeNotifications)
                {
                    Editor::EditorChangeNotifications::WorldStructureChangedEvent ev{};
                    ev.world = world2;
                    ev.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
                    app.m_ChangeNotifications->NotifyWorldStructureChanged(ev);
                }

                json submeshIds = json::array();
                for (auto e : created.submeshEntities)
                    submeshIds.push_back(e.id);
                outResult = json{
                    {"rootEntityId", created.rootEntity.id},
                    {"submeshEntityIds", std::move(submeshIds)},
                    {"skinned", created.skinned},
                    {"clipIndex", clipIndex},
                };
                return true;
            });

        return EditorDebugServer::DeferredMarker(); });

    // 8c. spawn_humanoid_test_pair — synchronous, main-thread spawn of a Model
    // entity (with optional AnimationClip wired into AnimatorRef on every
    // skinned submesh). Bypasses the deferred-response path of `spawn_model`
    // which times out before first-time FBX import completes. Loads assets
    // synchronously via LoadAssetAsync(...).get() (same pattern used by
    // ModelRenderSetup / RenderServices). Intended for autonomous test
    // scaffolding (humanoid retargeting scenarios).
    server.RegisterHandler("spawn_humanoid_test_pair", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices available");

        const std::string modelPath = ctx.params.value("modelPath", std::string{});
        if (modelPath.empty())
            return Editor::RefuseRequest("Missing 'modelPath' (e.g. 'Models/BusinessMale.fbx')");

        auto& am = EngineCore::GetInstance().GetAssetManager();
        const GUID modelGuid = am.ResolveAssetGuid(modelPath);
        if (modelGuid.IsNull())
            return Editor::RefuseRequest("Could not resolve modelPath: " + modelPath);

        // Synchronous load on the main thread. Same pattern used elsewhere in
        // the engine (ModelRenderSetup.cpp:30, RenderServices.cpp:314).
        SharedPtr<Asset> modelAsset = am.GetAsset(modelGuid);
        if (!modelAsset)
            modelAsset = am.LoadAssetAsync(modelGuid, AssetLoadPriority::High).get();
        if (!modelAsset)
            return Editor::RefuseRequest("Failed to load model asset");

        auto* modelPtr = dynamic_cast<ModelAsset*>(modelAsset.get());
        if (!modelPtr || !modelPtr->IsLoaded())
            return Editor::RefuseRequest("Asset is not a loaded ModelAsset");

        const std::string clipPath = ctx.params.value("clipPath", std::string{});
        GUID clipGuid;
        if (!clipPath.empty())
        {
            clipGuid = am.ResolveAssetGuid(clipPath);
            if (clipGuid.IsNull())
                return Editor::RefuseRequest("Could not resolve clipPath: " + clipPath);
            // Force the clip asset to fully import as well — required so
            // ClipStore::GetOrLoadClipIndex() finds non-zero clip data on the
            // first call without polling.
            SharedPtr<Asset> clipAsset = am.GetAsset(clipGuid);
            if (!clipAsset)
                clipAsset = am.LoadAssetAsync(clipGuid, AssetLoadPriority::High).get();
            if (!clipAsset)
                return Editor::RefuseRequest("Failed to load clip asset");

            // If clipPath points to a Model file (e.g. an FBX with embedded
            // animations), normalize to the first embedded clip's derived
            // GUID — same path the Animator inspector takes via
            // NormalizeClipSelection.
            if (clipAsset->GetType() == AssetType::Model)
            {
                auto* clipModel = dynamic_cast<ModelAsset*>(clipAsset.get());
                if (clipModel && !clipModel->GetEmbeddedClipGuids().empty())
                    clipGuid = clipModel->GetEmbeddedClipGuids().front();
                else
                    return Editor::RefuseRequest("clipPath points to a Model with no embedded clips: " + clipPath);
            }
        }

        const std::string rootName = ctx.params.value("name",
            std::filesystem::path(modelPath).stem().string());

        auto created = Engine::Renderer::ModelEntityFactory::CreateFromModel(
            *rs, *world, *modelPtr, modelGuid, rootName, Editor::GetFbxModelEntityFactoryOptions());
        if (!created.IsValid())
            return Editor::RefuseRequest("ModelEntityFactory::CreateFromModel failed");

        // Apply optional position to the root entity's transform.
        if (ctx.params.contains("position") && ctx.params["position"].is_object())
        {
            const auto& p = ctx.params["position"];
            const float px = p.value("x", 0.0f);
            const float py = p.value("y", 0.0f);
            const float pz = p.value("z", 0.0f);
            if (auto* xf = world->GetComponent<Components::Transform>(created.rootEntity))
            {
                Components::Transform updated = *xf;
                updated.matrix[12] = px;
                updated.matrix[13] = py;
                updated.matrix[14] = pz;
                world->AddComponentImmediate(created.rootEntity, updated);
            }
        }

        // Wire AnimatorRef on every skinned submesh entity. ModelEntityFactory
        // already adds a default AnimatorRef (no clip); we replace it with the
        // requested clip index here.
        //
        // Also set Animator.clipGuid on the root entity so play-mode entry
        // doesn't wipe the clip selection. The runtime resolves clipGuid →
        // ClipStore index on play; without it, AnimatorRef.ClipIndex resets
        // to 0 when the editor swaps to play world.
        uint32_t clipIndex = 0;
        if (!clipGuid.IsNull())
        {
            clipIndex = Engine::Renderer::ClipStore::Instance().GetOrLoadClipIndex(clipGuid, am);
            if (clipIndex != 0)
            {
                const bool loop   = ctx.params.value("loop", true);
                const bool paused = ctx.params.value("paused", false);
                const float speed = ctx.params.value("speed", 1.0f);
                for (auto e : created.submeshEntities)
                {
                    Components::AnimatorRef anim{};
                    anim.ClipIndex = clipIndex;
                    anim.Time = 0.0f;
                    anim.Speed = speed;
                    anim.Flags = (loop ? Components::AnimatorRef::kFlag_Loop : 0u)
                               | (paused ? Components::AnimatorRef::kFlag_Paused : 0u);
                    world->AddComponentImmediate(e, anim);
                }

                // Stamp the Animator.clipGuid on whichever entity currently
                // owns the Animator component (root for multi-submesh, or the
                // sole submesh for single-submesh models).
                ECS::EntityHandle animatorOwner = created.rootEntity;
                if (auto* existing = world->GetComponent<Components::Animator>(animatorOwner))
                {
                    Components::Animator updated = *existing;
                    updated.SelectClip(clipGuid);
                    world->AddComponentImmediate(animatorOwner, updated);
                }
                else
                {
                    for (auto e : created.submeshEntities)
                    {
                        if (auto* ex = world->GetComponent<Components::Animator>(e))
                        {
                            Components::Animator updated = *ex;
                            updated.SelectClip(clipGuid);
                            world->AddComponentImmediate(e, updated);
                            break;
                        }
                    }
                }
            }
        }

        if (app.m_ChangeNotifications)
        {
            Editor::EditorChangeNotifications::WorldStructureChangedEvent ev{};
            ev.world = world;
            ev.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
            app.m_ChangeNotifications->NotifyWorldStructureChanged(ev);
        }

        json submeshIds = json::array();
        for (auto e : created.submeshEntities)
            submeshIds.push_back(e.id);

        return json{
            {"rootEntityId", created.rootEntity.id},
            {"submeshEntityIds", std::move(submeshIds)},
            {"skinned", created.skinned},
            {"clipIndex", clipIndex},
        }; });

    // 8d. export_model_materials — invoke Editor::ExportModelMaterials on an
    // already-spawned model. Test scaffolding for the provenance integration
    // test. The drag-drop UI path
    // in SceneViewPanel / HierarchyPanel is what normally calls
    // ExportModelMaterials, but those paths require UI events to fire — this
    // RPC lets a test harness run the same flow over IPC.
    //
    // Caller is expected to have spawned the model first (e.g. via
    // spawn_humanoid_test_pair) and passes the resulting submesh entity IDs
    // plus the model's asset-relative path.
    server.RegisterHandler("export_model_materials", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        (void)app;
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        const std::string modelPath = ctx.params.value("modelPath", std::string{});
        if (modelPath.empty())
            return Editor::RefuseRequest("Missing 'modelPath'");

        auto& am = EngineCore::GetInstance().GetAssetManager();
        const GUID modelGuid = am.ResolveAssetGuid(modelPath);
        if (modelGuid.IsNull())
            return Editor::RefuseRequest("Could not resolve modelPath: " + modelPath);

        const std::filesystem::path absModelPath = am.ResolveAssetPath(modelPath);

        std::vector<ECS::EntityHandle> entities;
        if (ctx.params.contains("entityIds") && ctx.params["entityIds"].is_array())
        {
            for (const auto& e : ctx.params["entityIds"])
            {
                if (e.is_number_unsigned())
                {
                    ECS::EntityHandle h;
                    h.id = e.get<uint32_t>();
                    entities.push_back(h);
                }
            }
        }
        if (entities.empty())
            return Editor::RefuseRequest("Missing or empty 'entityIds'");

        Editor::ExportModelMaterials(absModelPath, modelGuid, entities, *world, am);

        return json{
            {"ok", true},
            {"modelPath", modelPath},
            {"modelGuid", modelGuid.ToString()},
            {"entityCount", entities.size()},
        }; });

    // 8e. enumerate_provenance — read all rows from the v6 provenance table
    // on the project source's SQLite cache. Test scaffolding for the
    // provenance integration test; lets the harness verify the importer
    // call sites wrote the expected rows.
    server.RegisterHandler("enumerate_provenance", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        (void)ctx;
        auto& am = EngineCore::GetInstance().GetAssetManager();
        auto& registry = am.GetRegistry();

        json rows = json::array();
        if (const auto* projectSource = registry.ProjectSourcePinned().get())
        {
            if (auto* cache = projectSource->Cache.get())
            {
                for (const auto& row : cache->EnumerateAllProvenance())
                {
                    rows.push_back(json{
                        {"produced", row.Produced.ToString()},
                        {"producer", row.Producer.ToString()},
                        {"importerId", row.ImporterId},
                    });
                }
            }
        }
        return json{{"count", rows.size()}, {"rows", std::move(rows)}}; });
}

} // namespace GameEngine
