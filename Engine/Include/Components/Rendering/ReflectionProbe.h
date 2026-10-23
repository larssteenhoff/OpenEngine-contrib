#pragma once

#include "Rendering/RenderLayerMask.h"
#include "Types/Types.h"

namespace GameEngine {
namespace Components {

enum class ReflectionProbeUpdateMode : uint32 {
    Once = 0,
    Realtime = 1,
};

enum class ReflectionProbeInfluenceShape : uint32 { Box = 1, Ellipsoid = 2 };

struct ReflectionProbe {
    // Local influence volume. Like PostProcessVolume, the entity transform owns
    // the probe range: a unit box/sphere is scaled and oriented by WorldTransform.
    // @ge-tooltip Fade width near the influence edge. Visible when the camera is near the probe boundary.
    float32 BlendDistance = 2.0f;
    // @ge-tooltip Tie-breaker when more than one probe influences the camera.
    uint32 Priority = 0;

    ReflectionProbeInfluenceShape InfluenceShape = ReflectionProbeInfluenceShape::Box;
    // Projection box in the influence volume's local axes. Extent multipliers
    // and world-unit offsets default to the original transform-owned bounds.
    float32 ProjectionScaleX = 1.0f; // @ge-range 0.001
    float32 ProjectionScaleY = 1.0f; // @ge-range 0.001
    float32 ProjectionScaleZ = 1.0f; // @ge-range 0.001
    float32 ProjectionOffsetX = 0.0f;
    float32 ProjectionOffsetY = 0.0f;
    float32 ProjectionOffsetZ = 0.0f;
    // @ge-tooltip Capture/projection origin offset along the probe's local X axis.
    float32 OriginOffsetX = 0.0f;
    // @ge-tooltip Capture/projection origin offset along the probe's local Y axis.
    float32 OriginOffsetY = 0.0f;
    // @ge-tooltip Capture/projection origin offset along the probe's local Z axis.
    float32 OriginOffsetZ = 0.0f;

    // Dynamic multiplier applied through EnvData, so changing it does not force a
    // cubemap rebake.
    // @ge-tooltip Runtime IBL multiplier. Updates immediately without rebaking the cubemap.
    float32 Intensity = 1.0f;
    // @ge-tooltip Exposure applied when baking the probe capture.
    float32 ExposureEV = 0.0f;
    // @ge-tooltip Yaw rotation applied to sky/HDRI probe lighting during bake.
    float32 RotationDegrees = 0.0f;
    // @ge-tooltip Darkens lighting sampled from below the horizon. 0 keeps it bright; 1 makes it black.
    float32 IblLowerHemisphereDarkness = 0.3f;

    // Include the global sky/HDRI in captured geometry's environment lighting.
    // Local probes are excluded from every probe capture, even when enabled,
    // so captures cannot feed themselves or form cross-probe feedback loops.
    bool CaptureEnvironment = false;

    // Scene capture behavior. Probes use the active sky/HDRI as background
    // when present, then capture nearby scene content.
    // @ge-tooltip Cubemap face resolution for the scene capture. Higher values sharpen mirror-like reflections but make probe bakes more expensive.
    uint32 CaptureResolution = 256;
    // @ge-tooltip Scene-capture refresh mode. Once rebakes only when probe inputs change; Realtime rebakes on the interval.
    ReflectionProbeUpdateMode UpdateMode = ReflectionProbeUpdateMode::Realtime;
    // Shortest time between realtime captures while the probe's world
    // changes (SceneProbeWorldEpochs: lights, renderables, dynamic depth,
    // material edits; not shader-time animation, video textures or
    // particles); a world at rest is not recaptured. 0 captures every frame:
    // a full six-face bake per frame, no time slicing and no rebake throttle.
    // @ge-tooltip Seconds between realtime scene captures. 0 captures every frame.
    float32 RealtimeUpdateInterval = 0.5f; // @ge-range 0
    // @ge-tooltip Far clip distance for scene capture. 0 uses the probe volume.
    float32 MaxDistance = 0.0f;
    // @ge-tooltip The render layers the probe's capture draws
    Rendering::RenderLayerMask CapturedLayers = Rendering::RenderLayerMask::All();
};

} // namespace Components
} // namespace GameEngine
