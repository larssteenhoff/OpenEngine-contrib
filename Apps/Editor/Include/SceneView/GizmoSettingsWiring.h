#pragma once

namespace GameEngine
{

class SceneViewController;

namespace Editor
{
namespace SceneTools
{

// Connects the Gizmo and Grid & Snapping settings to the scene view that uses
// them, keeping feature-specific wiring out of EditorApplication.cpp.
namespace GizmoSettingsWiring
{
    // Registers the Gizmo and Grid & Snapping settings pages against the
    // TransformTool and grid of the given SceneViewController, and applies the
    // saved gizmo and grid preferences to them. sceneController must be
    // non-null and outlive the Settings panel's pages.
    void WireGizmoSettings(SceneViewController* sceneController);

} // namespace GizmoSettingsWiring

} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine
