// Draw order for forward contributors (terrain, ocean, grass, fog particles).
//
// The world pass drains its ForwardCommands stream in emission order. Producer-
// emitted commands (grass, fog — pushed during the ForwardCommands producer
// sweep) land ahead of node-emitted content (CBT terrain, ocean surface —
// pushed later from their render-graph nodes). Blended commands write no depth,
// so opaque content drained afterward rasterizes over their pixels wherever it
// covers them. Ordering the drain so every depth-writing command precedes every
// blended one — preserving relative order within each group — keeps opaque
// contributors from overwriting blended ones regardless of who emitted first.

#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/WorldDrawBuilder.h"
#include "Rendering/Materials/MaterialAlphaMode.h"

#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine::Engine::Renderer
{

inline bool IsBlendedContributor(const DrawCommand& cmd)
{
    return cmd.Material && cmd.Material->GetAlphaMode() == MaterialAlphaMode::Blend;
}

// Dispatch both sources together: finishing entities before starting contributors
// can still put a blended entity ahead of an opaque contributor. The callbacks
// are the world pass's actual draw recorders; guards and range consolidation stay
// with those recorders. Each phase preserves each source's existing order.
template <typename RecordEntity, typename RecordContributor>
void DrainWorldForwardOpaqueFirst(std::span<const WorldDrawBuilder::BatchKey> entities,
                                 std::span<const DrawCommand> contributors,
                                 const RecordEntity& recordEntity,
                                 const RecordContributor& recordContributor)
{
    for (bool blendPhase : {false, true})
    {
        for (const auto& key : entities)
            if (bool(key.material && key.material->GetAlphaMode() == MaterialAlphaMode::Blend) == blendPhase)
                recordEntity(key);
        for (const auto& command : contributors)
            if (IsBlendedContributor(command) == blendPhase)
                recordContributor(command);
    }
}

// Indices into `commands` in drain order: depth-writing (opaque / masked /
// materialless) commands first in emission order, then blended commands in
// emission order.
inline std::vector<uint32_t> ForwardDrainOrderOpaqueFirst(std::span<const DrawCommand> commands)
{
    std::vector<uint32_t> order;
    order.reserve(commands.size());
    for (uint32_t i = 0; i < commands.size(); ++i)
        if (!IsBlendedContributor(commands[i]))
            order.push_back(i);
    for (uint32_t i = 0; i < commands.size(); ++i)
        if (IsBlendedContributor(commands[i]))
            order.push_back(i);
    return order;
}

} // namespace GameEngine::Engine::Renderer
