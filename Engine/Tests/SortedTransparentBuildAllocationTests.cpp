// The sorted transparent per-view build on a real device: once the view's retained staging has
// grown, a frame's build takes nothing from the heap (its working sets live in a ScratchScope).

#include "ExtractionHarness.h"
#include "Components/Rendering/RenderSorting.h"

#include "Memory/AllocationCountScope.h"
#include "Rendering/Core/GPUScene.h"

#include <cstdint>
#include <cstdlib>

namespace GameEngine::Testing::ExtractionFastPath
{
namespace
{

constexpr int kWarmupFrames = 8;
constexpr int kMeasuredFrames = 32;

// Two order-dependent Blend materials, so the build discovers two runs.
Material* RegisterBlendMaterial(ExtractionHarness& h, const GUID& guid, const char* name,
                                uint32_t gpuSceneMaterialIndex)
{
    MaterialDocument doc{};
    doc.materialName = name;
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    doc.alphaMode = MaterialAlphaMode::Blend;
    Material* material = h.rs.Materials().Registry().Register(guid, doc);
    if (!material)
        return nullptr;
    Material::TestFactory::SetGraphicsPipelineId(*material, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(*material, gpuSceneMaterialIndex);
    return material;
}

using SortedTransparentBuildAllocationTest = ExtractionFastPathTest;

TEST_F(SortedTransparentBuildAllocationTest, SortedTransparencyAllocatesNothingPerViewAfterWarmup)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), /*fastPath=*/true));
    if (!m_Device->GetCapabilities().supportsBufferDeviceAddress)
        GTEST_SKIP() << "the sorted transparent path needs buffer device addresses";

    const GUID glassA = FixedGuid(0x41);
    const GUID glassB = FixedGuid(0x42);
    ASSERT_NE(RegisterBlendMaterial(h, glassA, "GlassA", 1u), nullptr);
    ASSERT_NE(RegisterBlendMaterial(h, glassB, "GlassB", 2u), nullptr);
    for (int i = 0; i < 6; ++i)
    {
        h.materialGuid = (i % 2 == 0) ? glassA : glassB;
        h.SpawnRenderable(-0.5f + 0.2f * static_cast<float>(i), 0.0f, 0.5f);
    }

    Rendering::GPUScene* scene = h.rs.GetGPUScene();
    ASSERT_NE(scene, nullptr);
    uint64_t measuredAllocations = 0;
    uint32_t previousFrameIndex = ~0u;
    for (int frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame)
    {
        ASSERT_TRUE(m_Device->BeginFrame());
        scene->AdvanceFrameSlot();
        h.rs.BeginWorldDrawFrame();
        h.StepFrame();
        h.rs.BuildWorldBatchKeys(); // merges the frame's submissions per view
        scene->FlushGPUBuffers();
        // The build runs once per device frame index; an unchanged index would measure its
        // early return instead of a build.
        ASSERT_NE(m_Device->GetFrameIndex(), previousFrameIndex) << "frame " << frame;
        previousFrameIndex = m_Device->GetFrameIndex();

        uint64_t allocations = 0;
        {
            Memory::AllocationCountScope count(Memory::CountWindow::ThisThread);
            h.rs.BuildSortedTransparentForView(h.viewId);
            allocations = count.Count();
        }
        ASSERT_TRUE(h.rs.IsSortedTransparentActive(h.viewId)) << "frame " << frame << " staged nothing";
        if (frame >= kWarmupFrames)
            measuredAllocations += allocations;

        scene->EndFrame();
        m_Device->Present();
    }
    m_Device->WaitForIdle();
    EXPECT_EQ(measuredAllocations, 0u) << "over " << kMeasuredFrames << " builds";
}

// Also run with GE_FORCE_COMPAT=1: authored order must activate the direct
// instance-list drain even when buffer device addresses are unavailable.
TEST_F(SortedTransparentBuildAllocationTest, AuthoredMeshOrderingActivatesOnCompatibilityDevices)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    if (const char* forced = std::getenv("GE_FORCE_COMPAT"); forced && std::string_view(forced) == "1")
    {
        ASSERT_TRUE(h.rs.GetProfile().IsCompat());
        ASSERT_FALSE(m_Device->GetCapabilities().supportsBufferDeviceAddress);
    }
    const GUID glass = FixedGuid(0x51);
    ASSERT_NE(RegisterBlendMaterial(h, glass, "OrderedGlass", 1u), nullptr);
    h.materialGuid = glass;
    auto entity = h.SpawnRenderable(0.0f, 0.0f, 0.5f);
    entity.Set(Components::RenderSorting{10, 2, false});
    auto* scene = h.rs.GetGPUScene();
    ASSERT_NE(scene, nullptr);
    ASSERT_TRUE(m_Device->BeginFrame());
    scene->AdvanceFrameSlot();
    h.rs.BeginWorldDrawFrame();
    h.StepFrame();
    h.rs.BuildWorldBatchKeys();
    scene->FlushGPUBuffers();
    h.rs.BuildSortedTransparentForView(h.viewId);
    EXPECT_TRUE(h.rs.IsSortedTransparentActive(h.viewId));
    scene->EndFrame();
    m_Device->Present();
    m_Device->WaitForIdle();
}

} // namespace
} // namespace GameEngine::Testing::ExtractionFastPath
