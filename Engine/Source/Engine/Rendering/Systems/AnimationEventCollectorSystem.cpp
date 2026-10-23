#include "ECSModules/Rendering/Systems/AnimationEventCollectorSystem.h"

#include "Animation/AnimationEventCollectorStore.h"
#include "Components/Animation/Animator.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

namespace GameEngine { namespace Engine::Renderer {

void AnimationEventCollectorSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto& store = Animation::AnimationEventCollectorStore::Instance();
    // Writes the Animator only when it gets its first collector, so a steady frame stamps nothing.
    world.Query<ECS::Write<Components::Animator>>().Each([&](ECS::Writable<Components::Animator> animator) {
        if (Animation::AnimationEventCollector* collector = store.Get(animator.Get().eventCollectorId))
            collector->Clear();
        else
            animator.Mutable().eventCollectorId = store.Create();
    });
}

} } // namespace GameEngine::Engine::Renderer
