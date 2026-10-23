#pragma once

namespace GameEngine::Editor
{
struct SettingsFieldDescriptor;

// The project-wide default raster-size dropdown for SVG texture assets without a
// per-asset override (power-of-two bounds, aspect ratio kept). Editor UI SVGs
// have no such setting: the UI rasterizes them at the size they are drawn at.
SettingsFieldDescriptor MakeSvgRasterSizeField();

} // namespace GameEngine::Editor
