#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/RenderPipelineAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ModuleRegistration.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "NativeScripting/NativeScriptManager.h"
#include "EngineLogCapture.h"
#include "Rendering/Common/Utils.h"
#include "Scripting/ScriptsConfig.h"
#include "TestDeviceHelper.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <fstream>
#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;
namespace fs = std::filesystem;

namespace
{
// Hold the actual async pipeline factory path, delegating to the production factory.
// The watchdog releases even a regressed blocking frame call, making failure bounded.
class HeldPipelineLoad final
{
  public:
    HeldPipelineLoad(AssetFactory inner, fs::path path)
        : m_Inner(std::move(inner)), m_Path(std::move(path)), m_Watchdog([this]
          {
              std::unique_lock lock(m_Mutex);
              if (!m_Condition.wait_for(lock, std::chrono::seconds(10), [this] { return m_Released; }))
              {
                  TimedOut = true;
                  m_Released = true;
                  m_Condition.notify_all();
              }
          }) {}
    ~HeldPipelineLoad() { Release(); m_Watchdog.join(); }
    SharedPtr<Asset> Create(const AssetMetadata& metadata)
    {
        std::error_code ec;
        if (fs::equivalent(metadata.Path, m_Path, ec))
        {
            std::unique_lock lock(m_Mutex);
            ++Entered;
            m_Condition.notify_all();
            m_Condition.wait(lock, [this] { return m_Released; });
        }
        return m_Inner(metadata);
    }
    bool WaitForEntry()
    {
        std::unique_lock lock(m_Mutex);
        return m_Condition.wait_for(lock, std::chrono::seconds(5), [this] { return Entered != 0; });
    }
    void Release()
    {
        std::lock_guard lock(m_Mutex);
        m_Released = true;
        m_Condition.notify_all();
    }
    std::atomic<unsigned> Entered{0};
    std::atomic<bool> TimedOut{false};

  private:
    AssetFactory m_Inner;
    fs::path m_Path;
    std::mutex m_Mutex;
    std::condition_variable m_Condition;
    bool m_Released = false;
    std::thread m_Watchdog;
};

struct Counters
{
    unsigned Constructed = 0;
    unsigned Declared = 0;
};

class LateNode final : public IRenderPipelineNode
{
  public:
    explicit LateNode(Counters& counters) : m_Counters(counters) { ++m_Counters.Constructed; }
    const char* GetTypeName() const override { return "LateNode"; }
    bool Initialize(std::string, std::string, std::string*) override { return true; }
    void DeclareForView(ViewDeclare&) override { ++m_Counters.Declared; }

  private:
    Counters& m_Counters;
};

// The asset-backed compiler is the behavior under test. No camera/window or
// scripting host is needed; a real device supplies the ordinary Declare owner.
class PipelineRegistrationTest : public testing::Test
{
  protected:
    fs::path root, previousCwd, pipelineFile, standInFile;
    std::unique_ptr<IDevice> device;
    std::unique_ptr<RenderServices> services;
    std::unique_ptr<RenderGraph::RGResourcePool> persistent;
    std::unique_ptr<RenderGraph::RGTransientPool> transient;
    std::unique_ptr<RenderGraph::RGUploadRing> ring;
    std::unique_ptr<RenderGraph::RGFrame> frame;
    ViewId view = 0;
    uint32_t frameIndex = 0;
    Counters counters;
    std::vector<std::string> messages;
    std::unique_ptr<TestLog::ScopedEngineLogCapture> capture;
    bool initialized = false;
    std::vector<std::shared_ptr<HeldPipelineLoad>> heldLoads;
    std::vector<AssetFuture> heldFutures;
    AssetTypeRegistration originalPipelineRegistration;
    bool factoryReplaced = false;

    static fs::path ResolveShader(const fs::path& path)
    {
        const auto candidate = fs::path(ENGINE_TEST_BUILD_DIR) / path;
        std::error_code ec;
        return fs::is_regular_file(candidate, ec) ? candidate : fs::path{};
    }

    void SetUp() override
    {
        previousCwd = fs::current_path();
        // The engine canonicalizes its workspace root (macOS: /var -> /private/var); an absolute path
        // the tests resolve must name the same registry entry as the engine's own relative lookup.
        root = fs::weakly_canonical(fs::temp_directory_path()) / ("pipeline-registration-" + GUID::Generate().ToString());
        pipelineFile = root / "Assets/RenderPipelines/ForwardPlus.rendergraph";
        fs::create_directories(pipelineFile.parent_path());
        ScriptsConfig scripts{};
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        auto& engine = EngineCore::GetInstance();
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config{};
        config.Name = "PipelineRegistrationTest";
        config.WorkspaceDirectory = root.string();
        config.AssetDirectory = "Assets";
        initialized = engine.Initialize(config);
        ASSERT_TRUE(initialized);
        // The engine's stand-in, in an 'editor' mount of its own: one pass the device can draw, under
        // the same relative path as the project's pipeline, which must not shadow it. A packaged game
        // has no 'editor' mount (MountsEditorAssets).
        standInFile = root / "Editor/RenderPipelines/ForwardPlus.rendergraph";
        fs::create_directories(standInFile.parent_path());
        {
            std::ofstream out(standInFile, std::ios::binary | std::ios::trunc);
            out << R"({"schemaVersion":2,"pipelineName":"EngineStandIn","passes":[{"id":"World","type":"WorldRender"}],)"
                << R"("outputs":{"FinalColor":"View.Resolve"}})";
            ASSERT_TRUE(out.good());
        }
        if (MountsEditorAssets())
        {
            AssetSourceDesc editorSource;
            editorSource.Alias = std::string(kAssetSourceAliasEditor);
            editorSource.Root = root / "Editor";
            editorSource.DerivedIdentity = true;
            editorSource.Priority = 50;
            ASSERT_TRUE(engine.GetAssetManager().RegisterSource(editorSource));
            engine.GetAssetManager().WaitForStartupScan(kAssetSourceAliasEditor);
        }
        // Keep device support shaders on the test build mount; the authored
        // .rendergraph itself still resolves through the real AssetManager.
        Utils::SetShaderFileLoader(nullptr);
        Utils::SetShaderPathResolver(&ResolveShader);
        device = CreateVulkanDeviceFast();
        if (!device)
            GTEST_SKIP() << "Vulkan device unavailable";
        services = std::make_unique<RenderServices>();
        ASSERT_TRUE(services->Initialize(device.get()));
        view = services->Views().AllocateView("Registration", services->Views().AllocateCamera("Registration"));
        services->Views().SetViewRenderLayerMask(view, GameEngine::Rendering::RenderLayerMask::Default());
        services->Views().SetViewTargets(view, 0, 0, 0, ViewClearConfig{});
        persistent = std::make_unique<RenderGraph::RGResourcePool>(device.get());
        transient = std::make_unique<RenderGraph::RGTransientPool>(device.get());
        ring = std::make_unique<RenderGraph::RGUploadRing>(device.get(), 2, 262144);
        frame = std::make_unique<RenderGraph::RGFrame>(device.get(), persistent.get(), transient.get(), ring.get());
        capture = std::make_unique<TestLog::ScopedEngineLogCapture>(&messages);
        Logger::Log::Info("pipeline-registration capture alive");
        Logger::Log::Flush();
        ASSERT_EQ(TestLog::CountLinesContaining(messages, "capture alive"), 1u);
    }

    virtual bool MountsEditorAssets() const { return true; }

    void TearDown() override
    {
        // Retain the held tasks across ASSERT exits. Release, drain, then restore
        // the exact factory before shutting down services or the asset manager.
        for (const auto& held : heldLoads)
            held->Release();
        for (auto& future : heldFutures)
            (void)future.get();
        heldFutures.clear();
        if (factoryReplaced)
        {
            auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry().GetTypeRegistry();
            registry.UnregisterAssetType(AssetType::RenderPipeline);
            EXPECT_TRUE(registry.RegisterAssetType(originalPipelineRegistration));
            factoryReplaced = false;
        }
        ECS::ClearActiveRegistrationModule();
        // The spine borrows the frame. Reap it before destruction, including
        // ASSERT exits; node counter references remain alive through Shutdown.
        if (services && frame)
            services->Spine().RemovePipelineInstanceForFrame(frame.get());
        if (services)
            services->Shutdown();
        services.reset();
        frame.reset();
        ring.reset();
        transient.reset();
        persistent.reset();
        if (device)
            device->Shutdown();
        device.reset();
        capture.reset();
        if (initialized)
            EngineCore::GetInstance().Shutdown();
        heldLoads.clear();
        std::error_code ec;
        fs::current_path(previousCwd, ec);
        fs::remove_all(root, ec);
    }

    std::shared_ptr<HeldPipelineLoad> Hold(const fs::path& path)
    {
        auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry().GetTypeRegistry();
        if (factoryReplaced || !registry.TryGetAssetTypeRegistration(
                AssetType::RenderPipeline, originalPipelineRegistration))
            return {};
        auto held = std::make_shared<HeldPipelineLoad>(originalPipelineRegistration.factory, path);
        auto replacement = originalPipelineRegistration;
        replacement.factory = [held](const AssetMetadata& metadata) { return held->Create(metadata); };
        registry.UnregisterAssetType(AssetType::RenderPipeline);
        factoryReplaced = true; // TearDown restores even if replacement registration fails.
        if (!registry.RegisterAssetType(replacement))
            return {};
        heldLoads.push_back(held);
        return held;
    }

    AssetFuture& StartHeldLoad(const GUID& guid)
    {
        heldFutures.push_back(EngineCore::GetInstance().GetAssetManager().LoadAssetAsync(guid));
        return heldFutures.back();
    }

    void Write(const char* passes, const char* name = "AuthoredLatePipeline")
    {
        std::ofstream out(pipelineFile, std::ios::binary | std::ios::trunc);
        out << "{\"schemaVersion\":2,\"pipelineName\":\"" << name << "\",\"passes\":[" << passes
            << "],\"outputs\":{\"FinalColor\":\"View.Color\"}}";
        ASSERT_TRUE(out.good());
    }

    void Build(bool preload = true)
    {
        // These older tests exercise compilation and reports, not asynchronous readiness.
        // Pending-load tests explicitly bypass this fixture preparation.
        if (preload)
        {
            auto& assets = EngineCore::GetInstance().GetAssetManager();
            auto path = services->Spine().GetActiveRenderPipelinePath();
            if (path.empty())
                path = "RenderPipelines/ForwardPlus.rendergraph";
            const auto requested = assets.ResolveAssetGuid(path);
            if (!requested.IsNull())
                assets.LoadAssetAsync(requested).get();
            if (MountsEditorAssets() && fs::exists(standInFile))
            {
                const auto standIn = assets.ResolveAssetGuid("RenderPipelines/ForwardPlus.rendergraph", kAssetSourceAliasEditor);
                if (!standIn.IsNull())
                    assets.LoadAssetAsync(standIn).get();
            }
        }
        frame->BeginFrame(frameIndex++);
        TextureDesc color{};
        color.width = color.height = 32;
        color.depth = color.mipLevels = color.arrayLayers = color.sampleCount = 1;
        color.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        color.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
        auto depth = color;
        depth.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
        depth.usage = static_cast<uint32_t>(TextureUsage::DepthStencil);
        const ViewTargetsRG targets{view, frame->ImportPersistentTexture("Registration.Color", color), frame->ImportPersistentTexture("Registration.Depth", depth), {}};
        services->BeginWorldDrawFrame();
        services->BuildWorldBatchKeys();
        FrameOrchestrator::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(&targets, 1);
        services->Spine().BuildFrameGraph(*frame, params);
        Logger::Log::Flush();
    }

    bool Register(const char* name)
    {
        return services->Spine().RegisterPipelineNodeType(name, [this]
                                                          { return std::make_unique<LateNode>(counters); }, true);
    }

    // Rejected compiles of the requested pipeline. One logs a "was rejected:" line per error, the
    // lines together, so a run of them is one rejection.
    size_t Rejections() const
    {
        size_t rejections = 0;
        bool inRun = false;
        for (const std::string& line : messages)
        {
            const bool rejected = line.find("was rejected:") != std::string::npos;
            if (rejected && !inRun)
                ++rejections;
            inRun = rejected;
        }
        return rejections;
    }

    // Log lines that apply a pipeline or announce a stand-in, after a flush.
    size_t AppliedOrFallbackLines()
    {
        Logger::Log::Flush();
        return TestLog::CountLinesContaining(messages, "applying");
    }

    // The orchestrator's own pipeline failure lines ("RenderPipeline '<path>' was
    // rejected / failed to load / ..."), after a flush. A startup resolve logs
    // none: the caller reports the failure once.
    size_t OrchestratorFailureLines()
    {
        Logger::Log::Flush();
        return TestLog::CountLinesContaining(messages, "RenderPipeline '");
    }

    // Writes `text` as an asset-relative file under the project's Assets.
    void WriteAsset(const char* relativePath, const char* text)
    {
        std::ofstream out(root / "Assets" / relativePath, std::ios::binary | std::ios::trunc);
        out << text;
        ASSERT_TRUE(out.good());
    }
};

// Exercise an unpinned last-good pipeline as well as the initial default request.
class PendingPipelineRegistrationTest : public PipelineRegistrationTest, public testing::WithParamInterface<bool> {};

TEST_P(PendingPipelineRegistrationTest, PendingRequestKeepsDrawingThenAppliesOnce)
{
    ASSERT_TRUE(Register("Ready"));
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    if (GetParam())
    {
        Write(R"({"id":"Ready","type":"Ready"})", "LastGood");
        Build();
        ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
        pipelineFile = root / "Assets/RenderPipelines/Replacement.rendergraph";
        services->Spine().SetActiveRenderPipelinePath("RenderPipelines/Replacement.rendergraph");
    }
    Write(R"({"id":"Ready","type":"Ready"})", "AfterLoad");
    auto held = Hold(pipelineFile);
    ASSERT_NE(held, nullptr);
    const auto guid = assets.ResolveAssetGuid(pipelineFile);
    ASSERT_FALSE(guid.IsNull());
    auto& loading = StartHeldLoad(guid);
    ASSERT_TRUE(held->WaitForEntry());
    const auto generation = services->Spine().ActiveBlueprintGen();
    Build(false);
    EXPECT_FALSE(held->TimedOut.load());
    EXPECT_EQ(OrchestratorFailureLines(), 0u);
    if (GetParam())
    {
        ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
        EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "LastGood");
        EXPECT_EQ(services->Spine().ActiveBlueprintGen(), generation);
    }
    held->Release();
    ASSERT_NE(loading.get(), nullptr);
    Build(false);
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "AfterLoad");
    const auto applied = services->Spine().ActiveBlueprintGen();
    Build(false);
    EXPECT_EQ(services->Spine().ActiveBlueprintGen(), applied);
    EXPECT_EQ(held->Entered.load(), 1u);
}

INSTANTIATE_TEST_SUITE_P(DefaultAndLastGood, PendingPipelineRegistrationTest, testing::Bool());

TEST_F(PipelineRegistrationTest, ChangingPathWhilePendingNeverAppliesTheOldRequest)
{
    ASSERT_TRUE(Register("Ready"));
    Write(R"({"id":"Ready","type":"Ready"})", "Abandoned");
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    auto held = Hold(pipelineFile);
    ASSERT_NE(held, nullptr);
    const auto oldGuid = assets.ResolveAssetGuid(pipelineFile);
    ASSERT_FALSE(oldGuid.IsNull());
    auto& oldLoad = StartHeldLoad(oldGuid);
    ASSERT_TRUE(held->WaitForEntry());
    Build(false);
    EXPECT_FALSE(held->TimedOut.load());
    pipelineFile = root / "Assets/RenderPipelines/Selected.rendergraph";
    Write(R"({"id":"Ready","type":"Ready"})", "Selected");
    ASSERT_NE(assets.LoadAssetAsync(assets.ResolveAssetGuid(pipelineFile)).get(), nullptr);
    services->Spine().SetActiveRenderPipelinePath("RenderPipelines/Selected.rendergraph");
    Build(false);
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "Selected");
    const auto generation = services->Spine().ActiveBlueprintGen();
    held->Release();
    ASSERT_NE(oldLoad.get(), nullptr);
    Build(false);
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "Selected");
    EXPECT_EQ(services->Spine().ActiveBlueprintGen(), generation);
}

TEST_F(PipelineRegistrationTest, StartupWaitsForPendingRequestedDecodeBeforeReturning)
{
    ASSERT_TRUE(Register("Ready"));
    Write(R"({"id":"Ready","type":"Ready"})", "StartupPipeline");
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    auto held = Hold(pipelineFile);
    ASSERT_NE(held, nullptr);
    const auto guid = assets.ResolveAssetGuid(pipelineFile);
    ASSERT_FALSE(guid.IsNull());
    auto& loading = StartHeldLoad(guid);
    ASSERT_TRUE(held->WaitForEntry());
    std::promise<void> returned;
    auto completed = returned.get_future();
    bool returnedBeforeRelease = false;
    std::thread release([&]
    {
        returnedBeforeRelease = completed.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
        held->Release();
    });
    const auto result = services->Spine().ResolveActiveRenderPipelineNow("RenderPipelines/ForwardPlus.rendergraph");
    returned.set_value();
    release.join();
    EXPECT_FALSE(returnedBeforeRelease);
    EXPECT_FALSE(held->TimedOut.load());
    EXPECT_EQ(result.Failure, PipelineResolveFailure::None) << result.Message;
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "StartupPipeline");
    EXPECT_NE(loading.get(), nullptr);
}

TEST_F(PipelineRegistrationTest, PendingStandInDoesNotReportFailureAndRetries)
{
    Write(R"({"id":"Missing","type":"Missing"})");
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    ASSERT_NE(assets.LoadAssetAsync(assets.ResolveAssetGuid(pipelineFile)).get(), nullptr);
    auto held = Hold(standInFile);
    ASSERT_NE(held, nullptr);
    const auto guid = assets.ResolveAssetGuid("RenderPipelines/ForwardPlus.rendergraph", kAssetSourceAliasEditor);
    ASSERT_FALSE(guid.IsNull());
    auto& loading = StartHeldLoad(guid);
    ASSERT_TRUE(held->WaitForEntry());
    Build(false);
    EXPECT_FALSE(held->TimedOut.load());
    EXPECT_EQ(TestLog::CountLinesContaining(messages, "cannot stand in for"), 0u);
    held->Release();
    ASSERT_NE(loading.get(), nullptr);
    Build(false);
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "EngineStandIn");
    const auto generation = services->Spine().ActiveBlueprintGen();
    Build(false);
    EXPECT_EQ(services->Spine().ActiveBlueprintGen(), generation);
    EXPECT_EQ(held->Entered.load(), 1u);
}

TEST_F(PipelineRegistrationTest, DefaultPathRecoversAfterFirstModuleRegistrationWithoutSourceEdit)
{
    Write(R"({"id":"ProjectPass","type":"ProjectLate"})");
    const auto time = fs::last_write_time(pipelineFile);
    Build();
    ASSERT_TRUE(services->Spine().LastCompileReport().Rejected);
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr) << "nothing stands in for the refused pipeline";
    ASSERT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "EngineStandIn");
    const auto sourceHash = services->Spine().LastCompileReport().SourceHash;
    ASSERT_NE(sourceHash, 0u);
    Build();
    ASSERT_EQ(Rejections(), 1u);

    ECS::SetActiveRegistrationModule("LateProjectModule", 1);
    ASSERT_TRUE(Register("ProjectLate"));
    ECS::ClearActiveRegistrationModule();
    services->Spine().ReconcileModuleNodeRegistrations("LateProjectModule", 1);
    Build();
    EXPECT_FALSE(services->Spine().LastCompileReport().Rejected);
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "AuthoredLatePipeline");
    EXPECT_EQ(services->Spine().LastCompileReport().SourceHash, sourceHash);
    EXPECT_EQ(fs::last_write_time(pipelineFile), time);
    EXPECT_EQ(counters.Constructed, 1u);
    EXPECT_EQ(counters.Declared, 1u);
    const auto generation = services->Spine().ActiveBlueprintGen();
    const auto reportGeneration = services->Spine().LastCompileReport().Generation;
    ASSERT_TRUE(Register("UnrelatedAfterSuccess"));
    Build();
    Build();
    EXPECT_EQ(services->Spine().ActiveBlueprintGen(), generation);
    EXPECT_EQ(services->Spine().LastCompileReport().Generation, reportGeneration);
    EXPECT_EQ(counters.Constructed, 1u);
    EXPECT_EQ(counters.Declared, 3u);
    EXPECT_EQ(Rejections(), 1u);
}

// The Player's startup order: a module registers its node type, then the
// pipeline is resolved, before any frame. The pipeline is the active one at
// the first frame, which neither recompiles nor re-applies it.
TEST_F(PipelineRegistrationTest, ResolvedAfterModuleRegistrationIsActiveBeforeTheFirstFrame)
{
    Write(R"({"id":"GameFog","type":"GameFog"})", "GamePipeline");
    ASSERT_TRUE(Register("GameFog"));

    const PipelineResolveResult result =
        services->Spine().ResolveActiveRenderPipelineNow("RenderPipelines/ForwardPlus.rendergraph");

    EXPECT_EQ(result.Failure, PipelineResolveFailure::None) << result.Message;
    EXPECT_TRUE(result.Message.empty()) << result.Message;
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "GamePipeline");
    const auto generation = services->Spine().ActiveBlueprintGen();
    Build();
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "GamePipeline");
    EXPECT_EQ(services->Spine().ActiveBlueprintGen(), generation);
}

// A node type no module provides is a startup error naming the pipeline and the type.
TEST_F(PipelineRegistrationTest, ResolveNamesTheNodeTypeNoModuleProvides)
{
    Write(R"({"id":"Fog","type":"MissionFog"})", "GamePipeline");

    const PipelineResolveResult result =
        services->Spine().ResolveActiveRenderPipelineNow("RenderPipelines/ForwardPlus.rendergraph");

    EXPECT_EQ(result.Failure, PipelineResolveFailure::Rejected) << result.Message;
    const std::string& error = result.Message;
    EXPECT_NE(error.find("'RenderPipelines/ForwardPlus.rendergraph' was rejected"), std::string::npos) << error;
    EXPECT_NE(error.find("Unknown pass type 'MissionFog' (node 'Fog')"), std::string::npos) << error;
    EXPECT_NE(error.back(), '.') << "the caller ends the sentence: " << error;
    // The compiler's fix is the editor's; the caller gives its own.
    EXPECT_EQ(error.find("build the scripts"), std::string::npos) << error;
    // The resolve reports instead of standing in: the engine's pipeline is not
    // applied, and the spine publishes no stand-in report.
    EXPECT_EQ(services->Spine().ActiveBlueprint(), nullptr);
    EXPECT_FALSE(services->Spine().ActivePipelineStandIn().IsActive());
    EXPECT_EQ(AppliedOrFallbackLines(), 0u);
    EXPECT_EQ(OrchestratorFailureLines(), 0u);
}

// A pipeline file in no asset mount is a startup error naming the file. The
// project's pipeline and the engine's are both valid, and neither stands in.
TEST_F(PipelineRegistrationTest, ResolveNamesAPipelineInNoMount)
{
    ASSERT_TRUE(Register("Ready"));
    Write(R"({"id":"Ready","type":"Ready"})", "DefaultStandIn");
    ASSERT_FALSE(fs::exists(root / "Assets/RenderPipelines/Missing.rendergraph"));

    const PipelineResolveResult result =
        services->Spine().ResolveActiveRenderPipelineNow("RenderPipelines/Missing.rendergraph");

    EXPECT_EQ(result.Failure, PipelineResolveFailure::NotInMount) << result.Message;
    EXPECT_NE(result.Message.find("'RenderPipelines/Missing.rendergraph' is in no asset mount"), std::string::npos)
        << result.Message;
    EXPECT_EQ(services->Spine().ActiveBlueprint(), nullptr);
    EXPECT_EQ(AppliedOrFallbackLines(), 0u);
    EXPECT_EQ(OrchestratorFailureLines(), 0u);
}

// The asset registry can still list a pipeline that no mount holds on disk (a
// stale asset database); its load then fails. That is a missing file, with the
// missing-file fix, not a load failure.
TEST_F(PipelineRegistrationTest, ResolveReportsAFileTheRegistryListsButNoMountHoldsAsMissing)
{
    WriteAsset("RenderPipelines/Stale.rendergraph",
               R"({"schemaVersion":2,"pipelineName":"Stale","passes":[],"outputs":{"FinalColor":"View.Color"}})");
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    ASSERT_FALSE(assets.ResolveAssetGuid("RenderPipelines/Stale.rendergraph").IsNull());
    const fs::path resolved = assets.ResolveAssetPath("RenderPipelines/Stale.rendergraph");
    ASSERT_TRUE(fs::remove(resolved));
    ASSERT_FALSE(assets.GetRegistry().GetAssetGUID(resolved).IsNull()) << "the registry must still list the file";

    const PipelineResolveResult result =
        services->Spine().ResolveActiveRenderPipelineNow("RenderPipelines/Stale.rendergraph");

    EXPECT_EQ(result.Failure, PipelineResolveFailure::NotInMount) << result.Message;
    EXPECT_NE(result.Message.find("'RenderPipelines/Stale.rendergraph' is in no asset mount"), std::string::npos)
        << result.Message;
    EXPECT_EQ(OrchestratorFailureLines(), 0u);
}

// A pipeline file a mount holds but that does not parse is a load failure.
TEST_F(PipelineRegistrationTest, ResolveReportsAMalformedFileAsLoadFailed)
{
    WriteAsset("RenderPipelines/Malformed.rendergraph", "{ this is not json");

    const PipelineResolveResult result =
        services->Spine().ResolveActiveRenderPipelineNow("RenderPipelines/Malformed.rendergraph");

    EXPECT_EQ(result.Failure, PipelineResolveFailure::LoadFailed) << result.Message;
    EXPECT_NE(result.Message.find("'RenderPipelines/Malformed.rendergraph' failed to load"), std::string::npos)
        << result.Message;
    // The asset's own parse error is the reason, not a generic "failed to parse/load".
    EXPECT_NE(result.Message.find("JSON parse failed"), std::string::npos) << result.Message;
    EXPECT_EQ(services->Spine().ActiveBlueprint(), nullptr);
    EXPECT_EQ(OrchestratorFailureLines(), 0u);
}

// A resident pipeline whose in-place reload no longer parses is a load failure
// that names the parse error too.
TEST_F(PipelineRegistrationTest, ResolveReportsAResidentPipelineThatNoLongerParsesAsLoadFailed)
{
    ASSERT_TRUE(Register("Ready"));
    Write(R"({"id":"Ready","type":"Ready"})", "LastGood");
    ASSERT_EQ(services->Spine().ResolveActiveRenderPipelineNow("RenderPipelines/ForwardPlus.rendergraph").Failure,
              PipelineResolveFailure::None);
    WriteAsset("RenderPipelines/ForwardPlus.rendergraph", "{ this is not json");
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const auto guid = assets.ResolveAssetGuid("RenderPipelines/ForwardPlus.rendergraph");
    ASSERT_FALSE(guid.IsNull());
    ASSERT_EQ(assets.ReloadAssetNow(guid), ReloadOutcome::Failed);

    const PipelineResolveResult result =
        services->Spine().ResolveActiveRenderPipelineNow("RenderPipelines/ForwardPlus.rendergraph");

    EXPECT_EQ(result.Failure, PipelineResolveFailure::LoadFailed) << result.Message;
    // The asset's error without its type prefix and closing period: the caller ends the sentence.
    EXPECT_EQ(result.Message,
              "render pipeline 'RenderPipelines/ForwardPlus.rendergraph' failed to load: "
              "JSON parse failed (malformed JSON)");
    EXPECT_EQ(OrchestratorFailureLines(), 0u);
}

TEST_F(PipelineRegistrationTest, StillInvalidRetriesOnceAndRejectedRegistrationsDoNotRetry)
{
    Write(R"({"id":"A","type":"LateA"},{"id":"B","type":"LateB"})");
    Build();
    ASSERT_EQ(Rejections(), 1u);
    ASSERT_TRUE(Register("LateA"));
    Build();
    EXPECT_TRUE(services->Spine().LastCompileReport().Rejected);
    EXPECT_EQ(Rejections(), 2u);
    Build();
    Build();
    EXPECT_EQ(Rejections(), 2u);
    EXPECT_FALSE(Register("LateA"));
    EXPECT_FALSE(Register(""));
    EXPECT_FALSE(services->Spine().RegisterPipelineNodeType("NoFactory", {}, true));
    services->Spine().ReconcileModuleNodeRegistrations("Unrelated", 1);
    Build();
    EXPECT_EQ(Rejections(), 2u);
    ASSERT_TRUE(Register("LateB"));
    Build();
    EXPECT_FALSE(services->Spine().LastCompileReport().Rejected);
    EXPECT_EQ(counters.Constructed, 2u);
}

TEST_F(PipelineRegistrationTest, SuccessfulRegistrationsCoalesceAndPinnedBlueprintKeepsOwnership)
{
    Write(R"({"id":"A","type":"LateA"},{"id":"B","type":"LateB"})");
    Build();
    ASSERT_TRUE(Register("LateA"));
    ASSERT_TRUE(Register("LateB"));
    Build();
    EXPECT_EQ(Rejections(), 1u);
    EXPECT_FALSE(services->Spine().LastCompileReport().Rejected);
    EXPECT_EQ(counters.Constructed, 2u);

    // Pinning after a rejection retains its diagnostic state. A subsequent
    // registration must respect the explicit programmatic owner as well.
    Write(R"({"id":"StillMissing","type":"AfterPin"})");
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const auto guid = assets.ResolveAssetGuid("RenderPipelines/ForwardPlus.rendergraph");
    ASSERT_EQ(assets.ReloadAssetNow(guid), ReloadOutcome::Reloaded);
    Build();
    ASSERT_TRUE(services->Spine().LastCompileReport().Rejected);
    ASSERT_EQ(Rejections(), 2u);
    RenderPipelineBlueprint pinned{};
    pinned.pipelineName = "ProgrammaticOwner";
    pinned.contentHash = 1234;
    services->Spine().SetActiveRenderPipelineBlueprint(std::move(pinned));
    const auto generation = services->Spine().ActiveBlueprintGen();
    ASSERT_TRUE(Register("AfterPin"));
    Build();
    Build();
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "ProgrammaticOwner");
    EXPECT_EQ(services->Spine().ActiveBlueprintGen(), generation);
    EXPECT_EQ(Rejections(), 2u);
}

TEST_F(PipelineRegistrationTest, RetainsLastGoodUntilMissingTypeArrivesAndUsesTheResidentSource)
{
    ASSERT_TRUE(Register("Ready"));
    Write(R"({"id":"Ready","type":"Ready"})", "LastGood");
    Build();
    ASSERT_FALSE(services->Spine().LastCompileReport().Rejected);
    ASSERT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "LastGood");
    const auto goodGeneration = services->Spine().ActiveBlueprintGen();

    Write(R"({"id":"Ready","type":"Ready"},{"id":"Late","type":"Late"})", "Replacement");
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const auto guid = assets.ResolveAssetGuid("RenderPipelines/ForwardPlus.rendergraph");
    ASSERT_FALSE(guid.IsNull());
    ASSERT_EQ(assets.ReloadAssetNow(guid), ReloadOutcome::Reloaded);
    const auto resident = assets.GetAsset(guid);
    Build();
    ASSERT_TRUE(services->Spine().LastCompileReport().Rejected);
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "LastGood");
    EXPECT_EQ(services->Spine().ActiveBlueprintGen(), goodGeneration);
    const auto rejectedHash = services->Spine().LastCompileReport().SourceHash;
    const auto time = fs::last_write_time(pipelineFile);
    Build();
    EXPECT_EQ(Rejections(), 1u);

    ASSERT_TRUE(Register("Late"));
    Build();
    EXPECT_FALSE(services->Spine().LastCompileReport().Rejected);
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "Replacement");
    EXPECT_EQ(services->Spine().LastCompileReport().SourceHash, rejectedHash);
    EXPECT_EQ(assets.GetAsset(guid).get(), resident.get());
    EXPECT_EQ(fs::last_write_time(pipelineFile), time);
    EXPECT_GT(services->Spine().ActiveBlueprintGen(), goodGeneration);
}
} // namespace

// A refused request leaves the views drawing with a stand-in. The spine says which pipeline was
// refused, what draws instead and the compiler's reason as a sentence; the refusal is one error line
// and one episode however often the request is compiled again, and it clears when the request
// compiles.
TEST_F(PipelineRegistrationTest, ARefusedPipelineReportsTheStandInDrawingInItsPlace)
{
    Write(R"({"id":"ProjectPass","type":"ProjectLate"})");
    Build();
    Build();
    const PipelineStandIn& standIn = services->Spine().ActivePipelineStandIn();
    ASSERT_TRUE(standIn.IsActive()) << "the views draw a stand-in and the spine does not say so";
    EXPECT_EQ(standIn.RequestedPath.generic_string(), "RenderPipelines/ForwardPlus.rendergraph");
    EXPECT_EQ(standIn.StandInName, "EngineStandIn") << "the project's file of the same name stood in";
    EXPECT_TRUE(standIn.StandInFailure.empty()) << standIn.StandInFailure;
    EXPECT_EQ(standIn.Reason.rfind("Unknown pass type 'ProjectLate'", 0), 0u) << standIn.Reason;
    EXPECT_EQ(standIn.Reason.back(), '.') << standIn.Reason;
    const uint64_t generation = standIn.Generation;

    // A registration the request does not need compiles it again, refused for the same reason.
    ASSERT_TRUE(Register("UnrelatedLate"));
    Build();
    Build();
    EXPECT_EQ(services->Spine().ActivePipelineStandIn().Generation, generation)
        << "a second compile of the same refusal started a new episode";
    EXPECT_EQ(TestLog::CountLinesContaining(messages, "was rejected"), 1u)
        << "the refusal is logged once, not once per compile";

    ECS::SetActiveRegistrationModule("LateProjectModule", 1);
    ASSERT_TRUE(Register("ProjectLate"));
    ECS::ClearActiveRegistrationModule();
    services->Spine().ReconcileModuleNodeRegistrations("LateProjectModule", 1);
    Build();
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "AuthoredLatePipeline");
    EXPECT_FALSE(services->Spine().ActivePipelineStandIn().IsActive());
    EXPECT_GT(services->Spine().ActivePipelineStandIn().Generation, generation);
}

// A pipeline that does not load leaves the engine's pipeline standing in, and the spine's report
// names why: the asset manager's decode error, which carries the parse error. Startup
// observes the first failure; subsequent nonblocking frames report its suppressed retry.
TEST_F(PipelineRegistrationTest, AStandInForAMalformedPipelineNamesTheParseError)
{
    WriteAsset("RenderPipelines/Malformed.rendergraph", "{ this is not json");
    services->Spine().SetActiveRenderPipelinePath("RenderPipelines/Malformed.rendergraph");
    const auto first = services->Spine().ResolveActiveRenderPipelineNow("RenderPipelines/Malformed.rendergraph");
    EXPECT_EQ(first.Failure, PipelineResolveFailure::LoadFailed) << first.Message;
    EXPECT_NE(first.Message.find("JSON parse failed"), std::string::npos) << first.Message;
    Build();
    const PipelineStandIn& standIn = services->Spine().ActivePipelineStandIn();
    ASSERT_TRUE(standIn.IsActive()) << "the views draw a stand-in and the spine does not say so";
    EXPECT_EQ(standIn.StandInName, "EngineStandIn");
    EXPECT_EQ(standIn.Reason.rfind("Its last load failed (", 0), 0u) << standIn.Reason;
    EXPECT_NE(standIn.Reason.find("JSON parse failed"), std::string::npos) << standIn.Reason;

    Build();
    const PipelineStandIn& suppressed = services->Spine().ActivePipelineStandIn();
    ASSERT_TRUE(suppressed.IsActive());
    EXPECT_EQ(suppressed.Reason.rfind("Its last load failed (", 0), 0u) << suppressed.Reason;
    EXPECT_NE(suppressed.Reason.find("JSON parse failed"), std::string::npos) << suppressed.Reason;
}

// While the project's scripts build, a pass whose type they register waits and the rest of the
// requested pipeline draws: the spine names the waiting pass, logs no refusal, error or warning,
// and compiles nothing on an unchanged frame. Once the scripts are in and the type is still
// unknown, the request is refused and the engine's pipeline stands in.
TEST_F(PipelineRegistrationTest, AScriptPassWaitsWhileTheRestOfThePipelineDraws)
{
    NativeScripting::NativeScriptManager* modules = EngineCore::GetInstance().GetNativeScriptManager();
    ASSERT_NE(modules, nullptr);
    NativeScripting::NativeBuildConfig pendingModule{};
    pendingModule.ModuleName = "PendingPackage";
    modules->SetPackageModuleConfigs({pendingModule});
    ASSERT_TRUE(modules->AreModulesPending());

    ASSERT_TRUE(Register("Ready"));
    Write(R"({"id":"Ready","type":"Ready"},{"id":"ProjectPass","type":"ProjectLate"})");
    // A startup resolve does not wait: it refuses the type no loaded module registers, and applies
    // nothing, not the rest of the pipeline without the waiting pass.
    const PipelineResolveResult resolved =
        services->Spine().ResolveActiveRenderPipelineNow("RenderPipelines/ForwardPlus.rendergraph");
    EXPECT_EQ(resolved.Failure, PipelineResolveFailure::Rejected) << resolved.Message;
    EXPECT_NE(resolved.Message.find("Unknown pass type 'ProjectLate'"), std::string::npos) << resolved.Message;
    EXPECT_EQ(services->Spine().ActiveBlueprint(), nullptr) << "the resolve applied a partial pipeline";
    // Warnings and errors only, for the wait: the fixture's capture holds every level.
    std::vector<std::string> problems;
    capture.reset();
    capture = std::make_unique<TestLog::ScopedEngineLogCapture>(&problems, Logger::LogLevel::Warning);
    Build();
    Build();
    const auto* active = services->Spine().ActiveBlueprint();
    ASSERT_NE(active, nullptr) << "nothing draws while the scripts build";
    EXPECT_EQ(active->pipelineName, "AuthoredLatePipeline") << "the request does not draw while its scripts build";
    ASSERT_EQ(active->passes.size(), 1u);
    EXPECT_EQ(active->passes[0].id, "Ready");
    EXPECT_GT(counters.Declared, 0u) << "the pass that needs no script did not draw";
    EXPECT_FALSE(services->Spine().LastCompileReport().Rejected);
    const auto& issues = services->Spine().LastCompileReport().Issues;
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].code, PipelineIssueCode::PendingScriptPass);
    EXPECT_EQ(issues[0].nodeId, "ProjectPass");

    const PipelineStandIn& waiting = services->Spine().ActivePipelineStandIn();
    ASSERT_TRUE(waiting.IsActive());
    EXPECT_TRUE(waiting.WaitingForModules);
    EXPECT_TRUE(waiting.StandInName.empty()) << waiting.StandInName;
    EXPECT_EQ(waiting.PendingPasses, std::vector<std::string>{"ProjectPass"});
    EXPECT_EQ(waiting.Reason, "Its script pass 'ProjectPass' joins once the project's scripts are built.");

    const uint64_t compiles = services->Spine().LastCompileReport().Compiles;
    for (int step = 0; step < 10; ++step)
        Build();
    EXPECT_EQ(services->Spine().LastCompileReport().Compiles, compiles) << "an unchanged frame of the wait compiled";
    Logger::Log::Flush();
    std::string allProblems;
    for (const std::string& line : problems)
        allProblems += line + "\n";
    // The fixture's world logs warnings of its own (no staged adapter shaders, no world id); the
    // pipeline must log none.
    EXPECT_EQ(TestLog::CountLinesContaining(problems, "RenderPipeline"), 0u)
        << "the wait logged a pipeline warning or error:\n" << allProblems;
    EXPECT_EQ(TestLog::CountLinesContaining(problems, "ForwardPlus.rendergraph"), 0u)
        << "the wait logged a warning or error about the request:\n" << allProblems;

    capture.reset();
    capture = std::make_unique<TestLog::ScopedEngineLogCapture>(&messages);
    modules->SetPackageModuleConfigs({});
    ASSERT_FALSE(modules->AreModulesPending());
    Build();
    const PipelineStandIn& refused = services->Spine().ActivePipelineStandIn();
    EXPECT_FALSE(refused.WaitingForModules);
    EXPECT_EQ(refused.StandInName, "EngineStandIn") << "a request drawing without a type that never came stays";
    EXPECT_NE(refused.Reason.find("the project's scripts built without registering it"), std::string::npos)
        << "after the build, the refusal still asks for a build: " << refused.Reason;
    EXPECT_EQ(refused.Reason.find("build the scripts"), std::string::npos) << refused.Reason;
    EXPECT_EQ(TestLog::CountLinesContaining(messages, "was rejected"), 1u);
}

// A request with a real error besides a waiting script pass is refused at once: waiting would hide
// the error and promise a pipeline that cannot come. The reason is the real error.
TEST_F(PipelineRegistrationTest, ARealErrorBesideAScriptPassIsRefusedAtOnce)
{
    NativeScripting::NativeScriptManager* modules = EngineCore::GetInstance().GetNativeScriptManager();
    ASSERT_NE(modules, nullptr);
    NativeScripting::NativeBuildConfig pendingModule{};
    pendingModule.ModuleName = "PendingPackage";
    modules->SetPackageModuleConfigs({pendingModule});
    ASSERT_TRUE(modules->AreModulesPending());

    ASSERT_TRUE(Register("Ready"));
    Write(R"({"id":"ProjectPass","type":"ProjectLate"},{"id":"Twice","type":"Ready"},{"id":"Twice","type":"Ready"})");
    Build();
    Build();
    const PipelineStandIn& refused = services->Spine().ActivePipelineStandIn();
    ASSERT_TRUE(refused.IsActive());
    EXPECT_FALSE(refused.WaitingForModules) << "a certain refusal waited for the scripts";
    EXPECT_EQ(refused.StandInName, "EngineStandIn");
    EXPECT_EQ(refused.Reason.rfind("Duplicate pass id 'Twice'", 0), 0u) << refused.Reason;
    EXPECT_EQ(TestLog::CountLinesContaining(messages, "was rejected: Duplicate pass id 'Twice'"), 1u);
}

// When the request's output comes from a waiting script pass, nothing of it can draw while the
// scripts build: the engine's pipeline stands in, the note names the pass, and nothing is refused.
TEST_F(PipelineRegistrationTest, AnOutputFromAWaitingPassWaitsBehindTheStandIn)
{
    NativeScripting::NativeScriptManager* modules = EngineCore::GetInstance().GetNativeScriptManager();
    ASSERT_NE(modules, nullptr);
    NativeScripting::NativeBuildConfig pendingModule{};
    pendingModule.ModuleName = "PendingPackage";
    modules->SetPackageModuleConfigs({pendingModule});
    ASSERT_TRUE(modules->AreModulesPending());

    ASSERT_TRUE(Register("Ready"));
    {
        std::ofstream out(pipelineFile, std::ios::binary | std::ios::trunc);
        out << R"({"schemaVersion":2,"pipelineName":"AuthoredLatePipeline",)"
            << R"("resources":{"FogColor":{"kind":"Texture","format":"RGBA8_UNORM"}},)"
            << R"("passes":[{"id":"Ready","type":"Ready"},{"id":"Fog","type":"ProjectLate","output":"FogColor"}],)"
            << R"("outputs":{"FinalColor":"FogColor"}})";
        ASSERT_TRUE(out.good());
    }
    Build();
    Build();
    const PipelineStandIn& waiting = services->Spine().ActivePipelineStandIn();
    ASSERT_TRUE(waiting.IsActive());
    EXPECT_TRUE(waiting.WaitingForModules);
    EXPECT_EQ(waiting.StandInName, "EngineStandIn") << "a request with nothing to draw was applied";
    EXPECT_EQ(waiting.PendingPasses, std::vector<std::string>{"Fog"});
    EXPECT_EQ(TestLog::CountLinesContaining(messages, "was rejected"), 0u) << "the wait was logged as a refusal";
}

// A request that drew in full before and now waits with its output coming from a waiting pass keeps
// drawing that last version, and the note says so.
TEST_F(PipelineRegistrationTest, AnOutputFromAWaitingPassKeepsTheLastVersionDrawing)
{
    NativeScripting::NativeScriptManager* modules = EngineCore::GetInstance().GetNativeScriptManager();
    ASSERT_NE(modules, nullptr);
    NativeScripting::NativeBuildConfig pendingModule{};
    pendingModule.ModuleName = "PendingPackage";
    modules->SetPackageModuleConfigs({pendingModule});
    ASSERT_TRUE(modules->AreModulesPending());

    ASSERT_TRUE(Register("Ready"));
    Write(R"({"id":"Ready","type":"Ready"})", "LastGood");
    Build();
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
    ASSERT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "LastGood");

    {
        std::ofstream out(pipelineFile, std::ios::binary | std::ios::trunc);
        out << R"({"schemaVersion":2,"pipelineName":"Edited",)"
            << R"("resources":{"FogColor":{"kind":"Texture","format":"RGBA8_UNORM"}},)"
            << R"("passes":[{"id":"Ready","type":"Ready"},{"id":"Fog","type":"ProjectLate","output":"FogColor"}],)"
            << R"("outputs":{"FinalColor":"FogColor"}})";
        ASSERT_TRUE(out.good());
    }
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const auto guid = assets.ResolveAssetGuid("RenderPipelines/ForwardPlus.rendergraph");
    ASSERT_EQ(assets.ReloadAssetNow(guid), ReloadOutcome::Reloaded);
    Build();
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "LastGood");
    const PipelineStandIn& waiting = services->Spine().ActivePipelineStandIn();
    EXPECT_TRUE(waiting.WaitingForModules);
    EXPECT_TRUE(waiting.LastVersionDraws) << "the note would say the rest of the pipeline draws";
    EXPECT_TRUE(waiting.StandInName.empty()) << waiting.StandInName;
}

// After a script failed to build, an unknown pass type's fix is that build, in the Script Errors tab.
TEST(PipelineStandInReason, AnUnknownTypeAfterAFailedScriptBuildPointsAtTheScriptErrorsTab)
{
    EXPECT_EQ(FrameOrchestrator::PointAtFailedBuild(
                  "Unknown pass type 'MissionFog' (node 'Fog'): no loaded scripts register it; build the scripts or "
                  "install the package that provides it, or remove the node."),
              "Unknown pass type 'MissionFog' (node 'Fog'): no loaded scripts register it, and a script failed to "
              "build; fix its errors in the Script Errors tab.");
}

// The wait ends well: once a script registers the waiting pass's type, the full pipeline replaces
// the one drawing without it, with no refusal and no error.
TEST_F(PipelineRegistrationTest, AScriptPassJoinsWhenItsTypeRegisters)
{
    NativeScripting::NativeScriptManager* modules = EngineCore::GetInstance().GetNativeScriptManager();
    ASSERT_NE(modules, nullptr);
    NativeScripting::NativeBuildConfig pendingModule{};
    pendingModule.ModuleName = "PendingPackage";
    modules->SetPackageModuleConfigs({pendingModule});
    ASSERT_TRUE(modules->AreModulesPending());

    ASSERT_TRUE(Register("Ready"));
    Write(R"({"id":"Ready","type":"Ready"},{"id":"ProjectPass","type":"ProjectLate"})");
    Build();
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
    ASSERT_EQ(services->Spine().ActiveBlueprint()->passes.size(), 1u);
    const uint64_t generation = services->Spine().ActiveBlueprintGen();
    const unsigned constructedBefore = counters.Constructed;

    ECS::SetActiveRegistrationModule("PendingPackage", 1);
    ASSERT_TRUE(Register("ProjectLate"));
    ECS::ClearActiveRegistrationModule();
    services->Spine().ReconcileModuleNodeRegistrations("PendingPackage", 1);
    Build();
    const auto* active = services->Spine().ActiveBlueprint();
    ASSERT_NE(active, nullptr);
    EXPECT_EQ(active->pipelineName, "AuthoredLatePipeline");
    ASSERT_EQ(active->passes.size(), 2u) << "the registration did not add the waiting pass";
    EXPECT_EQ(active->passes[1].id, "ProjectPass");
    EXPECT_GT(services->Spine().ActiveBlueprintGen(), generation);
    EXPECT_GT(counters.Constructed, constructedBefore) << "the script pass's node was not created";
    EXPECT_FALSE(services->Spine().ActivePipelineStandIn().IsActive());

    modules->SetPackageModuleConfigs({});
    Build();
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
    EXPECT_EQ(services->Spine().ActiveBlueprint()->passes.size(), 2u);
    EXPECT_FALSE(services->Spine().ActivePipelineStandIn().IsActive());
    EXPECT_EQ(TestLog::CountLinesContaining(messages, "was rejected"), 0u) << "the wait ended in a refusal line";
}

// The stand-in is compiled once while it stands in, and again when its source changes.
TEST_F(PipelineRegistrationTest, AnEditedStandInIsCompiledAgain)
{
    Write(R"({"id":"ProjectPass","type":"ProjectLate"})");
    Build();
    Build();
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
    ASSERT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "EngineStandIn");
    const uint64_t generation = services->Spine().ActiveBlueprintGen();

    {
        std::ofstream out(standInFile, std::ios::binary | std::ios::trunc);
        out << R"({"schemaVersion":2,"pipelineName":"EngineStandInEdited","passes":[{"id":"World","type":"WorldRender"}],)"
            << R"("outputs":{"FinalColor":"View.Resolve"}})";
        ASSERT_TRUE(out.good());
    }
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const auto guid = assets.ResolveAssetGuid("RenderPipelines/ForwardPlus.rendergraph", kAssetSourceAliasEditor);
    ASSERT_EQ(assets.ReloadAssetNow(guid), ReloadOutcome::Reloaded);
    Build();
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "EngineStandInEdited");
    EXPECT_EQ(services->Spine().ActiveBlueprintGen(), generation + 1);
}

// While a stand-in draws for a refused request, a frame that changes nothing compiles nothing: not
// the request, not the stand-in.
TEST_F(PipelineRegistrationTest, AStandInDrawingForARefusedRequestCompilesNothingMore)
{
    Write(R"({"id":"ProjectPass","type":"ProjectLate"})");
    Build();
    ASSERT_NE(services->Spine().ActiveBlueprint(), nullptr);
    ASSERT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "EngineStandIn");
    const uint64_t compiles = services->Spine().LastCompileReport().Compiles;
    ASSERT_EQ(compiles, 2u) << "the request and the stand-in compile once each";
    for (int step = 0; step < 10; ++step)
        Build();
    EXPECT_EQ(services->Spine().LastCompileReport().Compiles, compiles) << "an unchanged frame compiled a pipeline";
    EXPECT_EQ(services->Spine().ActiveBlueprint()->pipelineName, "EngineStandIn");
}

// A packaged game has no 'editor' mount and no stand-in: a refused pipeline draws nothing, even with a
// project file at the engine pipeline's path, and its own rejection is the one error line.
class PackagedGameRegistrationTest : public PipelineRegistrationTest
{
  protected:
    bool MountsEditorAssets() const override { return false; }
};

TEST_F(PackagedGameRegistrationTest, ARefusedPipelineDrawsNothingAndIsReportedOnce)
{
    ASSERT_TRUE(Register("Ready"));
    Write(R"({"id":"Ready","type":"Ready"})", "ProjectForwardPlus");
    const fs::path gameFile = root / "Assets/RenderPipelines/Game.rendergraph";
    {
        std::ofstream out(gameFile, std::ios::binary | std::ios::trunc);
        out << R"({"schemaVersion":2,"pipelineName":"Game","passes":[{"id":"ProjectPass","type":"ProjectLate"}],)"
            << R"("outputs":{"FinalColor":"View.Color"}})";
        ASSERT_TRUE(out.good());
    }
    services->Spine().SetActiveRenderPipelinePath("RenderPipelines/Game.rendergraph");
    std::vector<std::string> errors;
    capture.reset();
    capture = std::make_unique<TestLog::ScopedEngineLogCapture>(&errors, Logger::LogLevel::Error);
    Build();
    Build();
    const PipelineStandIn& standIn = services->Spine().ActivePipelineStandIn();
    ASSERT_TRUE(standIn.IsActive());
    EXPECT_TRUE(standIn.StandInName.empty()) << "a stand-in drew in a packaged game: " << standIn.StandInName;
    EXPECT_EQ(standIn.StandInFailure, "A packaged game has no stand-in pipeline.");
    EXPECT_EQ(services->Spine().ActiveBlueprint(), nullptr);
    std::string allErrors;
    for (const std::string& line : errors)
        allErrors += line + "\n";
    ASSERT_EQ(errors.size(), 1u) << "the refusal is not one error line:\n" << allErrors;
    EXPECT_NE(errors[0].find("'RenderPipelines/Game.rendergraph' was rejected: Unknown pass type 'ProjectLate'"),
              std::string::npos)
        << errors[0];
}

// When the engine's own pipeline cannot stand in either, nothing is drawn: the spine says why, and
// the console has one error line for it however often the request is compiled again.
TEST_F(PipelineRegistrationTest, AStandInThatCannotLoadLeavesNothingDrawnAndSaysWhy)
{
    fs::remove(standInFile);
    Write(R"({"id":"ProjectPass","type":"ProjectLate"})");
    Build();
    ASSERT_TRUE(Register("UnrelatedLate"));
    Build();
    const PipelineStandIn& standIn = services->Spine().ActivePipelineStandIn();
    ASSERT_TRUE(standIn.IsActive());
    EXPECT_TRUE(standIn.StandInName.empty()) << standIn.StandInName;
    EXPECT_EQ(standIn.StandInFailure.rfind("The engine's pipeline file is missing", 0), 0u) << standIn.StandInFailure;
    EXPECT_EQ(services->Spine().ActiveBlueprint(), nullptr);
    EXPECT_EQ(TestLog::CountLinesContaining(messages, "cannot stand in for"), 1u);
}
