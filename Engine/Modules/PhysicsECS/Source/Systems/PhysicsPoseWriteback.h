#pragma once

#include "PhysicsECS/Components/PhysicsWritebackRecord.h"

#include "Types/Types.h"

#include <optional>

namespace GameEngine::ECS
{
class World;
struct EntityHandle;
template <typename T>
class Writable;
} // namespace GameEngine::ECS

namespace GameEngine::Physics
{
struct Transform;
} // namespace GameEngine::Physics

namespace GameEngine::Components
{
struct Transform;
struct WorldTransform;
} // namespace GameEngine::Components

namespace GameEngine::PhysicsECS
{
/// Writes a backend pose to an entity's Transform and WorldTransform, keeping the Transform's scale
/// (scale is baked into the shapes). The one writeback rule for rigid bodies and characters.
///
/// The decision is made on the inputs: the entity is skipped, before any scale read or matrix compose,
/// when the record is valid, `current` equals the recorded pose, `interpolateFrom` (when given) equals
/// it too, and both Transform and WorldTransform still hold the recorded bytes. Then every alpha
/// composes the same pose, and an edited Transform or WorldTransform fails the byte compare and snaps
/// back to the body pose.
///
/// Otherwise the pose (blended from `interpolateFrom` by `alpha` when given) is composed, each
/// component whose bytes differ is written (WorldTransform with its Version bump and dirty-feed entry),
/// and the updated record is returned. A blend between two different poses moves with alpha, so it
/// leaves the record invalid and the next frame composes again.
///
/// Only a component that is written is write-granted, so a skipped entity grants none of its columns.
/// The caller owns the record's component and stores the returned record through its own grant.
///
/// @param interpolateFrom The backend pose before the last step to blend from, or null for no blend.
/// @param alpha Blend weight of `current` in [0, 1]; read only with `interpolateFrom`.
/// @return The record to store when the pose was composed, nullopt when the entity was skipped as
///         unchanged.
std::optional<Components::PhysicsWritebackRecord> WritePhysicsPose(ECS::World& world,
                                                                   ECS::EntityHandle entity,
                                                                   const Physics::Transform& current,
                                                                   const Physics::Transform* interpolateFrom,
                                                                   float32 alpha,
                                                                   ECS::Writable<Components::Transform> transform,
                                                                   ECS::Writable<Components::WorldTransform> worldTransform,
                                                                   const Components::PhysicsWritebackRecord& record);

} // namespace GameEngine::PhysicsECS
