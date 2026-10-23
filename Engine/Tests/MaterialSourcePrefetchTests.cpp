// The shader-source read MaterialSystem::PrefetchMaterialShaderSources submits
// on a tracked worker. The read is advisory: a read submitted after shutdown
// began is rejected by the tracked submission and its future is ready at once.
//
// PR 2037's paced scene-build registration (SceneBuildPump waiting for this read
// before registering a standalone material) was not carried into OpenEngine's
// rewritten SceneBuildPump, so its pump cases are not here.
//
// Owns an EngineCore lifecycle (the read runs on its job system), so it stays
// out of the EngineCore-free material aggregate.

#include "Assets/MaterialAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Scripting/ScriptsConfig.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <string>

using namespace GameEngine;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace GameEngine::Engine::Renderer
{
class MaterialSourcePrefetchTest : public testing::Test
{
  protected:
    static inline fs::path s_Workspace;
    static inline fs::path s_PreviousDirectory;
    Rendering::MaterialBuildContext m_Context;

    static void SetUpTestSuite()
    {
        s_PreviousDirectory = fs::current_path();
        s_Workspace = fs::temp_directory_path() / ("source-prefetch-" + GUID::Generate().ToString());
        fs::create_directories(s_Workspace / "Assets");
        auto& engine = EngineCore::GetInstance();
        ASSERT_FALSE(engine.IsInitialized());
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = s_Workspace.string();
        ASSERT_TRUE(engine.Initialize(config));
    }
    static void TearDownTestSuite()
    {
        EngineCore::GetInstance().Shutdown();
        fs::current_path(s_PreviousDirectory);
        std::error_code error;
        fs::remove_all(s_Workspace, error);
    }

    void SetUp() override
    {
        const fs::path root = s_Workspace / GUID::Generate().ToString();
        m_Context.AdapterShaderDir = root / "adapter";
        m_Context.ProjectRoots = {root / "project"};
        m_Context.CacheRoot = root / "cache";
        fs::create_directories(m_Context.AdapterShaderDir);
        fs::create_directories(m_Context.ProjectRoots[0]);
    }

    // The one private entry point this suite reaches for.
    static std::shared_future<void> Prefetch(MaterialSystem& materials, const MaterialDocument& doc)
    {
        return materials.PrefetchMaterialShaderSources(doc, {});
    }
};
} // namespace GameEngine::Engine::Renderer

using GameEngine::Engine::Renderer::MaterialSourcePrefetchTest;
using GameEngine::Engine::Renderer::MaterialSystem;

// A read submitted after shutdown began is dropped by the tracked submission;
// its future must be ready at once, or a caller polling it would wait forever.
TEST_F(MaterialSourcePrefetchTest, ReadRejectedAtShutdownIsReadyAtOnce)
{
    MaterialSystem materials;
    materials.SetMaterialBuildContext(m_Context);
    materials.ShutdownPhaseA();
    MaterialDocument doc;
    doc.surfaceShader = "surface.glsl";
    const std::shared_future<void> sourceRead = Prefetch(materials, doc);
    EXPECT_EQ(sourceRead.wait_for(0s), std::future_status::ready);
    EXPECT_THROW(sourceRead.get(), std::future_error);
    EXPECT_EQ(materials.GetPrewarmProgress().submitted, 0u);
}
