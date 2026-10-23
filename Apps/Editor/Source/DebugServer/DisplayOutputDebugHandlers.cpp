#include "DebugServer/DisplayOutputDebugHandlers.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"

#include "Core/Engine.h"
#include "Editor/Settings/RenderProjectSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "EditorApplication.h"
#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/AntiAliasingProjectSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Platform/Display.h"
#include "Rendering/Core/Device.h"
#include "UI/UIManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

void RegisterDisplayOutputDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    // R0.11 diagnostic: the device's HDR output state (swapchain mode, encode
    // anchors, static metadata) next to the OS per-monitor SDR white levels so
    // the paper-white divisor can be verified against what Windows reports.
    server.RegisterHandler("get_hdr_output", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        json result;
        // The main window UI's most recent RenderRG declaration: the
        // TARGET-derived encoding and the subpixel gate it resolved (#784).
        // Under a pending capture these reflect the capture frame's SDR
        // declaration; steady-state they follow the display-bound target.
        if (!app.m_Windows.empty() && app.m_Windows[0] && app.m_Windows[0]->ui)
        {
            result["uiOutputEncoding"] = app.m_Windows[0]->ui->GetLastResolvedOutputEncoding();
            result["uiTextSubpixelActive"] = app.m_Windows[0]->ui->GetLastTextSubpixelActive();
        }
        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        {
            if (auto* device = rs->GetDevice())
            {
                const Rendering::HdrOutputState st = device->GetHdrOutputState();
                result["enabled"] = st.enabled;
                result["requestedMode"] = Rendering::HdrOutputModeToString(st.requestedMode);
                result["activeMode"] = Rendering::HdrOutputModeToString(st.activeMode);
                result["swapchainBitDepth"] = static_cast<int>(st.swapchainBitDepth);
                result["scRGBFramebufferWhiteNits"] = Rendering::GetScRGBFramebufferWhiteNits(st);
                result["outputMaxLinearValue"] = Rendering::GetHdrOutputMaxLinearValue(st);
                result["peakLuminanceNits"] = Rendering::GetHdrOutputPeakLuminanceNits(st);
                result["staticMetadata"] = {
                    {"paperWhiteNits", st.staticMetadata.paperWhiteNits},
                    {"maxMasteringLuminance", st.staticMetadata.maxMasteringLuminance},
                    {"minMasteringLuminance", st.staticMetadata.minMasteringLuminance},
                    {"maxContentLightLevel", st.staticMetadata.maxContentLightLevel},
                    {"maxFrameAverageLightLevel", st.staticMetadata.maxFrameAverageLightLevel}};
                result["display"] = {
                    {"id", st.display.id},
                    {"name", st.display.name},
                    {"activeMonitor", st.display.activeMonitor},
                    {"hdrAvailable", st.display.hdrAvailable},
                    {"hdrActive", st.display.hdrActive},
                    {"supportsHDR10_PQ", st.display.supportsHDR10_PQ},
                    {"supportsScRGB", st.display.supportsScRGB},
                    {"swapchainFormat", static_cast<int>(st.display.swapchainFormat)},
                    {"maxLuminance", st.display.maxLuminance},
                    {"minLuminance", st.display.minLuminance},
                    {"maxFullFrameLuminance", st.display.maxFullFrameLuminance},
                    {"paperWhiteNits", st.display.paperWhiteNits},
                    {"outputMaxLinearValue", st.display.outputMaxLinearValue},
                    {"requestedMode", Rendering::HdrOutputModeToString(st.display.requestedMode)},
                    {"resolvedMode", Rendering::HdrOutputModeToString(st.display.resolvedMode)},
                    {"colorSpaces", st.display.colorSpaces},
                    {"diagnosticHints", st.display.diagnosticHints}};
            }
        }
        json monitors = json::array();
        for (const auto& m : Platform::EnumerateMonitors())
        {
            monitors.push_back({{"index", m.index},
                                {"id", m.id},
                                {"name", m.name},
                                {"primary", m.primary},
                                {"hdrAvailable", m.hdrAvailable},
                                {"hdrActive", m.hdrActive},
                                {"supportsScRGB", m.supportsScRGB},
                                {"supportsHDR10_PQ", m.supportsHDR10_PQ},
                                {"maxLuminance", m.maxLuminance},
                                {"minLuminance", m.minLuminance},
                                {"maxFullFrameLuminance", m.maxFullFrameLuminance},
                                {"sdrWhiteLevelNits", m.sdrWhiteLevelNits},
                                {"diagnosticHints", m.diagnosticHints}});
        }
        result["osMonitors"] = monitors;
        // Which values are explicit project-settings overrides. Absent = auto
        // (monitor-derived); that distinction is exactly what this diagnostic
        // exists to make visible.
        {
            json overrides = json::object();
            Editor::SettingsStore store =
                Editor::OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
            std::string error;
            if (store.Load(&error) && store.Json().is_object())
            {
                const auto& root = store.Json();
                if (auto itR = root.find("rendering"); itR != root.end() && itR->is_object())
                {
                    if (auto itH = itR->find("hdr"); itH != itR->end() && itH->is_object())
                    {
                        for (const char* k : {"enabled", "mode", "uiPaperWhiteNits",
                                              "uiBlackLiftNits", "metadata"})
                            if (itH->contains(k))
                                overrides[k] = (*itH)[k];
                    }
                }
            }
            result["settingsOverrides"] = overrides;
        }
        return result; });

    server.RegisterHandler("set_hdr_output", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const bool enabled = ctx.params.value("enabled", true);
        const bool hasExplicitMode = ctx.params.contains("mode") && ctx.params["mode"].is_string();
        const std::string modeText = ctx.params.value("mode", std::string("Auto"));

        auto& engine = EngineCore::GetInstance();
        Editor::SettingsStore store = Editor::OpenProjectSettings(engine.GetWorkspaceRoot());
        std::string error;
        if (!store.Load(&error))
            return Editor::RefuseRequest(error.empty() ? "Failed to load project settings" : error);

        nlohmann::json hdr = nlohmann::json::object();
        const auto& root = store.Json();
        if (root.is_object())
        {
            const auto itRendering = root.find("rendering");
            if (itRendering != root.end() && itRendering->is_object())
            {
                const auto itHdr = itRendering->find("hdr");
                if (itHdr != itRendering->end() && itHdr->is_object())
                    hdr = *itHdr;
            }
        }
        // A caller-supplied mode is validated, never silently coerced: an
        // unrecognized spelling is refused rather than read as Off, which the
        // enabled path normalizes to Auto — a typo would apply a mode the caller
        // never asked for and report success.
        std::optional<Rendering::HdrOutputMode> requestedMode;
        if (hasExplicitMode)
        {
            requestedMode = Rendering::TryParseHdrOutputMode(modeText);
            if (!requestedMode)
                return Editor::RefuseRequest("Unrecognized HDR mode '" + modeText +
                                                 "'. Valid values: Off, Auto, HDR10_PQ, HLG, scRGB, HDR10+.");
        }

        hdr["enabled"] = enabled;
        if (enabled)
        {
            Rendering::HdrOutputMode mode = requestedMode.value_or(Rendering::HdrOutputMode::Auto);
            if (mode == Rendering::HdrOutputMode::Off)
                mode = Rendering::HdrOutputMode::Auto;
            hdr["mode"] = Rendering::HdrOutputModeToString(mode);
        }
        else
        {
            Rendering::HdrOutputMode mode = Rendering::HdrOutputMode::Off;
            if (hdr.contains("mode") && hdr["mode"].is_string())
                mode = Rendering::HdrOutputModeFromString(hdr["mode"].get<std::string>());
            if (requestedMode && *requestedMode != Rendering::HdrOutputMode::Off)
                mode = *requestedMode;
            if (mode == Rendering::HdrOutputMode::Off)
                mode = Rendering::HdrOutputMode::Auto;
            hdr["mode"] = Rendering::HdrOutputModeToString(mode);
        }
        // Persist only caller-provided values. Materializing defaults here made
        // every field read as an explicit user override and permanently disabled
        // the monitor-derived tuning (paperWhiteNits pinned at 203 while the OS
        // SDR white level said otherwise). null erases a key = back to auto.
        auto writeOrErase = [&ctx](nlohmann::json& target, const char* key)
        {
            const auto it = ctx.params.find(key);
            if (it == ctx.params.end())
                return;
            if (it->is_null())
                target.erase(key);
            else if (it->is_number())
                target[key] = it->get<float>();
        };
        auto& metadata = hdr["metadata"];
        if (!metadata.is_object())
            metadata = nlohmann::json::object();
        writeOrErase(metadata, "paperWhiteNits");
        writeOrErase(metadata, "maxMasteringLuminance");
        writeOrErase(metadata, "maxContentLightLevel");
        writeOrErase(metadata, "maxFrameAverageLightLevel");
        if (metadata.empty())
            hdr.erase("metadata");
        writeOrErase(hdr, "uiPaperWhiteNits");
        writeOrErase(hdr, "uiBlackLiftNits");
        auto& writableRoot = store.Json();
        if (!writableRoot.is_object())
            writableRoot = nlohmann::json::object();
        auto& rendering = writableRoot["rendering"];
        if (!rendering.is_object())
            rendering = nlohmann::json::object();
        rendering["hdr"] = hdr;
        if (!store.Save(&error))
            return Editor::RefuseRequest(error.empty() ? "Failed to save project settings" : error);

        app.RequestHdrOutputRefreshForAllWindows();
        return json{{"ok", true}, {"queued", true}, {"enabled", enabled}, {"mode", enabled ? hdr["mode"].get<std::string>() : "Off"}}; });

    // get_msaa — report the engine-wide default MSAA sample count (the value cameras
    // with MSAASamples==0 inherit), the device cap, and the persisted project setting.
    //
    // The sample count alone does NOT say whether anything is multisampled: it
    // keeps its value while aaMode is off or taa, so a caller reading only
    // "samples" can report 8x on a run with no MSAA at all. aaMode is therefore
    // reported alongside it and is the field that decides whether samples is in
    // effect (it is honored only in msaa mode). aaModePersisted is the project
    // setting, which names a concrete mode: opening a project writes one for it
    // if it never chose. It is empty only for a project whose file has not been
    // materialized, and aaMode then reports the capability default the renderer
    // resolved in memory.
    //
    // Both fields describe the ENGINE-WIDE default, which is not necessarily what
    // a given view renders. At least two things reroute it downstream, so treat
    // this as a floor rather than an exhaustive list: any TAA-resolved mode is
    // dropped per-view where TAA does not apply (SceneViewController /
    // GameViewController drop it for orthographic, 2D, fixed-orientation,
    // letterboxed or already-multisampled views, and they do so identically
    // for every stored mode, including a materialized default), and a camera can
    // override the mode outright via RenderServices::ResolveAntiAliasing.
    server.RegisterHandler("get_msaa", [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("RenderServices unavailable");
        uint32_t deviceMax = 1u;
        if (auto* device = rs->GetDevice())
            deviceMax = std::max(1u, device->GetCapabilities().maxMSAASamples);
        std::string persisted;
        std::string persistedAAMode;
        {
            Editor::SettingsStore store =
                Editor::OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
            std::string error;
            if (store.Load(&error) && store.Json().is_object())
            {
                const auto& root = store.Json();
                if (auto itR = root.find("rendering"); itR != root.end() && itR->is_object())
                {
                    if (auto itM = itR->find("msaa"); itM != itR->end() && itM->is_string())
                        persisted = itM->get<std::string>();
                    if (auto itA = itR->find("aaMode"); itA != itR->end() && itA->is_string())
                        persistedAAMode = itA->get<std::string>();
                }
            }
        }
        return json{{"samples", rs->GetDefaultMSAASampleCount()},
                    {"deviceMax", deviceMax},
                    {"persisted", persisted},
                    {"aaMode", Rendering::ToAAModeToken(rs->GetDefaultAntiAliasingMode())},
                    {"aaModePersisted", persistedAAMode}}; });

    // set_msaa — persist rendering.msaa (same form the Settings panel uses) and
    // re-apply live (fans out to every window). samples: 1 = off, or 2/4/8 —
    // every accepted value is a concrete count, so what lands in the file is
    // what the renderer runs. The device clamps unsupported counts down. Shadow
    // maps are unaffected (depth-only, always single-sample).
    server.RegisterHandler("set_msaa", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("samples") || !ctx.params["samples"].is_number_integer())
            return Editor::RefuseRequest("set_msaa requires integer 'samples' (1=off, 2, 4, or 8)");
        const int requested = ctx.params["samples"].get<int>();
        if (requested != 1 && requested != 2 && requested != 4 && requested != 8)
            return Editor::RefuseRequest("invalid 'samples' (expected 1, 2, 4, or 8)");

        // Persist FIRST: the fan-out below re-applies from the settings file,
        // so a failed save would silently apply the old value.
        const std::string normalized =
            requested == 1 ? std::string("off") : std::to_string(requested);
        Editor::SettingsStore store =
            Editor::OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
        std::string error;
        if (!store.Load(&error))
            return Editor::RefuseRequest(error.empty() ? "Failed to load project settings" : error);
        auto& root = store.Json();
        if (!root.is_object())
            root = nlohmann::json::object();
        auto& rendering = root["rendering"];
        if (!rendering.is_object())
            rendering = nlohmann::json::object();
        rendering["msaa"] = normalized;
        if (!store.Save(&error))
            return Editor::RefuseRequest(error.empty() ? "Failed to save project settings" : error);

        app.ApplyProjectRenderSettingsToAllWindows();
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        const uint32_t resolved =
            rs ? rs->GetDefaultMSAASampleCount() : static_cast<uint32_t>(std::max(1, requested));
        return json{{"ok", true}, {"samples", resolved}, {"persisted", normalized}}; });

    // set_aa_mode — persist rendering.aaMode and re-apply live (fans out to
    // every window; targets re-spec next frame). mode: "off" | "msaa" | "taa" |
    // "fxaa" | "smaa" | "temporalfxaa". Supersampling is mode "off" with
    // set_taa_render_scale above 1.0; it has no token of its own.
    // Every mode leaves the sample count as rendering.msaa / set_msaa
    // configured it (honored only in msaa mode). The response reports the
    // APPLIED mode and samples, which differ from the request only under
    // GE_AA_MODE — that override persists the value but suppresses the live
    // apply, because the env var outranks project settings. get_msaa reports
    // the same applied mode without mutating anything.
    server.RegisterHandler("set_aa_mode", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string mode = ctx.params.value("mode", "");
        if (mode != "ssaa" && mode != Rendering::ToAAModeToken(Rendering::ParseAAModeToken(mode)))
            return Editor::RefuseRequest("set_aa_mode requires 'mode' of off|msaa|taa|fxaa|smaa|temporalfxaa|ssaa");

        // Persist FIRST: the fan-out below re-applies from the settings file.
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        auto settings = Editor::LoadProjectAntiAliasingSettings(workspaceRoot);
        settings.ModeChosen = true;
        settings.AAMode = Rendering::ParseAAModeToken(mode);
        if (!Editor::SaveProjectAntiAliasingSettings(workspaceRoot, settings))
            return Editor::RefuseRequest("Failed to save project settings");

        app.ApplyProjectRenderSettingsToAllWindows();
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        return json{{"ok", true},
                    {"mode", mode},
                    {"applied", Rendering::ToAAModeToken(
                                    rs ? rs->GetDefaultAntiAliasingMode() : settings.AAMode)},
                    {"samples", rs ? rs->GetDefaultMSAASampleCount() : 1u}}; });

    // set_taa_render_scale — persist rendering.taaRenderScale and re-apply
    // live (fans out to every window; the pipeline pre-pass re-derives the
    // internal extent next frame). scale: [0.5, 2.0]; 1.0 = native, above 1.0
    // supersamples (SSAA). Fully live, including the material texture mip bias
    // (per-view ViewParams uniform derived from the actual extent ratio).
    // Under GE_TAA_RENDER_SCALE the value persists but the live apply is
    // suppressed — the env override outranks project settings.
    server.RegisterHandler("set_taa_render_scale",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("scale") || !ctx.params["scale"].is_number())
            return Editor::RefuseRequest("set_taa_render_scale requires number 'scale' in [0.5, 2.0]");
        const float requested = ctx.params["scale"].get<float>();
        if (!(requested >= 0.5f && requested <= 2.0f))
            return Editor::RefuseRequest("invalid 'scale' (expected [0.5, 2.0])");

        // Persist FIRST: the fan-out below re-applies from the settings file.
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        auto settings = Editor::LoadProjectAntiAliasingSettings(workspaceRoot);
        settings.RenderScale = requested;
        // A non-native scale should take effect: Render Scale Mode Native pins
        // the scale to 1.0 on apply, so move it to Fixed — same behavior as the
        // settings slider. Dynamic keeps its mode.
        const bool nativeScale = requested > 0.995f && requested < 1.005f;
        if (!nativeScale && settings.ScaleMode == Engine::Renderer::DynamicResolutionMode::Off)
            settings.ScaleMode = Engine::Renderer::DynamicResolutionMode::Fixed;
        if (!Editor::SaveProjectAntiAliasingSettings(workspaceRoot, settings))
            return Editor::RefuseRequest("Failed to save project settings");

        app.ApplyProjectRenderSettingsToAllWindows();
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        return json{{"ok", true},
                    {"applied", rs ? rs->GetDefaultRenderScale() : requested}}; });

    // set_dynamic_resolution — mode + target for DRS, live, for same-session
    // A/B. mode: "off" | "fixed" | "dynamic". targetFps / targetMs optionally
    // retune the controller. Dynamic mode arms the render-graph GPU profiler
    // (its per-pass timestamps are the cost signal) and drives the same render
    // scale set_taa_render_scale writes, so the two are mutually exclusive by
    // construction. Returns the live controller state so a harness can watch
    // the loop converge without a screenshot.
    //
    // Fans out live rather than through the settings file like its three
    // siblings: this knob does not persist (targetMs, minScale and maxScale
    // have no project key at all), so the file cannot carry the state, and
    // under GE_TAA_RENDER_SCALE the file-backed re-apply skips the whole DRS
    // block — which would make this handler a no-op in exactly the A/B runs it
    // exists for.
    server.RegisterHandler("set_dynamic_resolution",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (rs == nullptr)
            return Editor::RefuseRequest("no RenderServices");

        using GameEngine::Engine::Renderer::DynamicResolutionMode;
        if (ctx.params.contains("mode"))
        {
            if (!ctx.params["mode"].is_string())
                return Editor::RefuseRequest("'mode' must be one of off|fixed|dynamic");
            const std::string mode = ctx.params["mode"].get<std::string>();
            if (mode == "off")
                rs->SetDynamicResolutionMode(DynamicResolutionMode::Off);
            else if (mode == "fixed")
                rs->SetDynamicResolutionMode(DynamicResolutionMode::Fixed);
            else if (mode == "dynamic")
                rs->SetDynamicResolutionMode(DynamicResolutionMode::Dynamic);
            else
                return Editor::RefuseRequest("'mode' must be one of off|fixed|dynamic");
        }

        auto config = rs->GetDynamicResolutionConfig();
        bool retune = false;
        if (ctx.params.contains("targetFps") && ctx.params["targetFps"].is_number())
        {
            const float fps = ctx.params["targetFps"].get<float>();
            if (!(fps > 0.0f))
                return Editor::RefuseRequest("'targetFps' must be > 0");
            config.TargetGpuMs = 1000.0f / fps;
            retune = true;
        }
        if (ctx.params.contains("targetMs") && ctx.params["targetMs"].is_number())
        {
            const float ms = ctx.params["targetMs"].get<float>();
            if (!(ms > 0.0f))
                return Editor::RefuseRequest("'targetMs' must be > 0");
            config.TargetGpuMs = ms;
            retune = true;
        }
        if (ctx.params.contains("minScale") && ctx.params["minScale"].is_number())
        {
            config.MinScale = ctx.params["minScale"].get<float>();
            retune = true;
        }
        if (ctx.params.contains("maxScale") && ctx.params["maxScale"].is_number())
        {
            config.MaxScale = ctx.params["maxScale"].get<float>();
            retune = true;
        }
        if (retune)
            rs->SetDynamicResolutionConfig(config);

        // Every floating window owns a private RenderServices, so the writes
        // above only moved the main one.
        app.ApplyDynamicResolutionFromMainWindowToAllWindows();

        const auto& applied = rs->GetDynamicResolutionConfig();
        const auto& stats = rs->GetDynamicResolutionStats();
        return json{{"ok", true},
                    {"mode", static_cast<uint32_t>(rs->GetDynamicResolutionMode())},
                    {"scale", rs->GetDefaultRenderScale()},
                    {"targetGpuMs", applied.TargetGpuMs},
                    {"minScale", applied.MinScale},
                    {"maxScale", applied.MaxScale},
                    // ticks/appFrameEpoch deltas between two polls must be ~1:
                    // the controller ticks once per app frame no matter how
                    // many windows declare (the multi-window dedup contract).
                    {"ticks", stats.Ticks},
                    {"appFrameEpoch", rs->Spine().WorldFrameEpoch()},
                    {"smoothedGpuMs", stats.SmoothedGpuMs},
                    {"acceptedDrops", stats.AcceptedDrops},
                    {"acceptedRises", stats.AcceptedRises},
                    {"saturatedLow", stats.SaturatedLow},
                    {"saturatedHigh", stats.SaturatedHigh},
                    {"responseFitValid", stats.ResponseFitValid},
                    {"scalingIneffective", stats.ScalingIneffective},
                    {"estimatedUnscalableMs", stats.EstimatedUnscalableMs},
                    {"estimatedFullRangeSavingMs", stats.EstimatedFullRangeSavingMs}}; });
}

} // namespace GameEngine
