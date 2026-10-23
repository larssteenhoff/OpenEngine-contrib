#include "Editor/Settings/SvgRasterSizeField.h"

#include "Assets/SvgRasterizer.h"
#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SvgRasterSettings.h"
#include "Logger/Logger.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
// Offered sizes are 2^kMinExponent .. 2^kMaxExponent pixels on the long edge.
constexpr int kMinExponent = 4;
constexpr int kMaxExponent = 14;

std::vector<SettingsFieldDescriptor::DropdownField::Option> RasterSizeOptions()
{
    std::vector<SettingsFieldDescriptor::DropdownField::Option> options;
    options.reserve(static_cast<size_t>(kMaxExponent - kMinExponent + 1));
    for (int exponent = kMinExponent; exponent <= kMaxExponent; ++exponent)
    {
        const std::string pixels = std::to_string(1 << exponent);
        options.push_back({pixels, pixels + "x" + pixels});
    }
    return options;
}

// The stored size rounded up to the nearest offered option, so a hand-edited
// value still selects an entry.
std::string StoredRasterSize()
{
    std::string loadError;
    const uint32_t pixels = SvgRasterSettings::Load(EngineCore::GetInstance().GetWorkspaceRoot(), &loadError);
    if (!loadError.empty())
        Logger::Log::Warning("SvgRasterSettings: failed to load setting: {}", loadError);
    int exponent = kMinExponent;
    while (exponent < kMaxExponent && (1u << exponent) < pixels)
        ++exponent;
    return std::to_string(1 << exponent);
}

void SaveRasterSize(const std::string& value)
{
    uint32 pixels = 0;
    if (!ParseSvgRasterSizeMeta(value, pixels))
        return;
    const std::filesystem::path& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    // The page also reports the stored value when it opens; that must not
    // rewrite the project file or reload every SVG asset.
    if (SvgRasterSettings::Load(workspaceRoot) == pixels)
        return;
    std::string err;
    if (!SvgRasterSettings::SaveAndApply(workspaceRoot, pixels, true, &err))
        Logger::Log::Warning("SvgRasterSettings: failed to save setting: {}", err);
}
} // namespace

SettingsFieldDescriptor MakeSvgRasterSizeField()
{
    SettingsFieldDescriptor field;
    field.Label = "Default Raster Size";
    field.Tooltip = "Default raster bounds for project SVG textures without a per-asset override. Aspect "
                    "ratio is preserved.";
    field.SearchKeywords = "svg texture import raster resolution default size";
    SettingsFieldDescriptor::DropdownField dropdown;
    dropdown.OptionsProvider = RasterSizeOptions;
    dropdown.DefaultValue = std::to_string(SvgRasterSettings::kDefaultSize);
    dropdown.Get = StoredRasterSize;
    dropdown.Set = SaveRasterSize;
    field.Control = std::move(dropdown);
    return field;
}

} // namespace GameEngine::Editor
