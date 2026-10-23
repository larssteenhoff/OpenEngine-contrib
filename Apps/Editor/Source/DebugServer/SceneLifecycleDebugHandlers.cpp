#include "DebugServer/SceneLifecycleDebugHandlers.h"

#include "DebugServer/DebugHandlerHelpers.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/PlayModeRequest.h"

#include "Core/Engine.h"
#include "EditorApplication.h"
#include "EditorPanelIds.h"
#include "PlayMode/PlayModeManager.h"
#include "Scene/SceneEditorController.h"
#include "Scene/SceneLoadFailure.h"
#include "UI/Layout/Docking.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>

namespace GameEngine
{

using json = nlohmann::json;

void RegisterSceneLifecycleDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    // open_scene
    //   path:          required scene path.
    //   additive:      optional (default false).
    //   restorePolicy: optional "prompt" (default) | "discard" | "restore" —
    //                  headless answer for the modals an open can raise. The
    //                  dirty-scene prompt resolves as Don't Save for both
    //                  non-prompt values; the crash-recovery prompt discards or
    //                  restores the autosave staging. With "prompt", a raised
    //                  modal silently blocks the open — poll get_editor_state
    //                  (modal field) and answer via respond_modal.
    server.RegisterHandler("open_scene", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!app.m_SceneEditor)
            return Editor::RefuseRequest("Scene editor is not available");

        const std::string pathStr = ctx.params.value("path", "");
        if (pathStr.empty())
            return Editor::RefuseRequest("Missing path");

        const bool additive = ctx.params.value("additive", false);
        const std::string policyStr = ctx.params.value("restorePolicy", "prompt");
        auto policy = Editor::SceneEditorController::OpenRestorePolicy::Prompt;
        if (policyStr == "discard")
            policy = Editor::SceneEditorController::OpenRestorePolicy::Discard;
        else if (policyStr == "restore")
            policy = Editor::SceneEditorController::OpenRestorePolicy::Restore;
        else if (policyStr != "prompt")
            return Editor::RefuseRequest("restorePolicy must be prompt, discard, or restore");

        app.m_SceneEditor->RequestOpenScene(std::filesystem::path(pathStr), additive, policy);
        return json{{"queued", true}, {"path", pathStr}, {"additive", additive},
                    {"restorePolicy", policyStr}}; });

    // respond_modal
    // Answers the currently-visible scene modal (the ones that block scene
    // opens/saves) by choice name, equivalent to clicking its button:
    //   saveChanges   -> save | dontSave | cancel
    //   restoreBackup -> restore | discard
    //   saveAs        -> cancel
    //   revertScene   -> confirm | cancel
    // get_editor_state reports which modal is up under "modal".
    server.RegisterHandler("respond_modal", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!app.m_SceneEditor)
            return Editor::RefuseRequest("Scene editor is not available");
        const std::string choice = ctx.params.value("choice", "");
        if (choice.empty())
            return Editor::RefuseRequest("Provide choice (save|dontSave|cancel|restore|discard|confirm|ok)");

        const std::string kind = app.m_SceneEditor->GetActiveModalKind();
        std::string error;
        if (!app.m_SceneEditor->RespondToActiveModal(choice, &error))
            return Editor::RefuseRequest(error);
        return json{{"ok", true}, {"modal", kind}, {"choice", choice}}; });

    // save_scene — synchronous save of the active scene document.
    //   path present -> Save As to that path (project-relative resolves under the
    //                   asset root, .scene enforced), adopting it as the document path.
    //   path absent  -> Save to the current scene path (fails if the document is untitled).
    // Routes through SceneDocumentManager::Save / SaveAs, so hierarchy-UI capture +
    // MarkClean happen exactly as the Ctrl+S / File menu path does.
    server.RegisterHandler("save_scene", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!app.m_SceneEditor)
            return Editor::RefuseRequest("Scene editor is not available");

        const std::string pathStr = ctx.params.value("path", "");
        // A degraded scene will not overwrite its own source without this. Automation is the easiest
        // way to destroy such a file silently — no modal stands in the way — so refusing is the
        // default and consent has to be typed.
        const bool force = ctx.params.value("force", false);
        const Editor::DegradedSavePolicy degradedPolicy =
            force ? Editor::DegradedSavePolicy::SaveAnyway : Editor::DegradedSavePolicy::Refuse;
        std::string error;

        if (pathStr.empty())
        {
            // Deliberately NOT the UI-replay command: it calls SaveActiveScene with the default
            // policy and carries no way to pass one, so force= could not reach the guard through it.
            if (!app.m_SceneEditor->SaveActiveScene(&error, degradedPolicy))
                return Editor::RefuseRequest(error.empty() ? "save failed" : error);
            const auto saved = app.m_SceneEditor->GetActiveScenePath();
            return json{{"ok", true}, {"path", saved ? saved->string() : std::string{}}};
        }

        std::filesystem::path scenePath(pathStr);
        if (scenePath.is_relative())
        {
            const auto& assetRoot = EngineCore::GetInstance().GetResolvedAssetRoot();
            if (!assetRoot.empty())
                scenePath = assetRoot / scenePath;
        }
        std::string ext = scenePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".scene")
            scenePath.replace_extension(".scene");
        scenePath = scenePath.lexically_normal();

        if (!app.m_SceneEditor->SaveActiveSceneAs(scenePath, &error, degradedPolicy))
            return Editor::RefuseRequest(error.empty() ? "save-as failed" : error);
        return json{{"ok", true}, {"path", scenePath.string()}}; });

    // set_play_mode — drive the editor's Play / Pause / Stop actions.
    //   action: "enter" | "exit" | "pause" | "resume".
    //   activateGameView: optional bool, default false.
    // Routed through the same UI-replay commands the toolbar buttons use, so the
    // play-fullscreen teardown and toolbar state stay in sync with the transition.
    // Each action is legal from exactly one state and refused from any other, and
    // the response always carries the resulting "playMode" so a caller never has
    // to trust its own intent.
    //
    // activateGameView raises the Game View through the same path focus_view
    // takes, before the transition: the Game View's composite is what publishes
    // the gameplay UI host, so a HUD needs that tab up to have anywhere to mount.
    // No main window means the view can never come up, so that refuses the whole
    // transition instead of entering play with the caller believing it did; the
    // reached-the-dock-model case reports "gameViewActive" read back instead.
    //
    // Nothing is saved on the caller's behalf. Entering snapshots the world and
    // exiting restores that snapshot, so runtime mutations are discarded — see the
    // tool description for the full contract.
    server.RegisterHandler("set_play_mode", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* playMode = app.m_PlayMode.get();
        if (!playMode)
            return Editor::RefuseRequest("Play mode is not available");

        // An exit while an entry waits for the script build drops the entry: the caller asked to
        // stay out of play mode, so play must not start when the build finishes.
        if (ctx.params.value("action", "") == "exit" && playMode->GetState() == Editor::PlayModeState::Edit &&
            playMode->GetEntryGate().GetPhase() == Editor::PlayModeEntryGate::Phase::Waiting)
        {
            playMode->ExitPlayMode();
            return json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}, {"entryCanceled", true}};
        }

        const Editor::PlayModeRequest request(ctx.params, playMode->GetState());
        if (!request.Error().empty())
            return Editor::RefuseRequest(request.Error(),
                                         json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});

        std::optional<ViewActivation> activation;
        if (request.ActivateGameView())
        {
            if (!app.m_Docking)
                return Editor::RefuseRequest("cannot activate the Game View; the main editor window is not available",
                                             json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});

            activation = ActivateDockedView(app, *app.m_Docking, EditorPanelIds::GameView,
                                            /*wantsViewportUiFocus=*/true);
            if (!activation->ok)
                return Editor::RefuseRequest("cannot activate the Game View; the main editor window is not available",
                                             json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});
        }

        std::string error;
        if (!app.InvokeUiReplayCommand(request.CommandId(), &error))
            return Editor::RefuseRequest(error.empty() ? std::string("play mode transition failed") : error,
                                         json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});

        const Editor::PlayModeState after = playMode->GetState();
        json result{{"playMode", Editor::PlayModeStateToString(after)}};

        // Present only when activation was requested: a caller that did not ask
        // to move its layout has no view state to reconcile.
        if (activation)
        {
            result["gameViewActive"] = activation->active;
            result["gameViewViewportFocused"] = activation->viewportFocused;
        }

        // A script build holds the entry: play mode starts when it finishes, and the
        // "Entering Play Mode" dialog shows the build until then.
        if (request.IsEnter() && after == Editor::PlayModeState::Edit &&
            playMode->GetEntryGate().GetPhase() == Editor::PlayModeEntryGate::Phase::Waiting)
        {
            result["waitingForScriptBuild"] = true;
            return result;
        }

        // A failed script build refuses play until the module rebuilds; say which build.
        if (request.IsEnter() && after == Editor::PlayModeState::Edit &&
            playMode->GetEntryGate().GetPhase() == Editor::PlayModeEntryGate::Phase::Failed)
        {
            const Editor::ScriptBuildSnapshot& build = playMode->GetEntryGate().GetSnapshot();
            std::string why = "play mode did not start; the latest script build failed:";
            for (const std::string& module : build.FailedModules)
                why += " " + module;
            if (!build.FirstError.empty())
                why += " (" + build.FirstError + ")";
            return Editor::RefuseRequest(why, std::move(result));
        }

        // EnterPlayMode refuses without a world, or while a deferred scene build is
        // still resolving entities, and says so only to the log. Report it rather
        // than answering ok with the editor still sitting in Edit.
        if (request.IsEnter() && after == Editor::PlayModeState::Edit)
            return Editor::RefuseRequest("play mode did not start; no world is open, the scene is still loading, "
                                         "or the pre-play snapshot failed — see the editor log",
                                         std::move(result));

        return result; });
}

} // namespace GameEngine
