#include "PhysicsECS/Systems/CharacterControllerWritebackSystem.h"

#include "PhysicsPoseWriteback.h"

#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Components/CharacterController.h"

#include "Components/Transform.h"
#include "ECS/Query.h"

namespace GameEngine::PhysicsECS
{
void CharacterControllerWritebackSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto* pw = PhysicsWorldService::TryGet();
    if (!pw)
        return;

    uint32 composed = 0;
    world.Query<ECS::Write<GameEngine::Components::Transform>,
                ECS::Write<GameEngine::Components::WorldTransform>,
                ECS::Write<GameEngine::Components::CharacterController>>()
        .Each([&](ECS::EntityHandle e,
                  ECS::Writable<GameEngine::Components::Transform> t,
                  ECS::Writable<GameEngine::Components::WorldTransform> wt,
                  ECS::Writable<GameEngine::Components::CharacterController> controller)
              {
                  const GameEngine::Components::CharacterController& cc = controller.Get();
                  if (!cc.initialized || !pw->IsCharacterValid(cc.character))
                      return;

                  const bool grounded = pw->IsCharacterGrounded(cc.character);
                  const Physics::Vector3 n = pw->GetCharacterGroundNormal(cc.character);
                  if (cc.grounded != grounded || cc.groundNormalX != n.x ||
                      cc.groundNormalY != n.y || cc.groundNormalZ != n.z)
                  {
                      GameEngine::Components::CharacterController& written = controller.Mutable();
                      written.grounded = grounded;
                      written.groundNormalX = n.x;
                      written.groundNormalY = n.y;
                      written.groundNormalZ = n.z;
                  }

                  if (const auto record = WritePhysicsPose(world, e, pw->GetCharacterTransform(cc.character),
                                                           nullptr, 1.0f, t, wt, cc.WritebackRecord))
                  {
                      controller.Mutable().WritebackRecord = *record;
                      ++composed;
                  }
              });
    PhysicsWorldService::SetLastCharactersComposed(composed);
}
} // namespace GameEngine::PhysicsECS
