#pragma once

#include "AssetCore/GUID.h"
#include "Components/AssetRef.h" // reflects the GUID field as FieldTypeId::AssetGuid
#include "Types/StringUtils.h"

#include <string_view>

namespace GameEngine::Components
{

// Marks an entity as the root of a scene [blueprint] instance, with enough metadata
// to re-serialize the instance back to a scene file.
//
// The path is a fixed-size buffer for ECS component constraints. A raw component write
// can fill every byte with no terminator, so read it through SourcePath().
// @ge-no-add  data helper, not user-addable in the editor
// [DoNotSerialize] — SceneIO reconstructs the [blueprint] section from this metadata; reflection would emit invalid property lines.
struct SceneBlueprintInstance
{
    // Blueprint asset path as authored in the scene's [resource] section (usually relative to asset root).
    char sourcePath[260]{};

    // The [resource] section's guid=, or null when the scene authored none.
    GUID sourceGuid{}; // @ge-readonly

    [[nodiscard]] std::string_view SourcePath() const { return FixedStringView(sourcePath); }
};

} // namespace GameEngine::Components
