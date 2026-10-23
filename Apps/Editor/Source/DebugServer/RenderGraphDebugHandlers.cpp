#include "DebugServer/RenderGraphDebugHandlers.h"

#include "DebugServer/DebugHandlerHelpers.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/RenderGraphResourceFilter.h"

#include "Core/Engine.h"
#include "EditorApplication.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGGraph.h"
#include "Rendering/Core/RenderGraph/RGTypes.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;
namespace RG = Rendering::RenderGraph;

namespace
{

// Lifetime classes reported by get_render_graph_resources. The backbuffer is
// an external import too, so it must be tested before the external case.
const char* RgResourceLifetimeName(const RG::RGFrame& frame, RG::RGResourceId r)
{
    if (frame.IsBackbuffer(RG::RGTexture{r}))
        return "Swapchain";
    if (frame.Graph().IsImported(r))
        return "Persistent";
    if (frame.Graph().IsExternal(r))
        return "Imported";
    return "Transient";
}

const char* RgLayoutName(RG::RGImageLayout l)
{
    switch (l)
    {
    case RG::RGImageLayout::Undefined:       return "Undefined";
    case RG::RGImageLayout::General:         return "General";
    case RG::RGImageLayout::ColorAttachment: return "ColorAttachment";
    case RG::RGImageLayout::DepthAttachment: return "DepthAttachment";
    case RG::RGImageLayout::DepthReadOnly:   return "DepthReadOnly";
    case RG::RGImageLayout::ShaderReadOnly:  return "ShaderReadOnly";
    case RG::RGImageLayout::TransferSrc:     return "TransferSrc";
    case RG::RGImageLayout::TransferDst:     return "TransferDst";
    case RG::RGImageLayout::Present:         return "Present";
    }
    return "?";
}

const char* RgAccessName(RG::RGAccess a)
{
    switch (a)
    {
    case RG::RGAccess::Sampled:         return "Sampled";
    case RG::RGAccess::SampledCompute:  return "SampledCompute";
    case RG::RGAccess::SampledVertex:   return "SampledVertex";
    case RG::RGAccess::UniformRead:     return "Uniform";
    case RG::RGAccess::StorageRead:     return "StorageRead";
    case RG::RGAccess::IndirectRead:    return "Indirect";
    case RG::RGAccess::IndexRead:       return "Index";
    case RG::RGAccess::VertexRead:      return "Vertex";
    case RG::RGAccess::CopySrc:         return "CopySrc";
    case RG::RGAccess::DepthRead:       return "DepthRead";
    case RG::RGAccess::ColorLoad:       return "ColorLoad";
    case RG::RGAccess::AccelerationStructureRead: return "AccelerationStructureRead";
    case RG::RGAccess::ColorAttachment: return "ColorAttachment";
    case RG::RGAccess::DepthWrite:      return "DepthWrite";
    case RG::RGAccess::StorageWrite:    return "StorageWrite";
    case RG::RGAccess::CopyDst:         return "CopyDst";
    case RG::RGAccess::AccelerationStructureBuild: return "AccelerationStructureBuild";
    }
    return "?";
}

uint32_t RgBarrierCountForPass(const RG::RGGraph& g, RG::RGPassId p)
{
    uint32_t n = 0;
    for (const auto& batch : g.BarrierBatches())
        if (batch.Pass == p)
            n += batch.Count;
    return n;
}

} // namespace

void RegisterRenderGraphDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    // ── Render Graph introspection ──────────────────────────────────────────────
    //
    // These read the MAIN window's live RGFrame at request time (see
    // MainRgFrame in DebugHandlerHelpers.h). The frame holds the previous
    // frame's compiled graph — a stable snapshot, since the next BeginFrame
    // (which clears it) runs later in the loop. Per-pass GPU timings are present only while
    // profiling is armed (Visual Profiler / Render Graph panel Profile button).

    // Build a name -> {cpuMs, gpuSpanMs} overlay from the last resolved timings.
    // See get_gpu_profiler's timingSemantics for what the GPU value measures.
    auto rgTimingMap = [](RG::RGFrame* frame) {
        std::unordered_map<std::string, std::pair<double, double>> m;
        if (frame)
            for (const auto& t : frame->LastFrameTimings())
                m[t.Name] = {t.CpuMs, t.GpuSpanMs};
        return m;
    };

    // 16. get_render_graph_overview
    server.RegisterHandler("get_render_graph_overview", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        const RG::RGFrame::FrameStats& s = frame->Stats();

        size_t transient = 0, persistent = 0, imported = 0;
        const size_t resCount = g.ResourceCount();
        for (RG::RGResourceId r = 0; r < resCount; ++r)
        {
            if (!g.IsResourceUsed(r)) continue;
            if (g.IsImported(r)) ++persistent;
            else if (g.IsExternal(r)) ++imported;
            else ++transient;
        }

        json result;
        result["passCount"] = g.ScheduledOrder().size();
        result["declaredPassCount"] = g.PassCount();
        result["culledPassCount"] = g.CulledPassCount();
        result["resourceCount"] = resCount;
        result["barrierCount"] = g.BarrierCount();
        result["barrierBatches"] = s.BarrierBatchesEmitted;
        result["barriersHoisted"] = s.BarriersHoisted;
        result["crossQueueTransitions"] = g.CrossQueueTransitionsConsidered();
        result["crossQueueRelocationsDeclined"] = g.CrossQueueRelocationsDeclined();
        result["renderPassesBegun"] = s.RenderPassesBegun;
        result["submissionCount"] = s.SubmissionsMade;
        result["transientCount"] = transient;
        result["persistentCount"] = persistent;
        result["importedCount"] = imported;
        result["workingSetTransitions"] = g.WorkingSetTransitions();
        result["profilingEnabled"] = frame->ProfilingEnabled();
        result["frameIndex"] = frame->FrameIndex();
        // Render pipeline compiles so far: a count that climbs every frame is a pipeline compiled
        // every frame.
        if (const Engine::Renderer::RenderServices* rs = EngineCore::GetInstance().GetRenderServices())
            result["pipelineCompiles"] = rs->Spine().LastCompileReport().Compiles;
        return result; });

    // 17. get_render_graph_passes
    server.RegisterHandler("get_render_graph_passes", [&app, rgTimingMap](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        const auto timings = rgTimingMap(frame);
        const bool includeAccesses = ctx.params.value("includeAccesses", false);

        json passes = json::array();
        uint32_t idx = 0;
        for (RG::RGPassId p : g.ScheduledOrder())
        {
            const char* name = g.PassName(p);
            json jp;
            jp["id"] = p;
            jp["scheduledIndex"] = idx++;
            jp["name"] = name ? name : "(unnamed)";
            jp["queue"] = RgQueueName(g.PassQueue(p));
            jp["phase"] = g.PassPhase(p);
            jp["level"] = g.Level(p);
            jp["barrierCount"] = RgBarrierCountForPass(g, p);
            if (name)
            {
                auto it = timings.find(name);
                if (it != timings.end())
                {
                    jp["cpuMs"] = it->second.first;
                    jp["gpuSpanMs"] = it->second.second;
                }
            }
            if (includeAccesses)
            {
                json accesses = json::array();
                for (const RG::RGAccessRecord& a : g.Accesses())
                {
                    if (a.Pass != p) continue;
                    const char* rn = g.ResourceName(a.Resource);
                    accesses.push_back({{"resource", rn ? rn : "(resource)"},
                                        {"access", RgAccessName(a.Access)},
                                        {"write", RG::IsWrite(a.Access)}});
                }
                jp["accesses"] = std::move(accesses);
            }
            passes.push_back(std::move(jp));
        }
        return json{{"passCount", passes.size()},
                    {"profilingEnabled", frame->ProfilingEnabled()},
                    {"passes", std::move(passes)}}; });

    // 18. get_render_graph_resources
    server.RegisterHandler("get_render_graph_resources", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();

        const Editor::RenderGraphResourceFilter filter(ctx.params);
        if (!filter.Error().empty())
            return Editor::RefuseRequest(filter.Error());

        json resources = json::array();
        const size_t resCount = g.ResourceCount();
        for (RG::RGResourceId r = 0; r < resCount; ++r)
        {
            const RG::RGResourceDesc& d = g.ResourceDesc(r);
            const bool used = g.IsResourceUsed(r);
            const char* kind = d.Kind == RG::RGResourceKind::Buffer                  ? "Buffer"
                               : d.Kind == RG::RGResourceKind::AccelerationStructure ? "AccelerationStructure"
                                                                                       : "Texture";
            const char* lifetime = RgResourceLifetimeName(*frame, r);
            if (!filter.Accepts(kind, lifetime, used))
                continue;

            const char* name = g.ResourceName(r);
            json jr;
            jr["id"] = r;
            jr["name"] = name ? name : "(unnamed)";
            jr["kind"] = kind;
            jr["used"] = used;
            jr["external"] = g.IsExternal(r);
            jr["imported"] = g.IsImported(r);
            jr["lifetime"] = lifetime;
            if (d.Kind == RG::RGResourceKind::Buffer)
            {
                jr["sizeBytes"] = d.SizeBytes;
            }
            else if (d.Kind == RG::RGResourceKind::Texture)
            {
                jr["width"] = d.Width;
                jr["height"] = d.Height;
                jr["format"] = d.Format;
                jr["mipLevels"] = d.MipLevels;
                jr["arrayLayers"] = d.ArrayLayers;
                jr["sampleCount"] = d.SampleCount;
            }
            resources.push_back(std::move(jr));
        }
        // totalResourceCount is the unfiltered size, so a caller can see that a
        // filter applied rather than guessing from the count alone.
        return json{{"resourceCount", resources.size()},
                    {"totalResourceCount", resCount},
                    {"resources", std::move(resources)}}; });

    // 19. get_render_graph_pass_detail
    server.RegisterHandler("get_render_graph_pass_detail", [&app, rgTimingMap](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const std::string wanted = ctx.params.value("name", std::string());
        if (wanted.empty())
            return Editor::RefuseRequest("Provide a 'name' parameter (use get_render_graph_passes to list names)");
        const RG::RGGraph& g = frame->Graph();

        RG::RGPassId target = RG::kInvalidId;
        for (RG::RGPassId p : g.ScheduledOrder())
        {
            const char* n = g.PassName(p);
            if (n && wanted == n) { target = p; break; }
        }
        if (target == RG::kInvalidId)
            return Editor::RefuseRequest("Pass not in the current scheduled graph");

        json accesses = json::array();
        for (const RG::RGAccessRecord& a : g.Accesses())
        {
            if (a.Pass != target) continue;
            const char* rn = g.ResourceName(a.Resource);
            accesses.push_back({{"resource", rn ? rn : "(resource)"},
                                {"access", RgAccessName(a.Access)},
                                {"write", RG::IsWrite(a.Access)}});
        }

        json barriers = json::array();
        const auto& flat = g.Barriers();
        for (const auto& batch : g.BarrierBatches())
        {
            if (batch.Pass != target) continue;
            for (uint32_t i = 0; i < batch.Count; ++i)
            {
                const uint32_t bi = batch.First + i;
                if (bi >= flat.size()) break;
                const RG::RGBarrier& b = flat[bi];
                const char* rn = g.ResourceName(b.Resource);
                barriers.push_back({{"resource", rn ? rn : "(resource)"},
                                    {"from", RgLayoutName(b.OldLayout)},
                                    {"to", RgLayoutName(b.NewLayout)}});
            }
        }

        json successors = json::array();
        for (RG::RGPassId succ : g.PassSuccessors(target))
        {
            const char* n = g.PassName(succ);
            successors.push_back(n ? n : "(unnamed)");
        }

        json detail;
        detail["name"] = wanted;
        detail["queue"] = RgQueueName(g.PassQueue(target));
        detail["phase"] = g.PassPhase(target);
        detail["level"] = g.Level(target);
        detail["barrierCount"] = RgBarrierCountForPass(g, target);
        detail["accesses"] = std::move(accesses);
        detail["barriers"] = std::move(barriers);
        detail["successors"] = std::move(successors);
        const auto timings = rgTimingMap(frame);
        auto it = timings.find(wanted);
        if (it != timings.end())
        {
            detail["cpuMs"] = it->second.first;
            detail["gpuSpanMs"] = it->second.second;
        }
        return detail; });

    // 20. get_render_graph_dependencies
    server.RegisterHandler("get_render_graph_dependencies", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        json edges = json::array();
        for (RG::RGPassId p : g.ScheduledOrder())
        {
            const char* from = g.PassName(p);
            for (RG::RGPassId succ : g.PassSuccessors(p))
            {
                const char* to = g.PassName(succ);
                edges.push_back({{"from", from ? from : "(unnamed)"},
                                 {"to", to ? to : "(unnamed)"}});
            }
        }
        return json{{"edgeCount", edges.size()}, {"edges", std::move(edges)}}; });

    // 21. get_render_graph_validation — the immediate-mode graph has no separate
    // validation layer; surface culled passes (declared but never run) and their
    // reason as warnings, which is the actionable signal here.
    server.RegisterHandler("get_render_graph_validation", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        auto reasonName = [](RG::RGCullReason r) -> const char* {
            switch (r)
            {
            case RG::RGCullReason::NotCulled:      return "NotCulled";
            case RG::RGCullReason::NoConsumer:     return "NoConsumer";
            case RG::RGCullReason::ProducerCulled: return "ProducerCulled";
            case RG::RGCullReason::Cycle:           return "Cycle";
            }
            return "?";
        };
        json issues = json::array();
        const size_t passCount = g.PassCount();
        for (RG::RGPassId p = 0; p < passCount; ++p)
        {
            if (!g.IsCulled(p)) continue;
            const char* n = g.PassName(p);
            issues.push_back({{"pass", n ? n : "(unnamed)"},
                              {"severity", "warning"},
                              {"reason", reasonName(g.CullReason(p))}});
        }
        return json{{"errorCount", 0},
                    {"warningCount", issues.size()},
                    {"passIssues", std::move(issues)}}; });
}

} // namespace GameEngine
