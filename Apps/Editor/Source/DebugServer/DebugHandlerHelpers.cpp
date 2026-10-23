#include "DebugServer/DebugHandlerHelpers.h"

#include "Components/Name.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "EditorApplication.h"
#include "EditorPanelIds.h"
#include "Engine/Rendering/RenderServices.h"
#include "Platform/Window.h"
#include "UI/Layout/Docking.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

namespace GameEngine
{

namespace RG = Rendering::RenderGraph;

RG::RGFrame* MainRgFrame(EditorApplication& app)
{
    if (app.m_Windows.empty() || !app.m_Windows[0])
        return nullptr;
    return app.m_Windows[0]->RenderGraphStream.Frame.get();
}

const char* RgQueueName(RG::RGQueue q)
{
    switch (q)
    {
    case RG::RGQueue::Graphics: return "Graphics";
    case RG::RGQueue::Compute:  return "Compute";
    case RG::RGQueue::Transfer: return "Copy";
    }
    return "?";
}

size_t PendingMaterialTextureBinds()
{
    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        return rs->Textures().PendingMaterialTextureBindCount();
    return 0u;
}

std::string EntityNameFromWorld(ECS::World* world, ECS::EntityHandle entity)
{
    if (!world || !entity.IsValid())
        return {};

    auto* name = world->GetComponent<Components::Name>(entity);
    if (name)
        return std::string(name->View());
    return {};
}

ViewActivation ActivateDockedView(EditorApplication& app,
                                  DockingManager& docking,
                                  const std::string& panelId,
                                  bool wantsViewportUiFocus)
{
    ViewActivation result;
    if (app.m_Windows.empty() || !app.m_Windows[0] || !app.m_Windows[0]->ui)
        return result;

    app.OpenPanel(panelId);

    auto* mainWindow = app.m_Windows[0].get();
    UIManager* ui = mainWindow->ui.get();

    // Raise the OS window: a backgrounded editor renders black or throttled, and
    // both callers exist to decide what gets rendered and captured next.
    if (mainWindow->window)
        mainWindow->window->Focus();

    if (wantsViewportUiFocus)
    {
        if (UIElement* rootEl = ui->GetRootElement())
        {
            if (rootEl->FindById(HashStringId(EditorPanelIds::GameViewViewport)))
            {
                ui->SetFocusById(EditorPanelIds::GameViewViewport);
                result.viewportFocused = true;
            }
        }
    }
    else
    {
        ui->SetFocusById("");
    }

    result.ok = true;
    result.active = docking.IsPanelActiveTab(panelId);
    result.focusedElementId = ui->GetFocusedElementId();
    return result;
}

} // namespace GameEngine
