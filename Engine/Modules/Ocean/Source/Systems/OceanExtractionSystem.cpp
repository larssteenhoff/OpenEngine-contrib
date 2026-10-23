#include "Ocean/Systems/OceanExtractionSystem.h"

#include "AssetCore/SharedFileRead.h"
#include "Ocean/OceanCollisionProvider.h"
#include "Ocean/OceanFFTCollisionAsset.h"
#include "Ocean/OceanInputDrawSource.h"
#include "Ocean/OceanPresetAsset.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanSeaLevel.h"
#include "Ocean/OceanSettingsAsset.h"
#include "Ocean/OceanTypes.h"

#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "AssetCore/GUID.h"
#include "Mathematics/HalfFloat.h"
#include "Types/Fnv1a.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "Spline/SplineTypes.h"
#include "SplineECS/SplineService.h"

#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "ECS/Query.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine::Ocean
{

namespace
{

// Depth beyond which the shallow tint, shallow wave attenuation and shoreline
// foam no longer change; depth bands ramp from it at their feathered edge.
float OceanDepthBandSaturation(const OceanParamsGPU& params)
{
    return std::max(params.SubSurfaceDepthMax, params.ShorelineFoamMaxDepth);
}

float SrgbChannelToLinear(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    if (value <= 0.04045f)
        return value / 12.92f;
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float32 ClampNonNegativeFinite(float32 value)
{
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

float ReadTextureChannel01(const TextureAsset& texture, size_t pixelIndex, uint32 channel,
                           uint32 bytesPerChannel, bool decodeSrgb)
{
    const uint32 channels = texture.GetChannels();
    if (channels == 0u || channel >= channels)
        return 1.0f;

    const uint8* pixels = texture.GetPixelData();
    if (!pixels)
        return 1.0f;

    const size_t channelIndex = pixelIndex * channels + channel;
    float value = 1.0f;
    if (bytesPerChannel == 1u)
    {
        value = static_cast<float>(pixels[channelIndex]) / 255.0f;
    }
    else if (bytesPerChannel == 2u)
    {
        const auto* halfs = reinterpret_cast<const uint16*>(pixels);
        value = Mathematics::HalfToFloat(halfs[channelIndex]);
    }
    else if (bytesPerChannel == 4u)
    {
        const auto* floats = reinterpret_cast<const float*>(pixels);
        value = floats[channelIndex];
    }

    if (decodeSrgb && channel < 3u)
        value = SrgbChannelToLinear(value);
    return value;
}

OceanCpuTextureRG MakeOceanCpuTextureRG(const TextureAsset& texture)
{
    OceanCpuTextureRG out{};
    const uint32 width = texture.GetWidth();
    const uint32 height = texture.GetHeight();
    const uint32 channels = texture.GetChannels();
    const uint8* pixels = texture.GetPixelData();
    if (!pixels || width == 0u || height == 0u || channels == 0u)
        return out;

    const uint64 texelCount = static_cast<uint64>(width) * height;
    const uint64 minBytes = texelCount * channels;
    if (texture.GetDataSize() < minBytes)
        return out;
    const uint32 bytesPerChannel =
        static_cast<uint32>(std::max<uint64>(1u, texture.GetDataSize() / minBytes));
    if (bytesPerChannel != 1u && bytesPerChannel != 2u && bytesPerChannel != 4u)
        return out;

    out.Width = width;
    out.Height = height;
    out.RG.resize(static_cast<size_t>(texelCount) * 2u);

    const bool decodeSrgb = texture.GetColorSpace() == TextureColorSpace::SRGB &&
                            bytesPerChannel == 1u;
    for (uint64 i = 0u; i < texelCount; ++i)
    {
        out.RG[static_cast<size_t>(i) * 2u + 0u] =
            ReadTextureChannel01(texture, static_cast<size_t>(i), 0u, bytesPerChannel,
                                 decodeSrgb);
        out.RG[static_cast<size_t>(i) * 2u + 1u] =
            ReadTextureChannel01(texture, static_cast<size_t>(i), 1u, bytesPerChannel,
                                 decodeSrgb);
    }
    return out;
}

OceanCpuTextureRG LoadOceanCpuTextureRG(const GUID& guid)
{
    if (guid.IsNull())
        return {};

    AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
    SharedPtr<Asset> asset = assetManager.GetAsset(guid);
    if ((!asset || !asset->IsLoaded()) && !guid.IsNull())
        asset = assetManager.LoadAssetAsync(guid).get();
    if (auto* texture = dynamic_cast<TextureAsset*>(asset.get()))
    {
        if (texture->IsLoaded())
            return MakeOceanCpuTextureRG(*texture);
    }

    AssetMetadata metadata{};
    if (assetManager.GetRegistry().TryGetAssetMetadata(guid, metadata) &&
        !metadata.Path.empty())
    {
        TextureAsset texture(guid, metadata.Path);
        if (texture.Load())
            return MakeOceanCpuTextureRG(texture);
    }

    return {};
}

float Hash01(uint32 i)
{
    float s = std::sin(static_cast<float>(i) * 12.9898f) * 43758.5453f;
    return s - std::floor(s);
}

struct DepthCacheStreamCamera
{
    bool Valid = false;
    float X = 0.0f;
    float Z = 0.0f;
};

DepthCacheStreamCamera ResolveDepthCacheStreamCamera(Engine::Renderer::RenderServices* rs)
{
    DepthCacheStreamCamera out{};
    if (!rs)
        return out;

    auto useCamera = [&](::GameEngine::Rendering::CameraId cameraId) -> bool {
        if (cameraId == 0)
            return false;
        const auto* cam = rs->Views().FindCameraData(cameraId);
        if (!cam)
            return false;
        out.Valid = true;
        out.X = cam->cameraPos[0];
        out.Z = cam->cameraPos[2];
        return true;
    };

    if (useCamera(rs->Views().GetViewpointCamera()))
        return out;

    for (const auto& view : rs->Views().GetViews())
    {
        if (!view.activeRenderPipeline || !view.Participates())
            continue;
        if (useCamera(view.cameraId))
            return out;
    }
    return out;
}

float DistanceSqToRectXZ(float x, float z, float originX, float originZ,
                         float sizeX, float sizeZ)
{
    const float halfX = std::max(sizeX * 0.5f, 0.0f);
    const float halfZ = std::max(sizeZ * 0.5f, 0.0f);
    const float centerX = originX + halfX;
    const float centerZ = originZ + halfZ;
    const float dx = std::max(std::abs(x - centerX) - halfX, 0.0f);
    const float dz = std::max(std::abs(z - centerZ) - halfZ, 0.0f);
    return dx * dx + dz * dz;
}

float DistanceSqToCenteredRectXZ(float x, float z, float centerX, float centerZ,
                                 float halfX, float halfZ)
{
    const float dx = std::max(std::abs(x - centerX) - std::max(halfX, 0.0f), 0.0f);
    const float dz = std::max(std::abs(z - centerZ) - std::max(halfZ, 0.0f), 0.0f);
    return dx * dx + dz * dz;
}

static_assert(Components::OceanWaterBodyLocalWaveCapacity == kMaxOceanWaveMaskLocalWaves,
              "Water-body local wave authoring and wave-mask GPU packet sizes must match");

void WriteLocalWaveSlot(float32 outWave[4], float32 amplitude, float32 wavelength,
                        float32 directionDegrees)
{
    const float32 localWaveRad = directionDegrees * 0.01745329252f;
    outWave[0] = std::max(amplitude, 0.0f);
    outWave[1] = std::max(wavelength, 0.1f);
    outWave[2] = std::cos(localWaveRad);
    outWave[3] = std::sin(localWaveRad);
}

uint32 BuildWaterBodyLocalWavePacket(
    float32 amplitude, float32 wavelength, float32 directionDegrees, uint32 requestedCount,
    const float32 extraAmplitude[Components::OceanWaterBodyExtraLocalWaveCapacity],
    const float32 extraWavelength[Components::OceanWaterBodyExtraLocalWaveCapacity],
    const float32 extraDirectionDegrees[Components::OceanWaterBodyExtraLocalWaveCapacity],
    float32 outWaves[kMaxOceanWaveMaskLocalWaves][4])
{
    for (uint32 i = 0u; i < kMaxOceanWaveMaskLocalWaves; ++i)
        WriteLocalWaveSlot(outWaves[i], 0.0f, 12.0f, 0.0f);

    const uint32 count = std::min(requestedCount, kMaxOceanWaveMaskLocalWaves);
    if (count == 0u)
        return 0u;

    WriteLocalWaveSlot(outWaves[0], amplitude, wavelength, directionDegrees);
    for (uint32 i = 1u; i < count; ++i)
    {
        const uint32 extra = i - 1u;
        WriteLocalWaveSlot(outWaves[i], extraAmplitude[extra], extraWavelength[extra],
                           extraDirectionDegrees[extra]);
    }
    return count;
}

// Build the analytic Gerstner wave set from the resolved wind spectrum. This is
// intentionally kept alongside the FFT path: it is the deterministic CPU/query
// fallback when FFT data is not ready and the runtime surface fallback when the
// device cannot initialize the FFT chain. Wavelengths are distributed
// geometrically and total steepness is normalized to avoid self-intersection.
void BuildGerstnerWaves(const Components::OceanWaveSpectrum& spec, OceanParamsGPU& out)
{
    constexpr uint32 numWaves = 12; // <= kMaxGerstnerWaves
    const float maxWL = (spec.MaxWavelength > 1.0f) ? spec.MaxWavelength : 250.0f;
    const float minWL = maxWL / 64.0f;
    const float windRad = spec.WindDirectionDegrees * 0.01745329252f;
    const float windSpeed = ClampNonNegativeFinite(spec.WindSpeed);
    const float windScale = windSpeed * windSpeed * 0.022f * spec.AmplitudeScale;
    const float spread = std::max(spec.DirectionalSpread, 0.0f);

    for (uint32 i = 0; i < numWaves; ++i)
    {
        const float t = (numWaves > 1)
                            ? static_cast<float>(i) / static_cast<float>(numWaves - 1)
                            : 0.0f;
        const float wl = minWL * std::pow(maxWL / minWL, t);
        const float amp = windScale * std::pow(wl / maxWL, 0.9f);
        const float angOffset = (Hash01(i * 7u + 3u) - 0.5f) * spread * 2.0f;
        const float a = windRad + angOffset;
        const float k = 6.2831853f / wl;
        float steep = 0.75f /
                      (k * std::max(amp, 1e-3f) * static_cast<float>(numWaves));
        steep = std::min(steep, 1.0f);

        GerstnerWave& w = out.Waves[i];
        w.DirectionX = std::cos(a);
        w.DirectionZ = std::sin(a);
        w.Amplitude = std::max(amp, 0.0f);
        w.Wavelength = std::max(wl, 0.1f);
        w.Steepness = steep;
        w.Speed = 1.0f;
    }
    out.GerstnerWaveCount = numWaves;
}

uint32 BuildWaterBodySpectrumWavePacket(const Components::OceanWaveSpectrum& spec,
                                        float32 outWaves[kMaxOceanWaveMaskLocalWaves][4])
{
    for (uint32 i = 0u; i < kMaxOceanWaveMaskLocalWaves; ++i)
        WriteLocalWaveSlot(outWaves[i], 0.0f, 12.0f, 0.0f);

    struct GeneratedWave
    {
        float32 Amplitude = 0.0f;
        float32 Wavelength = 12.0f;
        float32 DirectionX = 1.0f;
        float32 DirectionZ = 0.0f;
    };

    constexpr uint32 generatedCount = 12u;
    GeneratedWave generated[generatedCount]{};
    const float32 maxWL = (spec.MaxWavelength > 1.0f) ? spec.MaxWavelength : 250.0f;
    const float32 minWL = maxWL / 64.0f;
    const float32 windRad = spec.WindDirectionDegrees * 0.01745329252f;
    const float32 windSpeed = ClampNonNegativeFinite(spec.WindSpeed);
    const float32 windScale = windSpeed * windSpeed * 0.022f * spec.AmplitudeScale;
    const float32 spread = (spec.DirectionalSpread < 0.0f) ? 0.0f : spec.DirectionalSpread;

    for (uint32 i = 0u; i < generatedCount; ++i)
    {
        const float32 t = static_cast<float32>(i) / static_cast<float32>(generatedCount - 1u);
        const float32 wl = minWL * std::pow(maxWL / minWL, t);
        const float32 amp = windScale * std::pow(wl / maxWL, 0.9f);
        const float32 angOffset = (Hash01(i * 7u + 3u) - 0.5f) * spread * 2.0f;
        const float32 a = windRad + angOffset;
        generated[i].Amplitude = std::max(amp, 0.0f);
        generated[i].Wavelength = std::max(wl, 0.1f);
        generated[i].DirectionX = std::cos(a);
        generated[i].DirectionZ = std::sin(a);
    }

    // The local packet is intentionally small, so take the dominant long-wave end
    // of the generated spectrum. These are the bands a bounded water body can most
    // visibly differentiate without a full independent FFT cascade.
    const uint32 count = std::min(generatedCount, kMaxOceanWaveMaskLocalWaves);
    const uint32 first = generatedCount - count;
    for (uint32 i = 0u; i < count; ++i)
    {
        const GeneratedWave& wave = generated[first + i];
        outWaves[i][0] = wave.Amplitude;
        outWaves[i][1] = wave.Wavelength;
        outWaves[i][2] = wave.DirectionX;
        outWaves[i][3] = wave.DirectionZ;
    }
    return count;
}

float32 JsonFloat(const nlohmann::json& doc, const char* key, float32 fallback)
{
    auto it = doc.find(key);
    return (it != doc.end() && it->is_number()) ? it->get<float32>() : fallback;
}

void JsonFloatArray(const nlohmann::json& doc, const char* key,
                    float32 (&dst)[kOceanSpectrumOctaves])
{
    auto it = doc.find(key);
    if (it == doc.end() || !it->is_array())
        return;
    const size_t count = std::min(it->size(), static_cast<size_t>(kOceanSpectrumOctaves));
    for (size_t i = 0; i < count; ++i)
        if ((*it)[i].is_number())
            dst[i] = (*it)[i].get<float32>();
}

void JsonBoolArray(const nlohmann::json& doc, const char* key,
                   bool (&dst)[kOceanSpectrumOctaves])
{
    auto it = doc.find(key);
    if (it == doc.end() || !it->is_array())
        return;
    const size_t count = std::min(it->size(), static_cast<size_t>(kOceanSpectrumOctaves));
    for (size_t i = 0; i < count; ++i)
        if ((*it)[i].is_boolean())
            dst[i] = (*it)[i].get<bool>();
}

bool TryParseOceanWaveSpectrumJson(const std::string& body,
                                   Components::OceanWaveSpectrum& out)
{
    if (body.empty())
        return false;

    nlohmann::json doc;
    try
    {
        doc = nlohmann::json::parse(body);
    }
    catch (...)
    {
        return false;
    }
    if (!doc.is_object())
        return false;

    out.WindSpeed = JsonFloat(doc, "WindSpeed", out.WindSpeed);
    out.WindSpeed = ClampNonNegativeFinite(out.WindSpeed);
    out.WindDirectionDegrees = JsonFloat(doc, "WindDirectionDegrees", out.WindDirectionDegrees);
    out.Turbulence = JsonFloat(doc, "Turbulence", out.Turbulence);
    out.Multiplier = JsonFloat(doc, "Multiplier", out.Multiplier);
    out.Chop = JsonFloat(doc, "Chop", out.Chop);
    out.GravityScale = JsonFloat(doc, "GravityScale", out.GravityScale);
    out.LoopPeriod = JsonFloat(doc, "LoopPeriod", out.LoopPeriod);
    out.DirectionalSpread = JsonFloat(doc, "DirectionalSpread", out.DirectionalSpread);
    out.AmplitudeScale = JsonFloat(doc, "AmplitudeScale", out.AmplitudeScale);
    out.MaxWavelength = JsonFloat(doc, "MaxWavelength", out.MaxWavelength);
    out.Weight = JsonFloat(doc, "Weight", out.Weight);
    out.MaxHorizontalDisplacement =
        JsonFloat(doc, "MaxHorizontalDisplacement", out.MaxHorizontalDisplacement);
    out.MaxVerticalDisplacement =
        JsonFloat(doc, "MaxVerticalDisplacement", out.MaxVerticalDisplacement);
    out.RespectShallowWaterAttenuation =
        JsonFloat(doc, "RespectShallowWaterAttenuation", out.RespectShallowWaterAttenuation);
    JsonFloatArray(doc, "SpectrumPower", out.SpectrumPower);
    JsonFloatArray(doc, "ChopScales", out.ChopScales);
    JsonFloatArray(doc, "GravityScales", out.GravityScales);
    JsonBoolArray(doc, "OctaveDisabled", out.OctaveDisabled);
    return true;
}

bool TryLoadOceanWaveSpectrumAsset(const Components::OceanWaveSpectrumRef& ref,
                                   Components::OceanWaveSpectrum& out)
{
    const GUID guid = ref.ToGuid();
    if (guid.IsNull())
        return false;

    AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
    SharedPtr<Asset> asset = assetManager.GetAsset(guid);
    if ((!asset || !asset->IsLoaded()) && !guid.IsNull())
        asset = assetManager.LoadAssetAsync(guid).get();
    if (!asset || !asset->IsLoaded())
        return false;
    if (asset->GetType() != AssetType::OceanWaveSpectrum &&
        asset->GetType() != AssetType::Unknown)
    {
        return false;
    }

    std::string body;
    if (auto* binary = dynamic_cast<BinaryAsset*>(asset.get()))
    {
        if (!binary->HasData())
            return false;
        body.assign(reinterpret_cast<const char*>(binary->GetData()),
                    static_cast<size_t>(binary->GetDataSize()));
    }
    else
    {
        String text;
        if (!ReadFileTextShared(asset->GetPath(), text))
            return false;
        body = std::move(text);
    }

    return TryParseOceanWaveSpectrumJson(body, out);
}

bool OceanWaveSpectrumEquivalent(const Components::OceanWaveSpectrum& a,
                                 const Components::OceanWaveSpectrum& b)
{
    return a.WindSpeed == b.WindSpeed &&
           a.WindDirectionDegrees == b.WindDirectionDegrees &&
           a.Turbulence == b.Turbulence &&
           a.Multiplier == b.Multiplier &&
           a.Chop == b.Chop &&
           a.GravityScale == b.GravityScale &&
           a.LoopPeriod == b.LoopPeriod &&
           a.DirectionalSpread == b.DirectionalSpread &&
           a.AmplitudeScale == b.AmplitudeScale &&
           a.MaxWavelength == b.MaxWavelength &&
           a.Weight == b.Weight &&
           a.MaxHorizontalDisplacement == b.MaxHorizontalDisplacement &&
           a.MaxVerticalDisplacement == b.MaxVerticalDisplacement &&
           a.RespectShallowWaterAttenuation == b.RespectShallowWaterAttenuation &&
           std::memcmp(a.SpectrumPower, b.SpectrumPower, sizeof(a.SpectrumPower)) == 0 &&
           std::memcmp(a.ChopScales, b.ChopScales, sizeof(a.ChopScales)) == 0 &&
           std::memcmp(a.GravityScales, b.GravityScales, sizeof(a.GravityScales)) == 0 &&
           std::memcmp(a.OctaveDisabled, b.OctaveDisabled, sizeof(a.OctaveDisabled)) == 0;
}

std::filesystem::path ResolveOceanDepthCacheSourcePath(
    const Components::OceanDepthCacheSource& cache)
{
    const GUID assetGuid = cache.CacheAsset.ToGuid();
    if (!assetGuid.IsNull())
    {
        AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
        AssetMetadata metadata{};
        if (assetManager.GetRegistry().TryGetAssetMetadata(assetGuid, metadata) &&
            (metadata.Type == AssetType::OceanDepthCache || metadata.Type == AssetType::Unknown))
        {
            return metadata.Path;
        }

        SharedPtr<Asset> asset = assetManager.GetAsset(assetGuid);
        if ((!asset || !asset->IsLoaded()) && !assetGuid.IsNull())
            asset = assetManager.LoadAssetAsync(assetGuid).get();
        if (asset && (asset->GetType() == AssetType::OceanDepthCache ||
                      asset->GetType() == AssetType::Unknown))
        {
            return asset->GetPath();
        }
    }

    return std::filesystem::path(cache.GetPath());
}

bool TryApplyLocalSpectrumWaveOverride(
    bool useLocalSpectrum, const Components::OceanWaveSpectrum* localSpectrum,
    float32& weight, float32& chop, uint32& localWaveCount,
    float32 outWaves[kMaxOceanWaveMaskLocalWaves][4])
{
    if (!useLocalSpectrum || !localSpectrum)
        return false;

    weight = std::max(localSpectrum->Weight, 0.0f);
    chop = std::max(localSpectrum->Chop, 0.0f);
    localWaveCount = BuildWaterBodySpectrumWavePacket(*localSpectrum, outWaves);
    return true;
}

// Fill the per-octave power LUT (the reference parity). Base values are the
// reference's default OceanWaveSpectrum._powerLog with the v0->v1 upgrade applied
// (power /= 25), converted to linear power and scaled by multiplier^2. Each octave
// is then multiplied by the authored per-octave SpectrumPower control (1 = the
// default curve). 14 octaves used, the rest zero-padded.
void FillSpectrumLUT(float multiplier, const float32 perOctavePower[kOceanSpectrumOctaves],
                     float32 out[16])
{
    static constexpr float kPowerLog[kOceanSpectrumOctaves] = {
        -5.71f, -5.03f, -4.54f, -3.88f, -3.28f, -2.32f, -1.78f,
        -1.21f, -0.54f, 0.28f, 0.54f, 1.03f, 1.44f, -8.0f};
    constexpr float kUpgradeOffset = 1.39794f; // log10(25): the reference v0->v1 (power /= 25)
    const float mult2 = multiplier * multiplier;
    for (uint32 i = 0; i < 16; ++i)
    {
        if (i >= kOceanSpectrumOctaves)
        {
            out[i] = 0.0f;
            continue;
        }
        const float scale = perOctavePower ? perOctavePower[i] : 1.0f;
        out[i] = std::pow(10.0f, kPowerLog[i] - kUpgradeOffset) * mult2 * scale;
    }
}

// Copy a per-octave control array (14 used) into the std140 vec4[4] = 16-float
// slot, identity-padding the trailing entries so unused lanes stay neutral.
void FillOctaveArray(const float32 src[kOceanSpectrumOctaves], float32 out[16], float pad)
{
    for (uint32 i = 0; i < 16; ++i)
        out[i] = (i < kOceanSpectrumOctaves) ? src[i] : pad;
}

// A world's content-digest term while it renders an enabled ocean surface.
// Presence is what a reflection probe capture photographs; wave animation is
// not a content change.
constexpr uint64 kOceanSurfaceContentDigest = Hashing::Fnv1a64("OceanSurface");

struct WaterBodyEmit
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float ExtentX = 0.0f;
    float ExtentZ = 0.0f;
    float UnderwaterDepth = 0.0f;
    float FlowX = 0.0f;
    float FlowZ = 0.0f;
    float ClipFeather = 0.0f;
    float WaveWeight = 1.0f;
    float WaveChop = 1.0f;
    float WaveFeather = 0.0f;
    float LocalFFTBlend = 0.0f;
    uint32 LocalFFTStream = 0u;
    uint32 LocalWaveCount = 0u;
    float LocalWaves[kMaxOceanWaveMaskLocalWaves][4] = {};
    bool LocalSpectrumApplied = false;
    Components::OceanWaveSpectrum LocalSpectrum{};
    bool ConfineSurface = false;
    bool UnderwaterVolume = false;
    bool Flow = false;
    bool WaveOverride = false;
};

struct PolygonBodyEmit
{
    OceanRenderFeature::WaterBodyPolygon Query{};
    OceanClipPolygonGPU Clip{};
    OceanFlowPolygonGPU FlowPolygon{};
    OceanWaveMaskPolygonGPU WavePolygon{};
    float CenterX = 0.0f;
    float CenterY = 0.0f;
    float CenterZ = 0.0f;
    float HalfX = 0.0f;
    float HalfZ = 0.0f;
    float UnderwaterDepth = 0.0f;
    float FlowX = 0.0f;
    float FlowZ = 0.0f;
    bool LocalSpectrumApplied = false;
    Components::OceanWaveSpectrum LocalSpectrum{};
    bool ConfineSurface = false;
    bool UnderwaterVolume = false;
    bool Flow = false;
    bool WaveOverride = false;
};

struct DepthCacheCandidate
{
    OceanSavedDepthCacheSource Source{};
    float DistanceSq = 0.0f;
    uint32 Priority = 0u;
    uint32 Order = 0u;
};

struct LocalFftCandidate
{
    Components::OceanWaveSpectrum Spectrum{};
    float DistanceSq = 0.0f;
    uint32 Order = 0u;
    uint32 Stream = kMaxOceanLocalFFTStreams;
};

struct SplineSample
{
    float X = 0.0f, Y = 0.0f, Z = 0.0f; // world position of the sample
    float TX = 0.0f, TZ = 0.0f; // world tangent XZ (normalized) for the flow direction
};
struct SplineEmit
{
    std::vector<SplineSample> Samples;
    float Width = 20.0f;
    float UnderwaterDepth = 20.0f;
    float FlowSpeed = 3.0f;
    float AlbedoCoverage = 0.8f;
    float ClipFeather = 0.0f;
    float DepthMeters = 1.5f;
    float DepthFeather = 1.0f;
    float Color[3] = {0.0f, 0.0f, 0.0f};
    bool ConfineSurface = false;
    bool UnderwaterVolume = false;
    bool Flow = false;
    bool Clip = false;
    bool Albedo = false;
    bool Depth = false;
    bool ReverseFlow = false;
};

// Applies the renderer's preset to a component copy when its entity binds that same preset.
template <typename TComponent>
void ApplyBoundPreset(const OceanPresetAsset* preset, const GUID& presetGuid,
                      const Components::OceanPresetBinding* presetBinding, const char* section,
                      TComponent& component)
{
    if (preset && presetBinding &&
        presetBinding->Preset.ToGuid() == presetGuid)
    {
        preset->Apply(section, ECS::GetComponentTypeId<TComponent>(), &component,
                      sizeof(component), presetBinding->Overrides);
    }
}

// Volumes, exclusion volumes and portal occluders share one box footprint.
template <typename TVolume>
void AppendUnderwaterBox(const TVolume& uv, const Components::WorldTransform& wt,
                         std::vector<OceanRenderFeature::UnderwaterVolumeBox>& boxes)
{
    if (uv.ExtentX <= 0.0f || uv.ExtentY <= 0.0f || uv.ExtentZ <= 0.0f)
        return;
    const float32* m = wt.matrix;
    OceanRenderFeature::UnderwaterVolumeBox b{};
    b.CenterX = m[12];
    b.CenterY = m[13];
    b.CenterZ = m[14];
    b.HalfX = uv.ExtentX;
    b.HalfY = uv.ExtentY;
    b.HalfZ = uv.ExtentZ;
    boxes.push_back(b);
}

} // anonymous namespace

void PackShallowClarityWindow(const Components::OceanSurface& surface, OceanParamsGPU& params)
{
    // The inspector's lower bound (OceanFieldRanges.cpp): the shortest window the
    // shader's smoothstep evaluates.
    constexpr float32 kMinShallowClarityDistance = 0.01f;
    // A non-finite value (a hand-edited scene or preset) falls back to the field's
    // default, the former shader constant: NaN passes std::max and std::clamp.
    constexpr float32 kDefaultShallowClarityDistance = 8.0f;
    constexpr float32 kDefaultShallowClarityFloor = 0.22f;
    const float32 distance = std::isfinite(surface.ShallowClarityDistance)
                                 ? surface.ShallowClarityDistance
                                 : kDefaultShallowClarityDistance;
    const float32 fogAtSurface = std::isfinite(surface.ShallowClarityFloor)
                                     ? surface.ShallowClarityFloor
                                     : kDefaultShallowClarityFloor;
    params.ShallowClarityDistance = std::max(distance, kMinShallowClarityDistance);
    params.ShallowClarityFloor = std::clamp(fogAtSurface, 0.0f, 1.0f);
}

// Everything one extraction tick resolves and hands from phase to phase.
struct OceanExtractionFrame
{
    OceanParamsGPU Params{};
    OceanInputRegistryFrame Inputs;

    // Renderer-level config; HaveRenderer is false when no enabled OceanRenderer exists.
    Components::OceanRenderer Renderer{};
    ECS::EntityHandle RendererPresetEntity{};
    bool HaveRenderer = false;
    bool HaveRendererPresetBinding = false;
    const OceanSettingsAsset* CollisionSettings = nullptr;
    const OceanSettingsAsset* DynamicSettings = nullptr;
    const OceanSettingsAsset* FoamSettings = nullptr;
    OceanQuality Tier = OceanQuality::High;
    DepthCacheStreamCamera StreamCamera{};
    uint32 MaxActiveDepthCaches = 0u;
    float32 OceanTime = 0.0f;
    float32 OceanDeltaTime = 0.0f;
    OceanFeatureCollisionProvider* CollisionProvider = nullptr;

    // The first enabled OceanSurface and the feature toggles it and the inputs enable.
    bool HasSurface = false;
    bool FlowEnabled = false;
    bool DynWavesEnabled = false;
    bool ClipEnabled = false;
    bool AlbedoEnabled = false;
    GUID FoamTextureGuid{};
    GUID NormalTextureGuid{};
    GUID CausticsTextureGuid{};
    Components::OceanSurface MaterialBaseSurface{};
    OceanUnderwaterSettings UnderwaterSettings{};

    std::vector<Components::OceanWaveSpectrum> LocalFftSpecs;
    std::vector<WaterBodyEmit> WaterBodies;
    std::vector<PolygonBodyEmit> PolygonWaterBodies;
    std::vector<OceanRenderFeature::WaterBodyBox> WaterBodyBounds;
    std::vector<OceanRenderFeature::WaterBodyPolygon> WaterBodyPolygons;
    std::vector<OceanRenderFeature::WaterBodyStamp> WaterBodyStamps;
    bool WaterBodiesConstrainSurface = false;

    // Set by the global spectrum; the foam sim and the local FFT streams read them.
    float32 WindSpeed = 0.0f;
    float32 WindRadians = 0.0f;
    bool FftActive = false;

    uint32 SplineInputCount = 0u;
    uint32 UnderwaterVolumeCount = 0u;
    uint32 UnderwaterVolumePolygonCount = 0u;
    uint32 UnderwaterExclusionVolumeCount = 0u;
};

namespace
{

void ResolveQualityAndStreaming(Engine::Renderer::RenderServices* renderServices,
                                OceanExtractionFrame& frame)
{
    frame.Tier = frame.HaveRenderer
                     ? static_cast<OceanQuality>(frame.Renderer.QualityOverride > 2u
                                                     ? 2u
                                                     : frame.Renderer.QualityOverride)
                     : OceanQuality::High;
    frame.StreamCamera = ResolveDepthCacheStreamCamera(renderServices);
    frame.MaxActiveDepthCaches = frame.HaveRenderer ? frame.Renderer.MaxActiveDepthCaches : 0u;
    frame.Inputs.Stats.SavedDepthCacheBudget = frame.MaxActiveDepthCaches;
}

void CopyColor(float32 (&dst)[4], const ColorLinear& c)
{
    dst[0] = c.r;
    dst[1] = c.g;
    dst[2] = c.b;
    dst[3] = c.a;
}

void ApplySurfaceShading(const Components::OceanSurface& ocean, OceanRenderFeature& feature,
                         OceanParamsGPU& params)
{
    // Keep the serialized mode meaningful for old scenes. New oceans default
    // to FFT; Gerstner remains a supported authored/fallback surface.
    params.WaveMode = static_cast<uint32>(ocean.WaveMode);
    params.ChoppyScale = ocean.ChoppyScale;
    params.FresnelPower = ocean.FresnelPower;
    params.DeepColor[0] = ocean.DeepColor.r;
    params.DeepColor[1] = ocean.DeepColor.g;
    params.DeepColor[2] = ocean.DeepColor.b;
    params.DeepColor[3] = ocean.DeepColor.a;
    params.FoamColor[0] = ocean.FoamColor.r;
    params.FoamColor[1] = ocean.FoamColor.g;
    params.FoamColor[2] = ocean.FoamColor.b;
    params.FoamColor[3] = ocean.FoamColor.a;
    params.ReflectionStrength = ocean.ReflectionStrength;
    params.SubsurfaceStrength = ocean.SubsurfaceStrength;
    params.FoamAmount = ocean.FoamAmount;
    params.FoamFadeRate = ocean.FoamFadeRate;
    params.WaveFoamStrength = ocean.WaveFoamStrength;
    params.WaveFoamCoverage = ocean.WaveFoamCoverage;
    params.FoamScale = ocean.FoamScale;
    params.FoamFeather = ocean.FoamFeather;
    params.FoamDebugMode = ocean.FoamDebugMode;
    params.IntersectionFoamDepth = ocean.IntersectionFoamDepth;
    params.IntersectionFoamStrength = ocean.IntersectionFoamStrength;
    params.ShorelineFoamMaxDepth = ocean.ShorelineFoamMaxDepth;
    params.ShorelineFoamStrength = ocean.ShorelineFoamStrength;

    // Material / shading parity block.
    CopyColor(params.Diffuse, ocean.Diffuse);
    CopyColor(params.DiffuseGrazing, ocean.DiffuseGrazing);
    CopyColor(params.DiffuseShadow, ocean.DiffuseShadow);
    CopyColor(params.SubSurfaceShallowCol, ocean.SubSurfaceShallowCol);
    CopyColor(params.SubSurfaceColour, ocean.SubSurfaceColour);
    CopyColor(params.SkyBase, ocean.SkyBase);
    CopyColor(params.SkyTowardsSun, ocean.SkyTowardsSun);
    CopyColor(params.SkyAwayFromSun, ocean.SkyAwayFromSun);
    CopyColor(params.DirectionalLightColor, ocean.DirectionalLightColor);
    params.NormalsStrength = ocean.NormalsStrength;
    params.NormalsScale = std::max(ocean.NormalsScale, 0.001f);
    params.FoamNormalStrength = std::clamp(ocean.FoamNormalStrength, 0.0f, 2.0f);
    params.FoamBubbleCoverage = std::clamp(ocean.FoamBubbleCoverage, 0.0f, 1.0f);
    params.FoamBubbleParallax = std::clamp(ocean.FoamBubbleParallax, 0.0f, 0.5f);
    params.FoamRoughness = std::clamp(ocean.FoamRoughness, 0.04f, 1.0f);
    params.SubSurfaceDepthMax = ocean.SubSurfaceDepthMax;
    params.SubSurfaceDepthPower = ocean.SubSurfaceDepthPower;
    params.SubSurfaceBase = ocean.SubSurfaceBase;
    params.SubSurfaceSun = ocean.SubSurfaceSun;
    params.SubSurfaceSunFallOff = ocean.SubSurfaceSunFallOff;
    params.Specular = ocean.Specular;
    params.Roughness = ocean.Roughness;
    params.IorAir = ocean.IorAir;
    params.IorWater = ocean.IorWater;
    params.PlanarReflections = ocean.PlanarReflections ? 1u : 0u;
    params.PlanarReflectionStrength = std::clamp(ocean.PlanarReflectionStrength, 0.0f, 3.0f);
    feature.SetPlanarReflectionScale(ocean.PlanarReflectionScale);
    params.SkyDirectionality = ocean.SkyDirectionality;
    params.DirectionalLightBoost = ocean.DirectionalLightBoost;
    params.DirectionalLightFallOff = ocean.DirectionalLightFallOff;

    // Refraction / depth-fog transparency. The density swatch carries the
    // three per-channel extinctions; alpha is unused.
    params.DepthFogDensity[0] = ocean.DepthFogDensity.r;
    params.DepthFogDensity[1] = ocean.DepthFogDensity.g;
    params.DepthFogDensity[2] = ocean.DepthFogDensity.b;
    params.DepthFogStartDistance = std::max(ocean.DepthFogStartDistance, 0.0f);
    params.DepthFogEndDistance = std::max(ocean.DepthFogEndDistance, 0.0f);
    params.DepthFogFalloffPower = std::max(ocean.DepthFogFalloffPower, 0.01f);
    params.DepthFogFalloffMode =
        static_cast<float32>(static_cast<uint32>(ocean.DepthFogFalloff));
    params.RefractionStrength = ocean.RefractionStrength;
    params.ShallowRefractionReflectionSuppression =
        std::clamp(ocean.ShallowRefractionReflectionSuppression, 0.0f, 1.0f);
    PackShallowClarityWindow(ocean, params);

    // Underwater caustics. CausticsAvailable is stamped by the contributor
    // at emit (it knows texture readiness + whether refraction is bound),
    // not here, so leave it at its default 0.
    params.CausticsScale = ocean.CausticsScale;
    params.CausticsAverage = ocean.CausticsAverage;
    params.CausticsStrength = ocean.CausticsStrength;
    params.CausticsFocalDepth = ocean.CausticsFocalDepth;
    params.CausticsDepthOfField = ocean.CausticsDepthOfField;
    params.CausticsDistortionStrength = ocean.CausticsDistortionStrength;
    params.CausticsDistortionScale = ocean.CausticsDistortionScale;

    // Underwater. Stamp the authored toggle into Underwater (1 = enabled);
    // the render node ANDs the per-frame submersion test onto it via
    // SetUnderwaterActive before the surface draw / overlay read.
    params.Underwater = ocean.Underwater ? 1u : 0u;
    params.MeniscusWidth = ocean.MeniscusWidth;
}

void ApplyUnderwaterEffectSettings(const Components::OceanSurface& ocean,
                                   OceanUnderwaterSettings& uwSettings)
{
    // Underwater + reflected-caustic effect knobs -> feature settings
    // (kept off the surface SSBO). Applied by OceanRenderNode to the passes.
    uwSettings.Inscatter = ocean.UnderwaterInscattering;
    uwSettings.Inscatter_Strength = ocean.InscatterStrength;
    uwSettings.Inscatter_PhaseG = ocean.InscatterPhaseG;
    uwSettings.Distortion = ocean.UnderwaterDistortion;
    uwSettings.Distortion_Strength = ocean.DistortionStrength;
    uwSettings.GodRays = ocean.UnderwaterGodRays;
    uwSettings.GodRay_Strength = ocean.GodRayStrength;
    uwSettings.GodRay_Density = ocean.GodRayDensity;
    uwSettings.CausticsOnGeometry = ocean.CausticsOnGeometry;
    uwSettings.WaterlineFadeDistance = ocean.WaterlineFadeDistance;
    uwSettings.ReflectedCaustics_Strength = ocean.ReflectedCausticsStrength;
    uwSettings.ReflectedCaustics_Height = ocean.ReflectedCausticsHeight;
    uwSettings.ReflectedCaustics_Falloff = ocean.ReflectedCausticsFalloff;
}

void ApplySurface(const Components::OceanSurface& ocean, const Components::Transform* transform,
                  const Components::WorldTransform* worldTransform, OceanRenderFeature& feature,
                  OceanExtractionFrame& frame)
{
    frame.MaterialBaseSurface = ocean;
    frame.Params.SeaLevel = ResolveOceanSeaLevel(ocean, transform, worldTransform);
    ApplySurfaceShading(ocean, feature, frame.Params);
    ApplyUnderwaterEffectSettings(ocean, frame.UnderwaterSettings);

    // Flow + dynamic waves. The availability flags are stamped by the
    // contributor at emit (it knows the bake/sim readiness + whether a
    // source/impulse is present), not here; leave them at their default 0.
    // PublishSurfaceTogglesAndTextures stores the authored toggles on the
    // feature so the contributor can gate. DynWavesAmplitude / FlowDetailScale are
    // surface-side scales fed straight through.
    frame.Params.DynWavesAmplitude = 1.0f;
    frame.Params.FlowDetailScale = 1.0f;
    frame.FlowEnabled = ocean.Flow;
    frame.DynWavesEnabled = ocean.DynamicWaves;

    // Optional user texture overrides (normal detail, foam bubbles, caustics).
    // PublishSurfaceTogglesAndTextures resolves them to GPU textures and
    // stores them on the feature.
    frame.FoamTextureGuid = ocean.FoamTexture.ToGuid();
    frame.NormalTextureGuid = ocean.NormalTexture.ToGuid();
    frame.CausticsTextureGuid = ocean.CausticsTexture.ToGuid();

    // Clip surface + albedo. The availability flags are stamped by the
    // contributor at emit (it knows the bake readiness + whether a source
    // is present), not here; leave them at their default 0. The authored
    // toggles are stored on the feature by PublishSurfaceTogglesAndTextures so
    // the contributor can gate. DefaultClippingState is the surface-side clip
    // default fed straight through (and into the clip bake in
    // ExtractClipSources so the gate matches).
    frame.Params.DefaultClippingState = ocean.DefaultClippingState;
    frame.ClipEnabled = ocean.ClipSurface;
    frame.AlbedoEnabled = ocean.Albedo;
}

void AppendWaterBody(const Components::OceanWaterBody& wb, const Components::WorldTransform& wt,
                     const Components::OceanWaveSpectrum* localSpectrum,
                     OceanExtractionFrame& frame)
{
    if (wb.ExtentX <= 0.0f || wb.ExtentZ <= 0.0f)
        return;
    const float32* m = wt.matrix;
    WaterBodyEmit e{};
    e.X = m[12];
    e.Y = m[13];
    e.Z = m[14];
    e.ExtentX = wb.ExtentX;
    e.ExtentZ = wb.ExtentZ;
    e.UnderwaterDepth = std::max(wb.UnderwaterDepth, 0.0f);
    e.FlowX = wb.FlowX;
    e.FlowZ = wb.FlowZ;
    e.ClipFeather = std::max(wb.ClipFeather, 0.0f);
    e.WaveWeight = std::max(wb.WaveWeight, 0.0f);
    e.WaveChop = std::max(wb.WaveChop, 0.0f);
    e.WaveFeather = std::max(wb.WaveFeather, 0.0f);
    e.LocalWaveCount = BuildWaterBodyLocalWavePacket(
        wb.LocalWaveAmplitude, wb.LocalWaveWavelength, wb.LocalWaveDirectionDegrees,
        wb.LocalWaveCount, wb.LocalWaveExtraAmplitude,
        wb.LocalWaveExtraWavelength, wb.LocalWaveExtraDirectionDegrees,
        e.LocalWaves);
    Components::OceanWaveSpectrum assetSpectrum{};
    const Components::OceanWaveSpectrum* resolvedSpectrum = localSpectrum;
    const bool haveSpectrumAsset =
        TryLoadOceanWaveSpectrumAsset(wb.LocalSpectrumAsset, assetSpectrum);
    if (haveSpectrumAsset)
        resolvedSpectrum = &assetSpectrum;
    const bool localSpectrumApplied = TryApplyLocalSpectrumWaveOverride(
        wb.UseLocalSpectrum || haveSpectrumAsset, resolvedSpectrum, e.WaveWeight, e.WaveChop,
        e.LocalWaveCount, e.LocalWaves);
    if (localSpectrumApplied && resolvedSpectrum)
    {
        e.LocalSpectrumApplied = true;
        e.LocalSpectrum = *resolvedSpectrum;
    }
    e.ConfineSurface = wb.ConfineSurface;
    e.UnderwaterVolume = wb.UnderwaterVolume && e.UnderwaterDepth > 0.0f;
    e.Flow = wb.Flow;
    e.WaveOverride = wb.WaveOverride || localSpectrumApplied;
    frame.WaterBodies.push_back(e);
    if (e.ConfineSurface)
    {
        frame.ClipEnabled = true;
        frame.Params.DefaultClippingState = 1.0f;
    }
    if (e.Flow)
        frame.FlowEnabled = true;
}

// Writes the world-space footprint of a polygon body; false for a degenerate one.
bool WritePolygonFootprint(const Components::OceanPolygonWaterBody& wb, const float32* m,
                           uint32 count, PolygonBodyEmit& e)
{
    float32 minX = 0.0f;
    float32 minZ = 0.0f;
    float32 maxX = 0.0f;
    float32 maxZ = 0.0f;
    for (uint32 i = 0; i < count; ++i)
    {
        const float32 lx = wb.PointX[i];
        const float32 lz = wb.PointZ[i];
        const float32 wx = m[0] * lx + m[8] * lz + m[12];
        const float32 wz = m[2] * lx + m[10] * lz + m[14];
        e.Query.X[i] = wx;
        e.Query.Z[i] = wz;
        e.Clip.Points[i][0] = wx;
        e.Clip.Points[i][1] = wz;
        e.FlowPolygon.Points[i][0] = wx;
        e.FlowPolygon.Points[i][1] = wz;
        e.WavePolygon.Points[i][0] = wx;
        e.WavePolygon.Points[i][1] = wz;
        if (i == 0u)
        {
            minX = maxX = wx;
            minZ = maxZ = wz;
        }
        else
        {
            minX = std::min(minX, wx);
            minZ = std::min(minZ, wz);
            maxX = std::max(maxX, wx);
            maxZ = std::max(maxZ, wz);
        }
    }
    if ((maxX - minX) <= 1e-4f || (maxZ - minZ) <= 1e-4f)
        return false;

    e.Clip.Bounds[0] = minX;
    e.Clip.Bounds[1] = minZ;
    e.Clip.Bounds[2] = maxX;
    e.Clip.Bounds[3] = maxZ;
    e.FlowPolygon.Bounds[0] = minX;
    e.FlowPolygon.Bounds[1] = minZ;
    e.FlowPolygon.Bounds[2] = maxX;
    e.FlowPolygon.Bounds[3] = maxZ;
    e.WavePolygon.Bounds[0] = minX;
    e.WavePolygon.Bounds[1] = minZ;
    e.WavePolygon.Bounds[2] = maxX;
    e.WavePolygon.Bounds[3] = maxZ;
    e.CenterX = 0.5f * (minX + maxX);
    e.CenterY = m[13];
    e.CenterZ = 0.5f * (minZ + maxZ);
    e.HalfX = 0.5f * (maxX - minX);
    e.HalfZ = 0.5f * (maxZ - minZ);
    return true;
}

void AppendPolygonWaterBody(const Components::OceanPolygonWaterBody& wb,
                            const Components::WorldTransform& wt,
                            const Components::OceanWaveSpectrum* localSpectrum,
                            OceanExtractionFrame& frame)
{
    const uint32 count = std::min(
        std::min(wb.PointCount, Components::OceanPolygonWaterBodyPointCapacity),
        kMaxOceanClipPolygonPoints);
    if (count < 3u)
        return;

    const float32* m = wt.matrix;
    PolygonBodyEmit e{};
    e.Query.PointCount = count;
    e.Clip.Meta[0] = static_cast<float32>(count);
    e.Clip.Meta[1] = 0.0f; // restore water inside the polygon
    e.Clip.Meta[2] = std::max(wb.ClipFeather, 0.0f);
    e.FlowPolygon.Meta[0] = static_cast<float32>(count);
    e.FlowPolygon.Meta[1] = std::max(wb.ClipFeather, 0.0f);
    e.WavePolygon.Meta[0] = static_cast<float32>(count);
    e.WavePolygon.Meta[1] = std::max(wb.WaveFeather, 0.0f);

    if (!WritePolygonFootprint(wb, m, count, e))
        return;

    e.UnderwaterDepth = std::max(wb.UnderwaterDepth, 0.0f);
    e.FlowX = wb.FlowX;
    e.FlowZ = wb.FlowZ;
    e.FlowPolygon.FlowVelocity[0] = wb.FlowX;
    e.FlowPolygon.FlowVelocity[1] = wb.FlowZ;
    e.WavePolygon.WaveChop[0] = std::max(wb.WaveWeight, 0.0f);
    e.WavePolygon.WaveChop[1] = std::max(wb.WaveChop, 0.0f);
    e.WavePolygon.Meta[2] = static_cast<float32>(BuildWaterBodyLocalWavePacket(
        wb.LocalWaveAmplitude, wb.LocalWaveWavelength, wb.LocalWaveDirectionDegrees,
        wb.LocalWaveCount, wb.LocalWaveExtraAmplitude,
        wb.LocalWaveExtraWavelength, wb.LocalWaveExtraDirectionDegrees,
        e.WavePolygon.LocalWaves));
    uint32 localWaveCount = static_cast<uint32>(e.WavePolygon.Meta[2]);
    float32 localWeight = e.WavePolygon.WaveChop[0];
    float32 localChop = e.WavePolygon.WaveChop[1];
    Components::OceanWaveSpectrum assetSpectrum{};
    const Components::OceanWaveSpectrum* resolvedSpectrum = localSpectrum;
    const bool haveSpectrumAsset =
        TryLoadOceanWaveSpectrumAsset(wb.LocalSpectrumAsset, assetSpectrum);
    if (haveSpectrumAsset)
        resolvedSpectrum = &assetSpectrum;
    const bool localSpectrumApplied = TryApplyLocalSpectrumWaveOverride(
        wb.UseLocalSpectrum || haveSpectrumAsset, resolvedSpectrum, localWeight, localChop,
        localWaveCount, e.WavePolygon.LocalWaves);
    e.WavePolygon.WaveChop[0] = localWeight;
    e.WavePolygon.WaveChop[1] = localChop;
    if (localSpectrumApplied && resolvedSpectrum)
    {
        e.LocalSpectrumApplied = true;
        e.LocalSpectrum = *resolvedSpectrum;
    }
    e.WavePolygon.Meta[2] = static_cast<float32>(localWaveCount);
    e.ConfineSurface = wb.ConfineSurface;
    e.UnderwaterVolume = wb.UnderwaterVolume && e.UnderwaterDepth > 0.0f;
    e.Flow = wb.Flow;
    e.WaveOverride = wb.WaveOverride || localSpectrumApplied;
    frame.PolygonWaterBodies.push_back(e);
    if (e.ConfineSurface)
    {
        frame.ClipEnabled = true;
        frame.Params.DefaultClippingState = 1.0f;
    }
    if (e.Flow)
        frame.FlowEnabled = true;
}

void SubmitLocalFftCandidate(std::vector<LocalFftCandidate>& localFftCandidates,
                             const Components::OceanWaveSpectrum& spectrum, float distanceSq,
                             uint32 order)
{
    for (LocalFftCandidate& existing : localFftCandidates)
    {
        if (!OceanWaveSpectrumEquivalent(existing.Spectrum, spectrum))
            continue;
        existing.DistanceSq = std::min(existing.DistanceSq, distanceSq);
        existing.Order = std::min(existing.Order, order);
        return;
    }

    LocalFftCandidate candidate{};
    candidate.Spectrum = spectrum;
    candidate.DistanceSq = distanceSq;
    candidate.Order = order;
    localFftCandidates.push_back(candidate);
}

std::vector<LocalFftCandidate> RankLocalFftCandidates(const OceanExtractionFrame& frame)
{
    std::vector<LocalFftCandidate> localFftCandidates;
    localFftCandidates.reserve(frame.WaterBodies.size() + frame.PolygonWaterBodies.size());
    uint32 localFftOrder = 0u;
    for (const WaterBodyEmit& wb : frame.WaterBodies)
    {
        if (wb.LocalSpectrumApplied)
        {
            const float distanceSq = frame.StreamCamera.Valid
                ? DistanceSqToCenteredRectXZ(frame.StreamCamera.X, frame.StreamCamera.Z,
                                             wb.X, wb.Z, wb.ExtentX, wb.ExtentZ)
                : 0.0f;
            SubmitLocalFftCandidate(localFftCandidates, wb.LocalSpectrum, distanceSq, localFftOrder);
        }
        ++localFftOrder;
    }
    for (const PolygonBodyEmit& wb : frame.PolygonWaterBodies)
    {
        if (wb.LocalSpectrumApplied)
        {
            const float distanceSq = frame.StreamCamera.Valid
                ? DistanceSqToCenteredRectXZ(frame.StreamCamera.X, frame.StreamCamera.Z,
                                             wb.CenterX, wb.CenterZ, wb.HalfX, wb.HalfZ)
                : 0.0f;
            SubmitLocalFftCandidate(localFftCandidates, wb.LocalSpectrum, distanceSq, localFftOrder);
        }
        ++localFftOrder;
    }
    std::stable_sort(
        localFftCandidates.begin(), localFftCandidates.end(),
        [](const LocalFftCandidate& a, const LocalFftCandidate& b)
        {
            if (a.DistanceSq < b.DistanceSq)
                return true;
            if (a.DistanceSq > b.DistanceSq)
                return false;
            return a.Order < b.Order;
        });
    return localFftCandidates;
}

uint32 ResolveLocalFftStream(const std::vector<LocalFftCandidate>& localFftCandidates,
                             const Components::OceanWaveSpectrum& spectrum)
{
    for (const LocalFftCandidate& candidate : localFftCandidates)
    {
        if (candidate.Stream < kMaxOceanLocalFFTStreams &&
            OceanWaveSpectrumEquivalent(candidate.Spectrum, spectrum))
        {
            return candidate.Stream;
        }
    }
    return kMaxOceanLocalFFTStreams;
}

void AssignLocalFftStreams(OceanExtractionFrame& frame)
{
    std::vector<LocalFftCandidate> localFftCandidates = RankLocalFftCandidates(frame);

    std::vector<Components::OceanWaveSpectrum>& localFftSpecs = frame.LocalFftSpecs;
    localFftSpecs.reserve(kMaxOceanLocalFFTStreams);
    localFftSpecs.clear();
    const uint32 selectedLocalFftCount =
        std::min<uint32>(static_cast<uint32>(localFftCandidates.size()),
                         kMaxOceanLocalFFTStreams);
    for (uint32 stream = 0u; stream < selectedLocalFftCount; ++stream)
    {
        localFftCandidates[stream].Stream = stream;
        localFftSpecs.push_back(localFftCandidates[stream].Spectrum);
    }

    // The render path exposes eight local FFT streams via two RGBA mask pages.
    // Pick them by viewpoint relevance; overflow bodies keep their spectrum-
    // derived compact local wave packets layered over the shared global spectrum.
    for (WaterBodyEmit& wb : frame.WaterBodies)
    {
        wb.LocalFFTBlend = 0.0f;
        wb.LocalFFTStream = 0u;
        if (!wb.LocalSpectrumApplied)
            continue;
        const uint32 stream = ResolveLocalFftStream(localFftCandidates, wb.LocalSpectrum);
        if (stream >= kMaxOceanLocalFFTStreams)
            continue;
        wb.LocalFFTBlend = 1.0f;
        wb.LocalFFTStream = stream;
    }
    for (PolygonBodyEmit& wb : frame.PolygonWaterBodies)
    {
        wb.WavePolygon.WaveChop[2] = 0.0f;
        wb.WavePolygon.WaveChop[3] = 0.0f;
        if (!wb.LocalSpectrumApplied)
            continue;
        const uint32 stream = ResolveLocalFftStream(localFftCandidates, wb.LocalSpectrum);
        if (stream >= kMaxOceanLocalFFTStreams)
            continue;
        wb.WavePolygon.WaveChop[2] = 1.0f;
        wb.WavePolygon.WaveChop[3] = static_cast<float32>(stream);
    }
}

void CollectConfiningWaterBodies(OceanExtractionFrame& frame)
{
    for (const WaterBodyEmit& wb : frame.WaterBodies)
    {
        if (!wb.ConfineSurface)
            continue;
        OceanRenderFeature::WaterBodyBox box{};
        box.CenterX = wb.X;
        box.CenterZ = wb.Z;
        box.HalfX = wb.ExtentX;
        box.HalfZ = wb.ExtentZ;
        frame.WaterBodyBounds.push_back(box);
        frame.WaterBodiesConstrainSurface = true;
    }
    for (const PolygonBodyEmit& wb : frame.PolygonWaterBodies)
    {
        if (!wb.ConfineSurface)
            continue;
        frame.WaterBodyPolygons.push_back(wb.Query);
        frame.WaterBodiesConstrainSurface = true;
    }
}

void ApplyQualityTier(OceanExtractionFrame& frame)
{
    // Apply the quality tier (ANDed onto the authored toggles). Medium drops flow
    // + dynamic waves; High keeps all. Wave mode is no longer an authoring option:
    // the surface uses FFT when available and falls back analytically only if the
    // FFT resources are unavailable.
    // Clip / albedo / seabed depth stay content-gated, so the tier leaves them be.
    if (frame.HasSurface)
    {
        frame.FlowEnabled = frame.FlowEnabled && OceanTierAllowsFlow(frame.Tier);
        frame.DynWavesEnabled = frame.DynWavesEnabled && OceanTierAllowsDynWaves(frame.Tier);
    }
}

void ApplySunLight(const Components::Light& light, const Components::WorldTransform& wt,
                   OceanParamsGPU& params)
{
    // Shine is +Z (col2). OceanParams wants surface-to-sun,
    // the opposite of travel direction.
    const float32* m = wt.matrix;
    const float32 fx = -m[8], fy = -m[9], fz = -m[10];
    float32 len = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (len < 1e-6f)
        len = 1.0f;
    params.SunDirection[0] = fx / len;
    params.SunDirection[1] = fy / len;
    params.SunDirection[2] = fz / len;
    // Resolve color temperature + intensity unit so ocean matches world meshes.
    float sunColor[3];
    float sunIntensity;
    Components::ResolveLightColorIntensity(light, sunColor, sunIntensity);
    params.SunColor[0] = sunColor[0] * sunIntensity;
    params.SunColor[1] = sunColor[1] * sunIntensity;
    params.SunColor[2] = sunColor[2] * sunIntensity;
}

void ExtractSun(ECS::World& world, OceanParamsGPU& params)
{
    // Main directional light → sun direction (toward the sun) + colour, so the
    // surface shader can do sun-driven subsurface scattering and glitter.
    bool foundSun = false;
    world.Query<ECS::Read<Components::Light>, ECS::Read<Components::WorldTransform>>()
        .Each(
            [&](ECS::EntityHandle, const Components::Light& light,
                const Components::WorldTransform& wt)
            {
                if (foundSun || !light.CastsLight ||
                    light.Type != Components::LightType::Directional)
                    return;
                foundSun = true;
                ApplySunLight(light, wt, params);
            });
}

void AddSavedDepthCacheCandidate(const Components::OceanDepthCacheSource& cache,
                                 const DepthCacheStreamCamera& depthCacheStreamCamera,
                                 OceanInputFrameStats& inputStats,
                                 std::vector<DepthCacheCandidate>& savedDepthCacheCandidates)
{
    if ((cache.Path[0] == '\0' && cache.CacheAsset.IsNull()))
        return;
    const std::filesystem::path resolvedPath = ResolveOceanDepthCacheSourcePath(cache);
    if (resolvedPath.empty())
        return;
    OceanSavedDepthCacheSource source{};
    source.Path = resolvedPath;
    source.SourceRevision = cache.SourceRevision;
    source.BakedRevision = cache.BakedRevision;
    source.CacheRevision = cache.CacheRevision;
    source.UseWhenStale = cache.UseWhenStale;

    float distanceSq = 0.0f;
    const float streamRadius = std::max(cache.StreamRadius, 0.0f);
    if (depthCacheStreamCamera.Valid)
    {
        distanceSq = DistanceSqToRectXZ(
            depthCacheStreamCamera.X, depthCacheStreamCamera.Z,
            cache.BakeOriginX, cache.BakeOriginZ,
            std::max(cache.BakeSizeX, 0.0f), std::max(cache.BakeSizeZ, 0.0f));
        if (streamRadius > 0.0f && distanceSq > streamRadius * streamRadius)
        {
            ++inputStats.DroppedSavedDepthCaches;
            return;
        }
    }

    DepthCacheCandidate candidate{};
    candidate.Source = std::move(source);
    candidate.DistanceSq = distanceSq;
    candidate.Priority = cache.StreamPriority;
    candidate.Order = static_cast<uint32>(savedDepthCacheCandidates.size());
    savedDepthCacheCandidates.push_back(std::move(candidate));
}

void ActivateSavedDepthCaches(std::vector<DepthCacheCandidate>& savedDepthCacheCandidates,
                              OceanExtractionFrame& frame)
{
    OceanInputRegistryFrame& inputs = frame.Inputs;
    auto& savedDepthCacheSources = inputs.SavedDepthCaches;
    if (!savedDepthCacheCandidates.empty())
    {
        std::stable_sort(
            savedDepthCacheCandidates.begin(), savedDepthCacheCandidates.end(),
            [](const DepthCacheCandidate& a, const DepthCacheCandidate& b) {
                if (a.Priority != b.Priority)
                    return a.Priority > b.Priority;
                if (a.DistanceSq != b.DistanceSq)
                    return a.DistanceSq < b.DistanceSq;
                return a.Order < b.Order;
            });

        const uint32 candidateCount = static_cast<uint32>(savedDepthCacheCandidates.size());
        uint32 activeCount = candidateCount;
        if (frame.MaxActiveDepthCaches != 0u)
            activeCount = std::min(activeCount, frame.MaxActiveDepthCaches);

        savedDepthCacheSources.reserve(activeCount);
        for (uint32 i = 0; i < activeCount; ++i)
            inputs.AddSavedDepthCache(savedDepthCacheCandidates[i].Source);

        if (activeCount < candidateCount)
            inputs.Stats.DroppedSavedDepthCaches += candidateCount - activeCount;
    }
}

void GatherSavedDepthCaches(ECS::World& world, OceanExtractionFrame& frame)
{
    // Optional saved depth caches. These supply broad baked terrain/geometry
    // depth before analytic seabeds and dynamic contributors refine it.
    std::vector<DepthCacheCandidate> savedDepthCacheCandidates;
    world.Query<ECS::Read<Components::OceanDepthCacheSource>>()
        .Each(
            [&](ECS::EntityHandle, const Components::OceanDepthCacheSource& cache)
            {
                AddSavedDepthCacheCandidate(cache, frame.StreamCamera, frame.Inputs.Stats,
                                            savedDepthCacheCandidates);
            });

    ActivateSavedDepthCaches(savedDepthCacheCandidates, frame);
    frame.Inputs.Stats.SavedDepthCacheCandidates =
        static_cast<uint32>(savedDepthCacheCandidates.size());
}

void AddSeabed(const Components::OceanSeabed& sb, const Components::WorldTransform& wt,
               OceanInputRegistryFrame& inputs)
{
    if (sb.ExtentX <= 0.0f || sb.ExtentZ <= 0.0f)
        return;
    const float32* m = wt.matrix;
    OceanSeabedGPU g{};
    g.OriginExtent[0] = m[12]; // world X of the entity origin
    g.OriginExtent[1] = m[14]; // world Z of the entity origin
    g.OriginExtent[2] = sb.ExtentX;
    g.OriginExtent[3] = sb.ExtentZ;
    g.HeightSlope[0] = sb.BaseHeight;
    g.HeightSlope[1] = sb.SlopeX;
    g.HeightSlope[2] = sb.SlopeZ;
    g.HeightSlope[3] = 0.0f;
    inputs.AddSeabed(g);
}

void AddDepthContributor(const Components::OceanDepthContributor& dc,
                         const Components::WorldTransform& wt, OceanInputRegistryFrame& inputs)
{
    if (dc.ExtentX <= 0.0f || dc.ExtentZ <= 0.0f || dc.Depth < 0.0f)
        return;
    const float32* m = wt.matrix;
    OceanDepthContributorGPU g{};
    g.OriginExtent[0] = m[12];
    g.OriginExtent[1] = m[14];
    g.OriginExtent[2] = dc.ExtentX;
    g.OriginExtent[3] = dc.ExtentZ;
    g.DepthShape[0] = dc.Depth;
    g.DepthShape[1] = std::max(dc.Feather, 0.0f);
    g.DepthShape[2] = std::clamp(dc.Roundness, 0.0f, 1.0f);
    inputs.AddDepthContributor(g);
}

void AddMeshDepthContributor(const Components::OceanMeshDepthContributor& mc,
                             const Components::WorldTransform& wt,
                             const Components::LocalBounds& bounds,
                             const Components::MeshRenderer* renderer, float32 seaLevel,
                             OceanInputRegistryFrame& inputs)
{
    if (renderer && !renderer->RenderLayers.Intersects(mc.ContributorLayers))
        return;

    const Mathematics::AABB aabb = bounds.Box.TransformToAABB(wt.matrix);
    if (aabb.max.y < aabb.min.y || aabb.max.x < aabb.min.x || aabb.max.z < aabb.min.z)
        return;
    if (aabb.min.y > seaLevel)
        return; // entirely above the calm water plane

    const float32 centerX = 0.5f * (aabb.min.x + aabb.max.x);
    const float32 centerZ = 0.5f * (aabb.min.z + aabb.max.z);
    const float32 extentX = 0.5f * (aabb.max.x - aabb.min.x) + std::max(mc.ExtentPadding, 0.0f);
    const float32 extentZ = 0.5f * (aabb.max.z - aabb.min.z) + std::max(mc.ExtentPadding, 0.0f);
    if (extentX <= 0.0f || extentZ <= 0.0f)
        return;
    const float32 minDepth = std::max(mc.MinDepth, 0.0f);
    const float32 maxDepth = std::max(mc.MaxDepth, minDepth);
    const float32 depth = std::clamp(seaLevel - aabb.max.y + mc.DepthBias,
                                     minDepth, maxDepth);

    OceanDepthContributorGPU g{};
    g.OriginExtent[0] = centerX;
    g.OriginExtent[1] = centerZ;
    g.OriginExtent[2] = extentX;
    g.OriginExtent[3] = extentZ;
    g.DepthShape[0] = depth;
    g.DepthShape[1] = std::max(mc.Feather, 0.0f);
    g.DepthShape[2] = std::clamp(mc.Roundness, 0.0f, 1.0f);
    inputs.AddDepthContributor(g);
}

void GatherDepthSources(ECS::World& world, OceanExtractionFrame& frame)
{
    OceanInputRegistryFrame& inputs = frame.Inputs;
    // Tagged seabeds → analytic seabed planes for the depth bake. Read each
    // seabed entity's world transform (origin XZ supplies the footprint center).
    // Capped at kMaxOceanSeabeds; extra seabeds are ignored.
    world.Query<ECS::Read<Components::OceanSeabed>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle, const Components::OceanSeabed& sb,
                  const Components::WorldTransform& wt) { AddSeabed(sb, wt, inputs); });

    // Dynamic depth contributors → temporary analytic footprints in the same depth
    // cascade. These are separate from intersection foam: they alter the cached
    // depth sampled by shallow colour, shoreline foam, and shallow-wave attenuation.
    world.Query<ECS::Read<Components::OceanDepthContributor>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle, const Components::OceanDepthContributor& dc,
                  const Components::WorldTransform& wt) { AddDepthContributor(dc, wt, inputs); });

    world.Query<ECS::Read<Components::OceanMeshDepthContributor>,
                ECS::Read<Components::WorldTransform>,
                ECS::Read<Components::LocalBounds>,
                ECS::Optional<Components::MeshRenderer>,
                ECS::Optional<ECS::ComponentDisabled<Components::MeshRenderer>>>()
        .Each(
            [&](ECS::EntityHandle,
                const Components::OceanMeshDepthContributor& mc,
                const Components::WorldTransform& wt,
                const Components::LocalBounds& bounds,
                const Components::MeshRenderer* renderer,
                const ECS::ComponentDisabled<Components::MeshRenderer>* rendererOff)
            {
                if (rendererOff)
                    return;
                AddMeshDepthContributor(mc, wt, bounds, renderer, frame.Params.SeaLevel, inputs);
            });
}

OceanWaterMaterialGPU* AppendWaterMaterial(OceanPresetCache& presets,
                                           Engine::Renderer::RenderServices& renderServices,
                                           const GUID& presetGuid, int32 priority, uint32 entity,
                                           const Components::OceanSurface& materialBaseSurface,
                                           std::vector<OceanWaterMaterialGPU>& waterMaterials)
{
    const auto preset = presets.Find(presetGuid);
    if (!preset)
        return nullptr;
    auto surface = materialBaseSurface;
    preset->Apply("Surface", ECS::GetComponentTypeId<Components::OceanSurface>(), &surface,
                         sizeof(surface), {});
    auto material = BuildOceanWaterMaterial(surface);
    material.EntityId = entity;
    material.Priority = priority;
    const GUID textures[] = {surface.FoamTexture.ToGuid(), surface.NormalTexture.ToGuid()};
    for (uint32 i = 0; i < 2; ++i)
        if (!textures[i].IsNull())
        {
            renderServices.Textures().DeclareTextureClassification(
                textures[i], TextureColorSpace::Linear,
                i == 0 ? TextureCookUsage::Mask : TextureCookUsage::Normal);
            const auto texture = renderServices.Textures().GetOrUpload(textures[i]);
            if (texture.IsValid())
                (i == 0 ? material.FoamTexture : material.NormalTexture) =
                    renderServices.Textures().GetBindlessIndex(texture);
        }
    waterMaterials.push_back(material);
    return &waterMaterials.back();
}

void SetWaterMaterialRectangle(OceanWaterMaterialGPU& m, const Components::OceanWaterBody& body,
                               const Components::WorldTransform& wt)
{
    const float x = wt.matrix[12], z = wt.matrix[14];
    m.Bounds[0] = x - body.ExtentX;
    m.Bounds[1] = z - body.ExtentZ;
    m.Bounds[2] = x + body.ExtentX;
    m.Bounds[3] = z + body.ExtentZ;
    m.PointCount = 4;
    for (uint32 i = 0; i < 4; ++i)
    {
        m.Points[i][0] = (i == 0 || i == 3) ? m.Bounds[0] : m.Bounds[2];
        m.Points[i][1] = i < 2 ? m.Bounds[1] : m.Bounds[3];
    }
}

void SetWaterMaterialPolygon(OceanWaterMaterialGPU& m, const Components::OceanPolygonWaterBody& body,
                             const Components::WorldTransform& wt)
{
    const uint32 count = std::min(body.PointCount, 8u);
    m.PointCount = static_cast<float>(count);
    m.Bounds[0] = m.Bounds[1] = std::numeric_limits<float>::max();
    m.Bounds[2] = m.Bounds[3] = -std::numeric_limits<float>::max();
    for (uint32 i = 0; i < count; ++i)
    {
        const float x = wt.matrix[0] * body.PointX[i] + wt.matrix[8] * body.PointZ[i] + wt.matrix[12];
        const float z =
            wt.matrix[2] * body.PointX[i] + wt.matrix[10] * body.PointZ[i] + wt.matrix[14];
        m.Points[i][0] = x;
        m.Points[i][1] = z;
        m.Bounds[0] = std::min(m.Bounds[0], x);
        m.Bounds[1] = std::min(m.Bounds[1], z);
        m.Bounds[2] = std::max(m.Bounds[2], x);
        m.Bounds[3] = std::max(m.Bounds[3], z);
    }
}

OceanInputDrawPacket MakeTypedInputPacket(ECS::EntityHandle entity,
                                          const Components::WorldTransform& wt,
                                          OceanInputFamily family, int32 priority,
                                          Components::OceanInputBlend blend,
                                          Components::OceanInputGeometryType geometry,
                                          float32 extentX, float32 extentZ, float32 feather)
{
    OceanInputDrawPacket packet{};
    packet.Family = family;
    packet.Blend = static_cast<OceanInputBlendMode>(blend);
    packet.Geometry = static_cast<OceanInputGeometry>(geometry);
    packet.Priority = priority;
    packet.EntityId = entity.id;
    packet.CenterX = wt.matrix[12];
    packet.CenterZ = wt.matrix[14];
    packet.ExtentX = std::max(extentX, 0.0f);
    packet.ExtentZ = std::max(extentZ, 0.0f);
    packet.Feather = std::max(feather, 0.0f);
    return packet;
}

void AppendAnimatedWavePacket(ECS::EntityHandle entity,
                              const Components::OceanAnimatedWaveInput& input,
                              const Components::WorldTransform& wt,
                              std::vector<OceanInputDrawPacket>& packets)
{
    if (input.ExtentX <= 0.0f || input.ExtentZ <= 0.0f)
        return;
    OceanInputDrawPacket packet = MakeTypedInputPacket(
        entity, wt, OceanInputFamily::AnimatedWaves, input.Priority, input.Blend,
        input.Geometry, input.ExtentX, input.ExtentZ, input.Feather);
    const float32 radians = input.DirectionDegrees * 0.01745329252f;
    packet.Value[0] = input.Amplitude;
    packet.Value[1] = std::max(input.Wavelength, 0.1f);
    packet.Value[2] = std::cos(radians);
    packet.Value[3] = std::sin(radians);
    packets.push_back(packet);
}

void AppendGerstnerShapePackets(ECS::EntityHandle entity,
                                const Components::OceanGerstnerShape& input,
                                const Components::WorldTransform& wt,
                                std::vector<OceanInputDrawPacket>& packets)
{
    if (input.ExtentX <= 0.0f || input.ExtentZ <= 0.0f)
        return;
    const uint32 count = std::min(input.WaveCount,
                                  Components::OceanGerstnerShapeWaveCapacity);
    for (uint32 wave = 0u; wave < count; ++wave)
    {
        OceanInputDrawPacket packet = MakeTypedInputPacket(
            entity, wt, OceanInputFamily::AnimatedWaves, input.Priority,
            Components::OceanInputBlend::Additive,
            Components::OceanInputGeometryType::Rectangle,
            input.ExtentX, input.ExtentZ, input.Feather);
        const float32 radians = input.DirectionDegrees[wave] * 0.01745329252f;
        packet.Value[0] = input.Amplitude[wave];
        packet.Value[1] = std::max(input.Wavelength[wave], 0.1f);
        packet.Value[2] = std::cos(radians);
        packet.Value[3] = std::sin(radians);
        packets.push_back(packet);
    }
}

void AppendHeightPacket(ECS::EntityHandle entity, const Components::OceanHeightInput& input,
                        const Components::WorldTransform& wt,
                        std::vector<OceanInputDrawPacket>& packets)
{
    if (input.ExtentX <= 0.0f || input.ExtentZ <= 0.0f)
        return;
    OceanInputDrawPacket packet = MakeTypedInputPacket(
        entity, wt, OceanInputFamily::Height, input.Priority, input.Blend,
        input.Geometry, input.ExtentX, input.ExtentZ, input.Feather);
    packet.Value[0] = input.Height;
    packet.Value[1] = input.AbsoluteHeight ? 1.0f : 0.0f;
    packets.push_back(packet);
}

void AppendFoamPacket(ECS::EntityHandle entity, const Components::OceanFoamInput& input,
                      const Components::WorldTransform& wt,
                      std::vector<OceanInputDrawPacket>& packets)
{
    if (input.ExtentX <= 0.0f || input.ExtentZ <= 0.0f)
        return;
    OceanInputDrawPacket packet = MakeTypedInputPacket(
        entity, wt, OceanInputFamily::Foam, input.Priority, input.Blend,
        input.Geometry, input.ExtentX, input.ExtentZ, input.Feather);
    packet.Value[0] = input.Amount;
    packet.Value[1] = input.UseVertexColor ? 1.0f : 0.0f;
    packets.push_back(packet);
}

void AppendDynamicWavePacket(ECS::EntityHandle entity,
                             const Components::OceanDynamicWaveInput& input,
                             const Components::WorldTransform& wt,
                             std::vector<OceanInputDrawPacket>& packets)
{
    if (input.ExtentX <= 0.0f || input.ExtentZ <= 0.0f)
        return;
    OceanInputDrawPacket packet = MakeTypedInputPacket(
        entity, wt, OceanInputFamily::DynamicWaves, input.Priority, input.Blend,
        input.Geometry, input.ExtentX, input.ExtentZ, input.Feather);
    packet.Value[0] = input.Amplitude;
    packets.push_back(packet);
}

void AppendFlowPacket(ECS::EntityHandle entity, const Components::OceanFlowInput& input,
                      const Components::WorldTransform& wt,
                      std::vector<OceanInputDrawPacket>& packets)
{
    if (input.ExtentX <= 0.0f || input.ExtentZ <= 0.0f)
        return;
    OceanInputDrawPacket packet = MakeTypedInputPacket(
        entity, wt, OceanInputFamily::Flow, input.Priority, input.Blend,
        input.Geometry, input.ExtentX, input.ExtentZ, input.Feather);
    packet.Value[0] = input.FlowX;
    packet.Value[1] = input.FlowZ;
    packets.push_back(packet);
}

void AppendClipPacket(ECS::EntityHandle entity, const Components::OceanClipInput& input,
                      const Components::WorldTransform& wt,
                      std::vector<OceanInputDrawPacket>& packets)
{
    if (input.ExtentX <= 0.0f || input.ExtentZ <= 0.0f)
        return;
    OceanInputDrawPacket packet = MakeTypedInputPacket(
        entity, wt, OceanInputFamily::Clip, input.Priority, input.Blend,
        input.Geometry, input.ExtentX, input.ExtentZ, input.Feather);
    packet.Value[0] = input.ClipState;
    packets.push_back(packet);
}

void AppendAlbedoPacket(ECS::EntityHandle entity, const Components::OceanAlbedoInput& input,
                        const Components::WorldTransform& wt,
                        std::vector<OceanInputDrawPacket>& packets)
{
    if (input.ExtentX <= 0.0f || input.ExtentZ <= 0.0f)
        return;
    OceanInputDrawPacket packet = MakeTypedInputPacket(
        entity, wt, OceanInputFamily::Albedo, input.Priority, input.Blend,
        input.Geometry, input.ExtentX, input.ExtentZ, input.Feather);
    packet.Value[0] = input.Color.r;
    packet.Value[1] = input.Color.g;
    packet.Value[2] = input.Color.b;
    packet.Value[3] = input.Coverage;
    packets.push_back(packet);
}

void AppendDepthPacket(ECS::EntityHandle entity, const Components::OceanDepthInput& input,
                       const Components::WorldTransform& wt,
                       std::vector<OceanInputDrawPacket>& packets)
{
    if (input.ExtentX <= 0.0f || input.ExtentZ <= 0.0f)
        return;
    OceanInputDrawPacket packet = MakeTypedInputPacket(
        entity, wt, OceanInputFamily::Depth, input.Priority, input.Blend,
        input.Geometry, input.ExtentX, input.ExtentZ, input.Feather);
    packet.Value[0] = input.Depth;
    packet.Value[1] = input.Roundness;
    packets.push_back(packet);
}

void AppendShadowPacket(ECS::EntityHandle entity, const Components::OceanShadowInput& input,
                        const Components::WorldTransform& wt,
                        std::vector<OceanInputDrawPacket>& packets)
{
    if (input.ExtentX <= 0.0f || input.ExtentZ <= 0.0f)
        return;
    OceanInputDrawPacket packet = MakeTypedInputPacket(
        entity, wt, OceanInputFamily::Shadow, input.Priority, input.Blend,
        input.Geometry, input.ExtentX, input.ExtentZ, input.Feather);
    packet.Value[0] = input.HardShadow;
    packet.Value[1] = input.SoftShadow;
    packets.push_back(packet);
}

void GatherTypedInputPackets(ECS::World& world, std::vector<OceanInputDrawPacket>& packets)
{
    world.Query<ECS::Read<Components::OceanAnimatedWaveInput>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanAnimatedWaveInput& input,
                  const Components::WorldTransform& wt)
              { AppendAnimatedWavePacket(entity, input, wt, packets); });
    world.Query<ECS::Read<Components::OceanGerstnerShape>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanGerstnerShape& input,
                  const Components::WorldTransform& wt)
              { AppendGerstnerShapePackets(entity, input, wt, packets); });
    world.Query<ECS::Read<Components::OceanHeightInput>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanHeightInput& input,
                  const Components::WorldTransform& wt)
              { AppendHeightPacket(entity, input, wt, packets); });
    world.Query<ECS::Read<Components::OceanFoamInput>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanFoamInput& input,
                  const Components::WorldTransform& wt)
              { AppendFoamPacket(entity, input, wt, packets); });
    world.Query<ECS::Read<Components::OceanDynamicWaveInput>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanDynamicWaveInput& input,
                  const Components::WorldTransform& wt)
              { AppendDynamicWavePacket(entity, input, wt, packets); });
    world.Query<ECS::Read<Components::OceanFlowInput>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanFlowInput& input,
                  const Components::WorldTransform& wt)
              { AppendFlowPacket(entity, input, wt, packets); });
    world.Query<ECS::Read<Components::OceanClipInput>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanClipInput& input,
                  const Components::WorldTransform& wt)
              { AppendClipPacket(entity, input, wt, packets); });
    world.Query<ECS::Read<Components::OceanAlbedoInput>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanAlbedoInput& input,
                  const Components::WorldTransform& wt)
              { AppendAlbedoPacket(entity, input, wt, packets); });
    world.Query<ECS::Read<Components::OceanDepthInput>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanDepthInput& input,
                  const Components::WorldTransform& wt)
              { AppendDepthPacket(entity, input, wt, packets); });
    world.Query<ECS::Read<Components::OceanShadowInput>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanShadowInput& input,
                  const Components::WorldTransform& wt)
              { AppendShadowPacket(entity, input, wt, packets); });
}

OceanWaveMaskSourceGPU MakeInputWaveMaskSource(const OceanInputDrawPacket& packet,
                                               float32 seaLevel)
{
    OceanWaveMaskSourceGPU source{};
    source.OriginExtent[0] = packet.CenterX;
    source.OriginExtent[1] = packet.CenterZ;
    source.OriginExtent[2] = packet.ExtentX;
    source.OriginExtent[3] = packet.ExtentZ;
    source.WaveChopFeather[2] = packet.Feather;
    source.LocalFFTBlend[3] = static_cast<float32>(packet.Blend);
    if (packet.Family == OceanInputFamily::AnimatedWaves)
    {
        source.WaveChopFeather[0] =
            packet.Blend == OceanInputBlendMode::Additive ? 0.0f : 1.0f;
        source.WaveChopFeather[1] =
            packet.Blend == OceanInputBlendMode::Additive ? 0.0f : 1.0f;
        source.WaveChopFeather[3] = 1.0f;
        source.LocalWaves[0][0] = packet.Value[0];
        source.LocalWaves[0][1] = packet.Value[1];
        source.LocalWaves[0][2] = packet.Value[2];
        source.LocalWaves[0][3] = packet.Value[3];
    }
    else
    {
        const bool absolute = packet.Value[1] > 0.5f;
        source.WaveChopFeather[0] = absolute ? 0.0f :
            (packet.Blend == OceanInputBlendMode::Additive ? 0.0f : 1.0f);
        source.WaveChopFeather[1] = absolute ? 0.0f :
            (packet.Blend == OceanInputBlendMode::Additive ? 0.0f : 1.0f);
        source.LocalFFTBlend[2] = absolute
                                      ? packet.Value[0] - seaLevel
                                      : packet.Value[0];
        if (absolute)
            source.LocalFFTBlend[3] = static_cast<float32>(OceanInputBlendMode::Replace);
    }
    return source;
}

void RouteTypedInputPacket(const OceanInputDrawPacket& packet, OceanExtractionFrame& frame)
{
    OceanInputRegistryFrame& inputs = frame.Inputs;
    switch (packet.Family)
    {
    case OceanInputFamily::AnimatedWaves:
    case OceanInputFamily::Height:
        inputs.AddWaveMaskSource(MakeInputWaveMaskSource(packet, frame.Params.SeaLevel));
        break;
    case OceanInputFamily::Depth:
    {
        OceanDepthContributorGPU source{};
        source.OriginExtent[0] = packet.CenterX;
        source.OriginExtent[1] = packet.CenterZ;
        source.OriginExtent[2] = packet.ExtentX;
        source.OriginExtent[3] = packet.ExtentZ;
        source.DepthShape[0] = std::max(packet.Value[0], 0.0f);
        source.DepthShape[1] = packet.Feather;
        source.DepthShape[2] = std::clamp(packet.Value[1], 0.0f, 1.0f);
        inputs.AddDepthContributor(source);
        break;
    }
    case OceanInputFamily::Flow:
    {
        frame.FlowEnabled = true;
        OceanFlowSourceGPU source{};
        source.OriginExtent[0] = packet.CenterX;
        source.OriginExtent[1] = packet.CenterZ;
        source.OriginExtent[2] = packet.ExtentX;
        source.OriginExtent[3] = packet.ExtentZ;
        source.FlowVelocity[0] = packet.Value[0];
        source.FlowVelocity[1] = packet.Value[1];
        source.FlowVelocity[2] = packet.Feather;
        source.FlowVelocity[3] = static_cast<float32>(packet.Blend) + 1.0f;
        inputs.AddFlowSource(source);
        break;
    }
    case OceanInputFamily::Clip:
    {
        frame.ClipEnabled = true;
        OceanClipSourceGPU source{};
        source.OriginExtent[0] = packet.CenterX;
        source.OriginExtent[1] = packet.CenterZ;
        source.OriginExtent[2] = packet.ExtentX;
        source.OriginExtent[3] = packet.ExtentZ;
        source.ClipState[0] = packet.Value[0];
        source.ClipState[1] = packet.Feather;
        source.ClipState[2] = static_cast<float32>(packet.Blend) + 1.0f;
        inputs.AddClipSource(source);
        break;
    }
    case OceanInputFamily::Albedo:
    {
        frame.AlbedoEnabled = true;
        OceanAlbedoSourceGPU source{};
        source.OriginExtent[0] = packet.CenterX;
        source.OriginExtent[1] = packet.CenterZ;
        source.OriginExtent[2] = packet.ExtentX;
        source.OriginExtent[3] = packet.ExtentZ;
        std::memcpy(source.Color, packet.Value, sizeof(source.Color));
        inputs.AddAlbedoSource(source);
        break;
    }
    case OceanInputFamily::DynamicWaves:
    {
        frame.DynWavesEnabled = true;
        OceanWaveImpulseGPU impulse{};
        impulse.CenterRadiusAmp[0] = packet.CenterX;
        impulse.CenterRadiusAmp[1] = packet.CenterZ;
        impulse.CenterRadiusAmp[2] = std::max(packet.ExtentX, packet.ExtentZ);
        impulse.CenterRadiusAmp[3] = packet.Value[0];
        inputs.AddDynamicWaveImpulse(impulse);
        break;
    }
    case OceanInputFamily::Foam:
    {
        OceanFoamInputGPU source{};
        source.OriginExtent[0] = packet.CenterX;
        source.OriginExtent[1] = packet.CenterZ;
        source.OriginExtent[2] = packet.ExtentX;
        source.OriginExtent[3] = packet.ExtentZ;
        source.AmountFeatherBlend[0] = packet.Value[0];
        source.AmountFeatherBlend[1] = packet.Feather;
        source.AmountFeatherBlend[2] = static_cast<float32>(packet.Blend);
        inputs.AddFoamInput(source);
        break;
    }
    default: break;
    }
}

void PublishTypedInputs(ECS::World& world, OceanRenderFeature& feature,
                        OceanExtractionFrame& frame)
{
    // Typed input components share one draw-packet contract. Collection is sorted
    // by priority then stable entity ID before any family is routed into its
    // cascade representation, so overlap behavior is deterministic.
    std::vector<OceanInputDrawPacket> typedInputPackets;
    GatherTypedInputPackets(world, typedInputPackets);

    OceanInputDrawRegistry::Sort(typedInputPackets);
    feature.GetShadows().SetInputs(typedInputPackets);
    feature.SetTypedInputPackets(typedInputPackets);
    frame.Inputs.Stats.TypedInputs = static_cast<uint32>(typedInputPackets.size());
    for (const OceanInputDrawPacket& packet : typedInputPackets)
        RouteTypedInputPacket(packet, frame);
}

void AppendSplineInput(const Components::OceanSplineInput& si,
                       const Components::SplineComponent& sc,
                       const Components::WorldTransform& wt,
                       SplineECS::SplineService* splineService,
                       std::vector<Spline::SplineFrame>& frames,
                       std::vector<SplineEmit>& splineInputs)
{
    if (!(si.ConfineSurface || si.Flow || si.Clip || si.Albedo || si.Depth))
        return;
    if (si.Width <= 0.0f)
        return;
    const Spline::SplineData* data = splineService->GetSplineData(
        SplineECS::SplineHandle(sc.SplineDataIndex, sc.SplineDataGeneration));
    if (!data || data->Points.size() < 2 || data->TotalArcLength <= 0.0f)
        return;
    const float sampleSpacing = std::max(si.MaxSegmentLength, 0.1f);
    const uint32 n = std::clamp<uint32>(
        static_cast<uint32>(
            std::min(std::ceil(data->TotalArcLength / sampleSpacing) + 1.0f, 4096.0f)),
        2u, 4096u);
    frames.clear();
    Spline::SampleUniform(*data, n, frames);
    if (frames.empty())
        return;
    SplineEmit e{};
    e.Width = si.Width;
    e.UnderwaterDepth = si.UnderwaterDepth;
    e.FlowSpeed = si.FlowSpeed;
    e.AlbedoCoverage = si.AlbedoCoverage;
    e.ClipFeather = std::max(si.ClipFeather, 0.0f);
    e.DepthMeters = si.DepthMeters;
    e.DepthFeather = si.DepthFeather;
    e.Color[0] = si.Color.r;
    e.Color[1] = si.Color.g;
    e.Color[2] = si.Color.b;
    e.ConfineSurface = si.ConfineSurface;
    e.UnderwaterVolume = si.UnderwaterVolume;
    e.Flow = si.Flow;
    e.Clip = si.Clip;
    e.Albedo = si.Albedo;
    e.Depth = si.Depth;
    e.ReverseFlow = si.ReverseFlow;
    const float32* m = wt.matrix; // column-major: local -> world
    e.Samples.reserve(frames.size());
    for (const Spline::SplineFrame& fr : frames)
    {
        SplineSample s{};
        s.X = m[0] * fr.Position.x + m[4] * fr.Position.y +
              m[8] * fr.Position.z + m[12];
        s.Y = m[1] * fr.Position.x + m[5] * fr.Position.y +
              m[9] * fr.Position.z + m[13];
        s.Z = m[2] * fr.Position.x + m[6] * fr.Position.y +
              m[10] * fr.Position.z + m[14];
        float tx = m[0] * fr.Forward.x + m[4] * fr.Forward.y + m[8] * fr.Forward.z;
        float tz = m[2] * fr.Forward.x + m[6] * fr.Forward.y + m[10] * fr.Forward.z;
        const float len = std::sqrt(tx * tx + tz * tz);
        if (len > 1e-4f)
        {
            tx /= len;
            tz /= len;
        }
        s.TX = tx;
        s.TZ = tz;
        e.Samples.push_back(s);
    }
    splineInputs.push_back(std::move(e));
}

OceanRibbonStyle MakeRibbonStyle(const SplineEmit& e, const OceanExtractionFrame& frame, uint32 id)
{
    OceanRibbonStyle style{};
    style.Width = e.Width;
    style.FlowSpeed = e.FlowSpeed * (e.ReverseFlow ? -1.0f : 1.0f);
    style.Depth = e.DepthMeters;
    style.DepthFeather = e.DepthFeather;
    style.Feather = e.ClipFeather > 0 ? e.ClipFeather : e.Width * 0.1f;
    style.UnderwaterDepth = e.UnderwaterDepth;
    style.DepthSaturation = OceanDepthBandSaturation(frame.Params);
    std::copy_n(e.Color, 3, style.Color);
    style.Color[3] = e.AlbedoCoverage;
    style.Flags = (e.Depth ? 1u : 0u) | (e.Flow && frame.FlowEnabled ? 2u : 0u) |
                  ((e.Clip || e.ConfineSurface) && frame.ClipEnabled ? 4u : 0u) |
                  (e.Albedo && frame.AlbedoEnabled ? 8u : 0u) | (e.ConfineSurface ? 16u : 0u) |
                  (e.ConfineSurface && e.UnderwaterVolume ? 32u : 0u);
    style.Id = id;
    return style;
}

void PublishSplineRibbons(ECS::World& world, OceanRenderFeature& feature,
                          OceanExtractionFrame& frame)
{
    // Tessellate continuous world-space ribbons for the field rasterizer.
    // These triangles do not consume the analytic point-source budgets.
    std::vector<SplineEmit> splineInputs;
    if (auto* splineService = SplineECS::SplineService::TryGet())
    {
        std::vector<Spline::SplineFrame> frames;
        world.Query<ECS::Read<Components::OceanSplineInput>,
                    ECS::Read<Components::SplineComponent>,
                    ECS::Read<Components::WorldTransform>>()
            .Each(
                [&](ECS::EntityHandle, const Components::OceanSplineInput& si,
                    const Components::SplineComponent& sc, const Components::WorldTransform& wt)
                { AppendSplineInput(si, sc, wt, splineService, frames, splineInputs); });
    }

    std::vector<OceanRibbonTriangleGPU> ribbonTriangles;
    uint32 ribbonId = 0;
    for (const SplineEmit& e : splineInputs)
    {
        if (e.ConfineSurface)
        {
            frame.ClipEnabled = true;
            frame.Params.DefaultClippingState = 1.0f;
            frame.WaterBodiesConstrainSurface = true;
        }
        const OceanRibbonStyle style = MakeRibbonStyle(e, frame, ribbonId++);
        std::vector<OceanRibbonPoint> points;
        for (const auto &v : e.Samples)
            points.push_back({v.X, v.Y, v.Z, v.TX, v.TZ});
        auto triangles = BuildOceanRibbon(points, style);
        ribbonTriangles.insert(ribbonTriangles.end(), triangles.begin(), triangles.end());
    }
    const bool ribbonsChanged = feature.GetSplineRaster().SetTriangles(std::move(ribbonTriangles));
    feature.GetSeabedDepth().SetSplineActive(feature.GetSplineRaster().HasField(0), ribbonsChanged);
    feature.GetFlow().SetSplineActive(feature.GetSplineRaster().HasField(1), ribbonsChanged);
    feature.GetClip().SetSplineActive(feature.GetSplineRaster().HasField(2), ribbonsChanged);
    {
        float clipBounds[4] = {};
        const bool hasClipBounds = feature.GetSplineRaster().ClipBoundsXZ(clipBounds[0], clipBounds[1],
                                                                          clipBounds[2], clipBounds[3]);
        feature.GetClip().SetSplineBounds(hasClipBounds, clipBounds[0], clipBounds[1], clipBounds[2],
                                          clipBounds[3]);
    }
    feature.GetAlbedo().SetSplineActive(feature.GetSplineRaster().HasField(3), ribbonsChanged);
    frame.SplineInputCount = static_cast<uint32>(splineInputs.size());
}

void PublishWaterBodies(OceanRenderFeature& feature, const OceanExtractionFrame& frame)
{
    feature.SetWaterBodies(frame.WaterBodyBounds, frame.WaterBodiesConstrainSurface);
    feature.SetWaterBodyStamps(frame.WaterBodyStamps);
    feature.SetWaterBodyPolygons(frame.WaterBodyPolygons);
}

void PublishSeabedDepth(OceanRenderFeature& feature, OceanExtractionFrame& frame)
{
    auto& savedDepthCacheSources = frame.Inputs.SavedDepthCaches;
    auto& seabeds = frame.Inputs.Seabeds;
    auto& depthContributors = frame.Inputs.DepthContributors;
    const OceanParamsGPU& params = frame.Params;
    if (feature.IsSeabedDepthReady())
    {
        if (!savedDepthCacheSources.empty())
            feature.GetSeabedDepth().LoadSavedDepthCaches(
                savedDepthCacheSources.data(), static_cast<uint32>(savedDepthCacheSources.size()));
        else
            feature.GetSeabedDepth().ClearSavedDepthCache();

        feature.GetSeabedDepth().SetSeabeds(
            seabeds.empty() ? nullptr : seabeds.data(), static_cast<uint32>(seabeds.size()),
            depthContributors.empty() ? nullptr : depthContributors.data(),
            static_cast<uint32>(depthContributors.size()),
            params.SeaLevel, OceanDepthBandSaturation(params));
    }
}

void AddFlowSource(const Components::OceanFlowSource& fs, const Components::WorldTransform& wt,
                   OceanInputRegistryFrame& inputs)
{
    if (fs.ExtentX <= 0.0f || fs.ExtentZ <= 0.0f)
        return;
    const float32* m = wt.matrix;
    OceanFlowSourceGPU g{};
    g.OriginExtent[0] = m[12]; // world X of the entity origin
    g.OriginExtent[1] = m[14]; // world Z of the entity origin
    g.OriginExtent[2] = fs.ExtentX;
    g.OriginExtent[3] = fs.ExtentZ;
    g.FlowVelocity[0] = fs.FlowX;
    g.FlowVelocity[1] = fs.FlowZ;
    inputs.AddFlowSource(g);
}

void AddFlowMapSource(const Components::OceanFlowMapSource& fs,
                      const Components::WorldTransform& wt, OceanInputRegistryFrame& inputs,
                      std::vector<GUID>& flowMapTextureGuids,
                      std::vector<OceanCpuTextureRG>& flowMapCpuTextures)
{
    if (fs.ExtentX <= 0.0f || fs.ExtentZ <= 0.0f)
        return;
    const GUID flowMapGuid = fs.FlowMap.ToGuid();
    if (flowMapGuid.IsNull())
        return;

    const float32* m = wt.matrix;
    OceanFlowMapSourceGPU g{};
    g.OriginExtent[0] = m[12]; // world X of the entity origin
    g.OriginExtent[1] = m[14]; // world Z of the entity origin
    g.OriginExtent[2] = fs.ExtentX;
    g.OriginExtent[3] = fs.ExtentZ;
    g.StrengthFeather[0] = fs.Strength;
    g.StrengthFeather[1] = std::max(fs.Feather, 0.0f);
    g.StrengthFeather[2] = fs.BiasX;
    g.StrengthFeather[3] = fs.BiasZ;
    if (inputs.AddFlowMapSource(g))
    {
        flowMapTextureGuids.push_back(flowMapGuid);
        flowMapCpuTextures.push_back(LoadOceanCpuTextureRG(flowMapGuid));
    }
}

void GatherFlowSources(ECS::World& world, OceanExtractionFrame& frame,
                       std::vector<GUID>& flowMapTextureGuids,
                       std::vector<OceanCpuTextureRG>& flowMapCpuTextures)
{
    if (!frame.FlowEnabled)
        return;

    OceanInputRegistryFrame& inputs = frame.Inputs;
    world.Query<ECS::Read<Components::OceanFlowSource>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle, const Components::OceanFlowSource& fs,
                  const Components::WorldTransform& wt) { AddFlowSource(fs, wt, inputs); });

    world.Query<ECS::Read<Components::OceanFlowMapSource>, ECS::Read<Components::WorldTransform>>()
        .Each(
            [&](ECS::EntityHandle, const Components::OceanFlowMapSource& fs,
                const Components::WorldTransform& wt)
            { AddFlowMapSource(fs, wt, inputs, flowMapTextureGuids, flowMapCpuTextures); });

    // Water bodies: optional body-wide current.
    for (const WaterBodyEmit& wb : frame.WaterBodies)
    {
        if (!wb.Flow)
            continue;
        OceanFlowSourceGPU g{};
        g.OriginExtent[0] = wb.X;
        g.OriginExtent[1] = wb.Z;
        g.OriginExtent[2] = wb.ExtentX;
        g.OriginExtent[3] = wb.ExtentZ;
        g.FlowVelocity[0] = wb.FlowX;
        g.FlowVelocity[1] = wb.FlowZ;
        inputs.AddFlowSource(g);
    }

    for (const PolygonBodyEmit& wb : frame.PolygonWaterBodies)
    {
        if (!wb.Flow)
            continue;
        inputs.AddFlowPolygon(wb.FlowPolygon);
    }
}

void PublishFlowSources(Engine::Renderer::RenderServices& renderServices,
                        OceanRenderFeature& feature, OceanExtractionFrame& frame,
                        const std::vector<GUID>& flowMapTextureGuids,
                        std::vector<OceanCpuTextureRG>& flowMapCpuTextures)
{
    if (!feature.IsFlowReady())
        return;

    auto& flowSources = frame.Inputs.FlowSources;
    auto& flowPolygons = frame.Inputs.FlowPolygons;
    auto& flowMapSources = frame.Inputs.FlowMapSources;
    feature.GetFlow().SetSources(flowSources.empty() ? nullptr : flowSources.data(),
                                 static_cast<uint32>(flowSources.size()));
    feature.GetFlow().SetPolygonSources(flowPolygons.empty() ? nullptr : flowPolygons.data(),
                                        static_cast<uint32>(flowPolygons.size()));
    std::vector<Rendering::TextureHandle> flowMapTextures;
    flowMapTextures.reserve(flowMapTextureGuids.size());
    for (const GUID& guid : flowMapTextureGuids)
        flowMapTextures.push_back(renderServices.Textures().GetOrUpload(guid));
    feature.GetFlow().SetFlowMapSources(
        flowMapSources.empty() ? nullptr : flowMapSources.data(),
        flowMapTextures.empty() ? nullptr : flowMapTextures.data(),
        static_cast<uint32>(std::min(flowMapSources.size(), flowMapTextures.size())),
        flowMapCpuTextures.empty() ? nullptr : flowMapCpuTextures.data());
}

void ExtractFlowSources(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                        OceanRenderFeature& feature, OceanExtractionFrame& frame)
{
    // Tagged flow sources → analytic flow-source rectangles for the flow bake.
    // The entity origin XZ supplies the footprint center. Capped at
    // kMaxOceanFlowSources. Flow-map sources add sampled vector textures in
    // rectangular footprints. Gathered only when the ocean's Flow toggle is on.
    std::vector<GUID> flowMapTextureGuids;
    std::vector<OceanCpuTextureRG> flowMapCpuTextures;
    GatherFlowSources(world, frame, flowMapTextureGuids, flowMapCpuTextures);
    PublishFlowSources(renderServices, feature, frame, flowMapTextureGuids, flowMapCpuTextures);
}

void AddWaveMaskTextureSource(
    const Components::OceanWaveMaskTextureSource& ms, const Components::WorldTransform& wt,
    OceanInputRegistryFrame& inputs, std::vector<GUID>& waveMaskTextureGuids,
    std::vector<OceanRenderFeature::WaterBodyWaveTextureBox>& waveWeightTextureBoxes)
{
    if (ms.ExtentX <= 0.0f || ms.ExtentZ <= 0.0f)
        return;
    const GUID maskGuid = ms.WaveMaskMap.ToGuid();
    if (maskGuid.IsNull())
        return;

    const float32* m = wt.matrix;
    OceanWaveMaskTextureSourceGPU g{};
    g.OriginExtent[0] = m[12]; // world X of the entity origin
    g.OriginExtent[1] = m[14]; // world Z of the entity origin
    g.OriginExtent[2] = ms.ExtentX;
    g.OriginExtent[3] = ms.ExtentZ;
    g.ScaleBias[0] = ms.WeightScale;
    g.ScaleBias[1] = ms.ChopScale;
    g.ScaleBias[2] = ms.WeightBias;
    g.ScaleBias[3] = ms.ChopBias;
    g.FeatherCoverage[0] = std::max(ms.Feather, 0.0f);
    g.FeatherCoverage[1] = std::clamp(ms.Coverage, 0.0f, 1.0f);
    if (!inputs.AddWaveMaskTextureSource(g))
        return;

    waveMaskTextureGuids.push_back(maskGuid);
    OceanRenderFeature::WaterBodyWaveTextureBox box{};
    box.CenterX = g.OriginExtent[0];
    box.CenterZ = g.OriginExtent[1];
    box.HalfX = g.OriginExtent[2];
    box.HalfZ = g.OriginExtent[3];
    box.WeightScale = g.ScaleBias[0];
    box.ChopScale = g.ScaleBias[1];
    box.WeightBias = g.ScaleBias[2];
    box.ChopBias = g.ScaleBias[3];
    box.Coverage = g.FeatherCoverage[1];
    box.Feather = g.FeatherCoverage[0];
    box.Texture = LoadOceanCpuTextureRG(maskGuid);
    waveWeightTextureBoxes.push_back(box);
}

void AddWaterBodyWaveOverride(const WaterBodyEmit& wb, OceanInputRegistryFrame& inputs,
                              std::vector<OceanRenderFeature::WaterBodyWaveBox>& waveWeightBoxes)
{
    if (!wb.WaveOverride)
        return;
    OceanWaveMaskSourceGPU g{};
    g.OriginExtent[0] = wb.X;
    g.OriginExtent[1] = wb.Z;
    g.OriginExtent[2] = wb.ExtentX;
    g.OriginExtent[3] = wb.ExtentZ;
    g.WaveChopFeather[0] = wb.WaveWeight;
    g.WaveChopFeather[1] = wb.WaveChop;
    g.WaveChopFeather[2] = wb.WaveFeather;
    g.WaveChopFeather[3] = static_cast<float32>(wb.LocalWaveCount);
    std::memcpy(g.LocalWaves, wb.LocalWaves, sizeof(g.LocalWaves));
    g.LocalFFTBlend[0] = wb.LocalFFTBlend;
    g.LocalFFTBlend[1] = static_cast<float32>(wb.LocalFFTStream);
    if (!inputs.AddWaveMaskSource(g))
        return;

    OceanRenderFeature::WaterBodyWaveBox box{};
    box.CenterX = wb.X;
    box.CenterZ = wb.Z;
    box.HalfX = wb.ExtentX;
    box.HalfZ = wb.ExtentZ;
    box.Weight = wb.WaveWeight;
    box.Chop = wb.WaveChop;
    box.Feather = wb.WaveFeather;
    box.LocalWaveCount = wb.LocalWaveCount;
    box.LocalAmplitude = wb.LocalWaves[0][0];
    box.LocalWavelength = wb.LocalWaves[0][1];
    box.LocalDirX = wb.LocalWaves[0][2];
    box.LocalDirZ = wb.LocalWaves[0][3];
    box.LocalFFTBlend = wb.LocalFFTBlend;
    box.LocalFFTStream = wb.LocalFFTStream;
    for (uint32 i = 1u; i < std::min(wb.LocalWaveCount, kMaxOceanWaveMaskLocalWaves); ++i)
        std::memcpy(box.LocalWaveExtra[i - 1u], wb.LocalWaves[i], sizeof(box.LocalWaveExtra[0]));
    waveWeightBoxes.push_back(box);
}

void AddPolygonWaveOverride(
    const PolygonBodyEmit& wb, OceanInputRegistryFrame& inputs,
    std::vector<OceanRenderFeature::WaterBodyWavePolygon>& waveWeightPolygons)
{
    if (!wb.WaveOverride)
        return;
    if (!inputs.AddWaveMaskPolygon(wb.WavePolygon))
        return;

    OceanRenderFeature::WaterBodyWavePolygon poly{};
    poly.PointCount = wb.Query.PointCount;
    poly.Weight = wb.WavePolygon.WaveChop[0];
    poly.Chop = wb.WavePolygon.WaveChop[1];
    poly.Feather = wb.WavePolygon.Meta[1];
    poly.LocalWaveCount = static_cast<uint32>(
        std::clamp(wb.WavePolygon.Meta[2], 0.0f,
                   static_cast<float32>(kMaxOceanWaveMaskLocalWaves)));
    poly.LocalAmplitude = wb.WavePolygon.LocalWaves[0][0];
    poly.LocalWavelength = wb.WavePolygon.LocalWaves[0][1];
    poly.LocalDirX = wb.WavePolygon.LocalWaves[0][2];
    poly.LocalDirZ = wb.WavePolygon.LocalWaves[0][3];
    poly.LocalFFTBlend = wb.WavePolygon.WaveChop[2];
    poly.LocalFFTStream = static_cast<uint32>(
        std::clamp(wb.WavePolygon.WaveChop[3], 0.0f,
                   static_cast<float32>(kMaxOceanLocalFFTStreams - 1u)));
    for (uint32 i = 1u; i < std::min(poly.LocalWaveCount, kMaxOceanWaveMaskLocalWaves); ++i)
        std::memcpy(poly.LocalWaveExtra[i - 1u], wb.WavePolygon.LocalWaves[i],
                    sizeof(poly.LocalWaveExtra[0]));
    const uint32 count = std::min(poly.PointCount, kMaxOceanClipPolygonPoints);
    for (uint32 i = 0; i < count; ++i)
    {
        poly.X[i] = wb.Query.X[i];
        poly.Z[i] = wb.Query.Z[i];
    }
    waveWeightPolygons.push_back(poly);
}

void PublishWaveMaskSources(Engine::Renderer::RenderServices& renderServices,
                            OceanRenderFeature& feature, OceanExtractionFrame& frame,
                            const std::vector<GUID>& waveMaskTextureGuids)
{
    if (!feature.IsWaveMaskReady())
        return;

    auto& waveMaskSources = frame.Inputs.WaveMaskSources;
    auto& waveMaskPolygons = frame.Inputs.WaveMaskPolygons;
    auto& waveMaskTextureSources = frame.Inputs.WaveMaskTextureSources;
    feature.GetWaveMask().SetTime(frame.OceanTime, frame.Params.WaveOriginOffsetX,
                                  frame.Params.WaveOriginOffsetZ);
    feature.GetWaveMask().SetSources(waveMaskSources.empty() ? nullptr : waveMaskSources.data(),
                                     static_cast<uint32>(waveMaskSources.size()));
    feature.GetWaveMask().SetPolygonSources(waveMaskPolygons.empty() ? nullptr : waveMaskPolygons.data(),
                                            static_cast<uint32>(waveMaskPolygons.size()));
    std::vector<Rendering::TextureHandle> waveMaskTextures;
    waveMaskTextures.reserve(waveMaskTextureGuids.size());
    for (const GUID& guid : waveMaskTextureGuids)
        waveMaskTextures.push_back(renderServices.Textures().GetOrUpload(guid));
    feature.GetWaveMask().SetTextureSources(
        waveMaskTextureSources.empty() ? nullptr : waveMaskTextureSources.data(),
        waveMaskTextures.empty() ? nullptr : waveMaskTextures.data(),
        static_cast<uint32>(std::min(waveMaskTextureSources.size(), waveMaskTextures.size())));
}

void ExtractWaveMasks(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                      OceanRenderFeature& feature, OceanExtractionFrame& frame)
{
    OceanInputRegistryFrame& inputs = frame.Inputs;
    std::vector<GUID> waveMaskTextureGuids;
    std::vector<OceanRenderFeature::WaterBodyWaveBox> waveWeightBoxes;
    std::vector<OceanRenderFeature::WaterBodyWavePolygon> waveWeightPolygons;
    std::vector<OceanRenderFeature::WaterBodyWaveTextureBox> waveWeightTextureBoxes;

    world.Query<ECS::Read<Components::OceanWaveMaskTextureSource>,
                ECS::Read<Components::WorldTransform>>()
        .Each(
            [&](ECS::EntityHandle, const Components::OceanWaveMaskTextureSource& ms,
                const Components::WorldTransform& wt)
            {
                AddWaveMaskTextureSource(ms, wt, inputs, waveMaskTextureGuids,
                                         waveWeightTextureBoxes);
            });

    for (const WaterBodyEmit& wb : frame.WaterBodies)
        AddWaterBodyWaveOverride(wb, inputs, waveWeightBoxes);
    for (const PolygonBodyEmit& wb : frame.PolygonWaterBodies)
        AddPolygonWaveOverride(wb, inputs, waveWeightPolygons);
    feature.SetWaterBodyWaveOverrides(waveWeightBoxes, waveWeightPolygons,
                                      waveWeightTextureBoxes);
    PublishWaveMaskSources(renderServices, feature, frame, waveMaskTextureGuids);
}

void AddClipSource(const Components::OceanClipSource& cs, const Components::WorldTransform& wt,
                   OceanInputRegistryFrame& inputs)
{
    if (cs.ExtentX <= 0.0f || cs.ExtentZ <= 0.0f)
        return;
    const float32* m = wt.matrix;
    OceanClipSourceGPU g{};
    g.OriginExtent[0] = m[12]; // world X of the entity origin
    g.OriginExtent[1] = m[14]; // world Z of the entity origin
    g.OriginExtent[2] = cs.ExtentX;
    g.OriginExtent[3] = cs.ExtentZ;
    g.ClipState[0] = cs.ClipState;
    g.ClipState[1] = std::max(cs.Feather, 0.0f);
    inputs.AddClipSource(g);
}

void ExtractClipSources(ECS::World& world, OceanRenderFeature& feature,
                        OceanExtractionFrame& frame)
{
    // Tagged clip sources → analytic clip rectangles for the clip bake. The entity
    // origin XZ supplies the footprint center. Capped at kMaxOceanClipSources.
    // Gathered only when the ocean's ClipSurface toggle is on.
    OceanInputRegistryFrame& inputs = frame.Inputs;
    auto& clipSources = inputs.ClipSources;
    auto& clipPolygons = inputs.ClipPolygons;
    if (frame.ClipEnabled)
    {
        for (const PolygonBodyEmit& wb : frame.PolygonWaterBodies)
        {
            if (!wb.ConfineSurface)
                continue;
            inputs.AddClipPolygon(wb.Clip);
        }

        // Water bodies: with DefaultClippingState = 1, these restore water inside
        // each bounded body footprint.
        for (const WaterBodyEmit& wb : frame.WaterBodies)
        {
            if (!wb.ConfineSurface)
                continue;
            OceanClipSourceGPU g{};
            g.OriginExtent[0] = wb.X;
            g.OriginExtent[1] = wb.Z;
            g.OriginExtent[2] = wb.ExtentX;
            g.OriginExtent[3] = wb.ExtentZ;
            g.ClipState[0] = 0.0f;
            g.ClipState[1] = wb.ClipFeather;
            inputs.AddClipSource(g);
        }

        world.Query<ECS::Read<Components::OceanClipSource>, ECS::Read<Components::WorldTransform>>()
            .Each([&](ECS::EntityHandle, const Components::OceanClipSource& cs,
                      const Components::WorldTransform& wt) { AddClipSource(cs, wt, inputs); });
    }
    if (feature.IsClipReady())
    {
        feature.GetClip().SetDefaultClippingState(frame.Params.DefaultClippingState);
        feature.GetClip().SetSources(clipSources.empty() ? nullptr : clipSources.data(),
                                     static_cast<uint32>(clipSources.size()));
        feature.GetClip().SetPolygonSources(clipPolygons.empty() ? nullptr : clipPolygons.data(),
                                            static_cast<uint32>(clipPolygons.size()));
    }
}

void AddAlbedoSource(const Components::OceanAlbedoSource& as, const Components::WorldTransform& wt,
                     OceanInputRegistryFrame& inputs)
{
    if (as.ExtentX <= 0.0f || as.ExtentZ <= 0.0f)
        return;
    const float32* m = wt.matrix;
    OceanAlbedoSourceGPU g{};
    g.OriginExtent[0] = m[12]; // world X of the entity origin
    g.OriginExtent[1] = m[14]; // world Z of the entity origin
    g.OriginExtent[2] = as.ExtentX;
    g.OriginExtent[3] = as.ExtentZ;
    g.Color[0] = as.Color.r;
    g.Color[1] = as.Color.g;
    g.Color[2] = as.Color.b;
    g.Color[3] = as.Coverage; // alpha = coverage (the colour's own alpha is ignored)
    inputs.AddAlbedoSource(g);
}

void ExtractAlbedoSources(ECS::World& world, OceanRenderFeature& feature,
                          OceanExtractionFrame& frame)
{
    // Tagged albedo sources → analytic albedo rectangles for the albedo bake. The
    // entity origin XZ supplies the footprint center. Capped at
    // kMaxOceanAlbedoSources. Gathered only when the ocean's Albedo toggle is on.
    OceanInputRegistryFrame& inputs = frame.Inputs;
    auto& albedoSources = inputs.AlbedoSources;
    if (frame.AlbedoEnabled)
    {
        world.Query<ECS::Read<Components::OceanAlbedoSource>, ECS::Read<Components::WorldTransform>>()
            .Each([&](ECS::EntityHandle, const Components::OceanAlbedoSource& as,
                      const Components::WorldTransform& wt) { AddAlbedoSource(as, wt, inputs); });
    }
    if (feature.IsAlbedoReady())
        feature.GetAlbedo().SetSources(albedoSources.empty() ? nullptr : albedoSources.data(),
                                       static_cast<uint32>(albedoSources.size()));
}

void ExtractUnderwaterVolumes(ECS::World& world, OceanRenderFeature& feature,
                              OceanExtractionFrame& frame)
{
    // Tagged underwater volumes (OceanUnderwaterVolume): axis-aligned boxes that
    // confine the underwater overlay to a bounded water body (Crest volume / fly-
    // through mode). With any volume present the render node's submersion gate
    // switches from the infinite SeaLevel plane to "camera inside a volume". Empty
    // when none are tagged, so the node falls back to the global sea-level test.
    std::vector<OceanRenderFeature::UnderwaterVolumeBox> underwaterVolumes;
    std::vector<OceanRenderFeature::UnderwaterVolumePolygon> underwaterVolumePolygons;
    std::vector<OceanRenderFeature::UnderwaterVolumeBox> underwaterExclusionVolumes;
    std::vector<OceanRenderFeature::UnderwaterVolumeBox> underwaterPortalOccluders;
    for (const WaterBodyEmit& wb : frame.WaterBodies)
    {
        if (!wb.UnderwaterVolume)
            continue;
        OceanRenderFeature::UnderwaterVolumeBox b{};
        b.CenterX = wb.X;
        b.CenterY = wb.Y - wb.UnderwaterDepth * 0.5f;
        b.CenterZ = wb.Z;
        b.HalfX = wb.ExtentX;
        b.HalfY = wb.UnderwaterDepth * 0.5f;
        b.HalfZ = wb.ExtentZ;
        underwaterVolumes.push_back(b);
    }
    for (const PolygonBodyEmit& wb : frame.PolygonWaterBodies)
    {
        if (!wb.UnderwaterVolume)
            continue;
        OceanRenderFeature::UnderwaterVolumePolygon p{};
        p.SurfaceY = wb.CenterY;
        p.Depth = wb.UnderwaterDepth;
        p.PointCount = wb.Query.PointCount;
        const uint32 count = std::min(p.PointCount, kMaxOceanClipPolygonPoints);
        for (uint32 i = 0; i < count; ++i)
        {
            p.X[i] = wb.Query.X[i];
            p.Z[i] = wb.Query.Z[i];
        }
        underwaterVolumePolygons.push_back(p);
    }

    world.Query<ECS::Read<Components::OceanUnderwaterVolume>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle, const Components::OceanUnderwaterVolume& uv,
                  const Components::WorldTransform& wt)
              { AppendUnderwaterBox(uv, wt, underwaterVolumes); });
    world.Query<ECS::Read<Components::OceanUnderwaterExclusionVolume>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle, const Components::OceanUnderwaterExclusionVolume& uv,
                  const Components::WorldTransform& wt)
              { AppendUnderwaterBox(uv, wt, underwaterExclusionVolumes); });
    world.Query<ECS::Read<Components::OceanUnderwaterPortalOccluder>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle, const Components::OceanUnderwaterPortalOccluder& uv,
                  const Components::WorldTransform& wt)
              { AppendUnderwaterBox(uv, wt, underwaterPortalOccluders); });
    feature.SetUnderwaterVolumes(underwaterVolumes);
    feature.SetUnderwaterVolumePolygons(underwaterVolumePolygons);
    feature.SetUnderwaterExclusionVolumes(underwaterExclusionVolumes);
    feature.SetUnderwaterPortalOccluders(underwaterPortalOccluders);
    frame.UnderwaterVolumeCount = static_cast<uint32>(underwaterVolumes.size());
    frame.UnderwaterVolumePolygonCount = static_cast<uint32>(underwaterVolumePolygons.size());
    frame.UnderwaterExclusionVolumeCount = static_cast<uint32>(underwaterExclusionVolumes.size());
}

void AddWaveImpulse(const Components::OceanWaveImpulse& imp, const Components::WorldTransform& wt,
                    OceanInputRegistryFrame& inputs)
{
    if (imp.Radius <= 0.0f)
        return;
    const float32* m = wt.matrix;
    OceanWaveImpulseGPU g{};
    g.CenterRadiusAmp[0] = m[12]; // world X of the entity origin
    g.CenterRadiusAmp[1] = m[14]; // world Z of the entity origin
    g.CenterRadiusAmp[2] = imp.Radius;
    g.CenterRadiusAmp[3] = imp.Amplitude;
    inputs.AddDynamicWaveImpulse(g);
}

float32 ComputeWakeAmplitude(const Components::OceanWaterInteraction& inter,
                             const OceanSurfaceSample& surfaceSample, float32 velocityX,
                             float32 velocityY, float32 velocityZ)
{
    const float32 horizontalSpeed =
        std::sqrt(velocityX * velocityX + velocityZ * velocityZ);
    const float32 verticalSpeed = velocityY;
    float32 amplitude = 0.0f;
    if (horizontalSpeed >= inter.MinSpeed)
        amplitude -= horizontalSpeed * inter.Strength;
    if (std::abs(verticalSpeed) >= inter.MinVerticalSpeed)
        amplitude += verticalSpeed * inter.VerticalStrength;
    if (surfaceSample.Valid && inter.LargeWaveBoost > 0.0f)
    {
        const float32 ambientWave = std::abs(surfaceSample.DisplacementWS[1]);
        amplitude *= 1.0f + inter.LargeWaveBoost *
            std::min(ambientWave / std::max(inter.Radius, 0.1f), 2.0f);
    }
    const float32 maxAmplitude = std::max(inter.MaxAmplitude, 0.0f);
    amplitude = std::clamp(amplitude, -maxAmplitude, maxAmplitude);
    return amplitude;
}

void EmitWakeImpulses(const Components::OceanWaterInteraction& inter, float32 px, float32 pz,
                      float32 dx, float32 dz, float32 velocityX, float32 velocityZ,
                      float32 amplitude, OceanInputRegistryFrame& inputs)
{
    const uint32 pathSteps = std::clamp(inter.DebugSubsteps, 1u, 4u);
    const uint32 spheres = std::clamp(inter.NestedSphereCount, 1u, 3u);
    const float32 radiusScale = std::clamp(inter.NestedRadiusScale, 0.1f, 1.0f);
    const float32 weightScale = std::clamp(inter.NestedWeight, 0.0f, 1.0f);
    for (uint32 pathStep = 0u; pathStep < pathSteps; ++pathStep)
    {
        const float32 t = static_cast<float32>(pathStep + 1u) /
                          static_cast<float32>(pathSteps);
        const float32 centerX = px + dx * t + velocityX * inter.VelocityLead;
        const float32 centerZ = pz + dz * t + velocityZ * inter.VelocityLead;
        float32 radius = inter.Radius;
        float32 weight = 1.0f;
        for (uint32 sphere = 0u; sphere < spheres; ++sphere)
        {
            OceanWaveImpulseGPU g{};
            g.CenterRadiusAmp[0] = centerX;
            g.CenterRadiusAmp[1] = centerZ;
            g.CenterRadiusAmp[2] = radius;
            g.CenterRadiusAmp[3] = amplitude * weight /
                                   static_cast<float32>(pathSteps);
            if (!inputs.AddDynamicWaveImpulse(g))
                return;
            radius *= radiusScale;
            weight *= weightScale;
        }
    }
}

void InjectInteractionWake(uint32 entityId, const Components::OceanWaterInteraction& inter,
                           const Components::WorldTransform& wt, float32 deltaTime,
                           OceanRenderFeature& feature,
                           std::unordered_map<uint32, std::array<float32, 3>>& prevInteractionXYZ,
                           OceanInputRegistryFrame& inputs)
{
    const float32* m = wt.matrix;
    const float32 wx = m[12];
    const float32 wy = m[13];
    const float32 wz = m[14];
    // Read the previous position BEFORE updating the map (the
    // update can rehash and invalidate the iterator).
    auto prevIt = prevInteractionXYZ.find(entityId);
    const bool havePrev = prevIt != prevInteractionXYZ.end();
    const float32 px = havePrev ? prevIt->second[0] : wx;
    const float32 py = havePrev ? prevIt->second[1] : wy;
    const float32 pz = havePrev ? prevIt->second[2] : wz;
    prevInteractionXYZ[entityId] = {wx, wy, wz};

    if (inter.Radius <= 0.0f || !havePrev)
        return;
    const float32 dx = wx - px;
    const float32 dy = wy - py;
    const float32 dz = wz - pz;
    const float32 travelDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (inter.TeleportDistance > 0.0f &&
        travelDistance > inter.TeleportDistance)
        return;

    float32 velocityX = dx / deltaTime;
    float32 velocityY = dy / deltaTime;
    float32 velocityZ = dz / deltaTime;
    const OceanSurfaceSample surfaceSample = feature.SampleSurfaceForRendering(wx, wz);
    if (inter.FlowRelativeVelocity)
    {
        const OceanCurrentSample current = feature.SampleFlow(wx, wz);
        if (current.Valid)
        {
            velocityX -= current.FlowX;
            velocityZ -= current.FlowZ;
        }
    }
    if (surfaceSample.Valid && inter.WaveMotionCompensation > 0.0f)
    {
        const float32 compensation =
            std::clamp(inter.WaveMotionCompensation, 0.0f, 1.0f);
        velocityX -= surfaceSample.VelocityWS[0] * compensation;
        velocityY -= surfaceSample.VelocityWS[1] * compensation;
        velocityZ -= surfaceSample.VelocityWS[2] * compensation;
    }
    const float32 speed =
        std::sqrt(velocityX * velocityX + velocityY * velocityY +
                  velocityZ * velocityZ);
    const float32 speedClamp = std::max(inter.SpeedClamp, 0.0f);
    if (speedClamp > 0.0f && speed > speedClamp)
    {
        const float32 scale = speedClamp / std::max(speed, 1e-5f);
        velocityX *= scale;
        velocityY *= scale;
        velocityZ *= scale;
    }
    const float32 amplitude =
        ComputeWakeAmplitude(inter, surfaceSample, velocityX, velocityY, velocityZ);
    if (std::abs(amplitude) <= 1e-5f)
        return;
    EmitWakeImpulses(inter, px, pz, dx, dz, velocityX, velocityZ, amplitude, inputs);
}

void PublishSurfaceTogglesAndTextures(Engine::Renderer::RenderServices& renderServices,
                                      OceanRenderFeature& feature,
                                      const OceanExtractionFrame& frame)
{
    // Store the authored toggles on the feature so the contributor can gate
    // binding + the per-draw availability flags.
    feature.SetFlowEnabled(frame.FlowEnabled);
    feature.SetDynWavesEnabled(frame.DynWavesEnabled);
    feature.SetClipEnabled(frame.ClipEnabled);
    feature.SetAlbedoEnabled(frame.AlbedoEnabled);

    // Resolve optional texture overrides (GUID -> GPU, cached by RenderServices).
    // Classify normal/foam data before upload. Empty slots clear the handles so
    // the shader can use its procedural fallbacks.
    renderServices.Textures().DeclareTextureClassification(
        frame.NormalTextureGuid, TextureColorSpace::Linear, TextureCookUsage::Normal);
    renderServices.Textures().DeclareTextureClassification(
        frame.FoamTextureGuid, TextureColorSpace::Linear, TextureCookUsage::Mask);
    feature.SetUserNormalTexture(frame.NormalTextureGuid.IsNull()
                                     ? Rendering::TextureHandle{}
                                     : renderServices.Textures().GetOrUpload(frame.NormalTextureGuid));
    feature.SetUserFoamTexture(frame.FoamTextureGuid.IsNull()
                                   ? Rendering::TextureHandle{}
                                   : renderServices.Textures().GetOrUpload(frame.FoamTextureGuid));
    feature.SetUserCausticsTexture(frame.CausticsTextureGuid.IsNull()
                                       ? Rendering::TextureHandle{}
                                       : renderServices.Textures().GetOrUpload(frame.CausticsTextureGuid));
}

void PublishCascadeConfig(OceanRenderFeature& feature, OceanExtractionFrame& frame)
{
    // Cascade LOD-count limit from the renderer (clamps the surface's snapped-
    // cascade sampling down, trimming fill). Clamped to the engine ceiling; 0 =
    // no limit (use each sim's full layer count). Only set when a renderer exists.
    if (frame.HaveRenderer)
    {
        uint32 lodLimit = frame.Renderer.LodCount;
        if (lodLimit > kMaxOceanLodCascades)
            lodLimit = kMaxOceanLodCascades;
        // MaxScale caps the coarsest cascade: with LOD 0 = MinScale, layer L spans
        // MinScale*2^L, so trim the layer count to keep the coarsest <= MaxScale.
        const float minScale = (frame.Renderer.MinScale > 0.0f) ? frame.Renderer.MinScale : 64.0f;
        if (frame.Renderer.MaxScale > minScale)
        {
            uint32 lodFromMax = 1;
            while (lodFromMax < kMaxOceanLodCascades &&
                   minScale * static_cast<float>(1u << lodFromMax) <= frame.Renderer.MaxScale)
                ++lodFromMax;
            if (lodFromMax < lodLimit)
                lodLimit = lodFromMax;
        }
        feature.SetLodCountLimit(lodLimit);
        // Cascade scale (MinScale -> finest cascade extent) + texel resolution
        // (LodDataResolution) + grid density. Applied once per frame by
        // OceanRenderFeature::ApplyCascadeConfig.
        const uint32 geometryGridSize = ResolveOceanGeometryGridSize(
            frame.Renderer.GeometryUpSampleFactor, frame.Renderer.GeometryDownSampleFactor);
        frame.Params.GeometryGridSize = static_cast<float32>(geometryGridSize);
        feature.SetCascadeBaseScale(frame.Renderer.MinScale);
        feature.SetCascadeResolution(frame.Renderer.LodDataResolution);
        feature.SetGeometryGridSize(geometryGridSize);
        feature.SetCombineEnabled(frame.Renderer.CombineDisplacementCascade);
        OceanRasterDepthCaptureSettings rasterSettings{};
        rasterSettings.Enabled = frame.Renderer.RasterDepthCapture;
        rasterSettings.Resolution = frame.Renderer.RasterDepthCaptureResolution;
        rasterSettings.CaptureLayers = frame.Renderer.RasterDepthCaptureLayers;
        rasterSettings.SizeX = frame.Renderer.RasterDepthCaptureSizeX;
        rasterSettings.SizeZ = frame.Renderer.RasterDepthCaptureSizeZ;
        rasterSettings.TopPadding = frame.Renderer.RasterDepthCaptureTopPadding;
        rasterSettings.DeepWaterDepth = frame.Renderer.RasterDepthCaptureDeepWaterDepth;
        feature.SetRasterDepthCaptureSettings(rasterSettings);
    }
    else
    {
        feature.SetLodCountLimit(0);
        feature.SetCascadeBaseScale(0.0f);
        feature.SetCascadeResolution(0u);
        frame.Params.GeometryGridSize = static_cast<float32>(kDefaultOceanGeometryGridSize);
        feature.SetGeometryGridSize(0u);
        feature.SetCombineEnabled(false);
        feature.SetRasterDepthCaptureSettings({});
    }
}

void ConfigureFoamSimulation(OceanRenderFeature& feature, const OceanExtractionFrame& frame)
{
    // Foam sim inputs: the FFT displacement (Jacobian source) + foam controls
    // + this tick's dt. The node flips the ping-pong + snaps to the camera at
    // schedule time; this only supplies the per-frame scalars.
    if (!feature.IsFoamReady())
        return;

    const OceanParamsGPU& params = frame.Params;
    const OceanSettingsAsset* foamSettings = frame.FoamSettings;
    if (foamSettings)
        feature.GetFoamSim().ConfigureSimulation(
            foamSettings->Foam.SimulationFrequency,
            foamSettings->Foam.MaximumSubsteps);
    else
        feature.GetFoamSim().ClearSimulationConfiguration();
    feature.GetFoamSim().SetAuthoredSources(
        frame.Inputs.FoamInputs.empty() ? nullptr : frame.Inputs.FoamInputs.data(),
        static_cast<uint32>(frame.Inputs.FoamInputs.size()));
    const float foamWindSpeed = frame.WindSpeed * 0.08f;
    const float32 foamFadeRate = foamSettings
                                     ? foamSettings->Foam.FadeRate
                                     : params.FoamFadeRate;
    const float32 foamCoverage = foamSettings
                                     ? foamSettings->Foam.WaveCoverage
                                     : params.WaveFoamCoverage;
    const float32 foamStrength = foamSettings
                                     ? foamSettings->Foam.WaveStrength
                                     : params.WaveFoamStrength;
    feature.GetFoamSim().SetFrameInputs(
        feature.GetDisplacementTexture(), feature.GetDisplacementSampler(),
        params.FFTCascadeCount, foamFadeRate, foamCoverage,
        foamStrength, frame.OceanDeltaTime,
        params.Weight, params.MaxHorizontalDisplacement,
        params.MaxVerticalDisplacement, params.RespectShallowAttenuation,
        params.SubSurfaceDepthMax,
        std::cos(frame.WindRadians) * foamWindSpeed,
        std::sin(frame.WindRadians) * foamWindSpeed,
        params.WaveOriginOffsetX, params.WaveOriginOffsetZ);

    // Shoreline foam: feed the seabed depth cascade (the foam cascade's
    // resolution, LOD count and base scale; the foam sim reads it with its
    // own write layout on the frames it steps). Any active depth content
    // may drive it: saved caches, analytic seabeds, spline bands, or
    // dynamic contributors.
    const bool haveSeabed =
        feature.IsSeabedDepthReady() && feature.GetSeabedDepth().HasSeabeds();
    feature.GetFoamSim().SetShorelineInputs(
        feature.GetSeabedDepthTexture(), feature.GetSeabedDepthSampler(),
        params.ShorelineFoamMaxDepth,
        foamSettings ? foamSettings->Foam.ShorelineStrength
                     : params.ShorelineFoamStrength,
        haveSeabed);

    // Flow advection: feed the flow cascade (the foam cascade's resolution,
    // LOD count and base scale; read with the foam write layout). Gated
    // on the Flow toggle + a flow source being present this frame, so the
    // foam advects only where the user placed a current.
    const bool haveFlow =
        feature.IsFlowReady() && frame.FlowEnabled && feature.GetFlow().HasSources();
    feature.GetFoamSim().SetFlowInputs(feature.GetFlowTexture(),
                                       feature.GetFlowSampler(),
                                       foamSettings
                                           ? foamSettings->Foam.FlowAdvection
                                           : 1.0f,
                                       haveFlow);
}

void ConfigureDynamicWaveSimulation(OceanRenderFeature& feature, OceanExtractionFrame& frame)
{
    // Dynamic-wave sim inputs: this frame's impulse list + the sim scalars +
    // dt. The node flips the ping-pong + snaps to the camera at schedule time;
    // this only supplies the per-frame data. Fed whenever the sim is ready and
    // the toggle is on (existing ripples keep propagating + decaying even on a
    // frame with no new impulse).
    if (!feature.IsDynWavesReady() || !frame.DynWavesEnabled)
        return;

    OceanParamsGPU& params = frame.Params;
    auto& impulses = frame.Inputs.DynamicWaveImpulses;
    constexpr float kDynWaveSpeed = 4.0f; // authored speed knob (CFL-clamped per LOD)
    constexpr float kDynDamping = 0.2f;   // per-second velocity damping
    const bool haveSeabed =
        feature.IsSeabedDepthReady() && feature.GetSeabedDepth().HasSeabeds();
    feature.GetDynWaves().SetShallowWaterInputs(
        feature.GetSeabedDepthTexture(), feature.GetSeabedDepthSampler(), haveSeabed);
    if (frame.DynamicSettings)
    {
        const auto& settings = frame.DynamicSettings->DynamicWaves;
        feature.GetDynWaves().ConfigureSimulation(
            settings.SimulationFrequency, settings.MaximumSubsteps,
            settings.Damping, settings.CourantNumber,
            settings.Gravity * (frame.HaveRenderer ? frame.Renderer.GravityMultiplier : 1.0f),
            settings.ShallowAttenuation, settings.HorizontalDisplacement,
            settings.DisplacementClamp, settings.MinimumCascade,
            settings.MaximumCascade);
        params.DynWavesHorizontalDisplacement = settings.HorizontalDisplacement;
        params.DynWavesDisplacementClamp = settings.DisplacementClamp;
    }
    else
    {
        feature.GetDynWaves().ClearSimulationConfiguration();
        params.DynWavesHorizontalDisplacement = 0.0f;
        params.DynWavesDisplacementClamp = 4.0f;
    }
    feature.GetDynWaves().SetFrameInputs(
        impulses.empty() ? nullptr : impulses.data(), static_cast<uint32>(impulses.size()),
        kDynWaveSpeed, kDynDamping, frame.OceanDeltaTime);
}

void PublishInputStats(OceanRenderFeature& feature, OceanExtractionFrame& frame)
{
    OceanInputFrameStats& inputStats = frame.Inputs.Stats;
    frame.Inputs.FinalizeCounts();
    inputStats.UnderwaterVolumes = frame.UnderwaterVolumeCount;
    inputStats.UnderwaterVolumePolygons = frame.UnderwaterVolumePolygonCount;
    inputStats.UnderwaterExclusionVolumes = frame.UnderwaterExclusionVolumeCount;
    inputStats.WaterBodyBoxes = static_cast<uint32>(frame.WaterBodyBounds.size());
    inputStats.WaterBodyPolygons = static_cast<uint32>(frame.WaterBodyPolygons.size());
    inputStats.WaterBodyStamps = static_cast<uint32>(frame.WaterBodyStamps.size());
    inputStats.SplineInputs = frame.SplineInputCount;
    inputStats.RibbonTriangles = static_cast<uint32>(feature.GetSplineRaster().GetTriangleCount());
    feature.SetInputStats(inputStats);
}

} // anonymous namespace

OceanExtractionSystem::OceanExtractionSystem(Engine::Renderer::RenderServices* renderServices)
    : m_RenderServices(renderServices)
{
}

void OceanExtractionSystem::Update(ECS::World& world, float32 deltaTime)
{
    if (!m_RenderServices)
        return;

    auto& feature = m_RenderServices->EnsureFeature<OceanRenderFeature>();
    m_Time += deltaTime;
    m_PresetOverrideValidator.Validate(world);

    OceanExtractionFrame frame;
    frame.Params.PatchExtent = kDefaultPatchExtent;

    ResolveRenderer(world, frame);
    ResolveSettingsAssets(feature, frame);
    ResolveQualityAndStreaming(m_RenderServices, frame);
    SampleOceanClock(feature, frame, deltaTime);
    ConfigureCollisionProvider(feature, frame);

    ExtractSurface(world, feature, frame);
    GatherWaterBodies(world, frame);
    GatherPolygonWaterBodies(world, frame);
    AssignLocalFftStreams(frame);
    CollectConfiningWaterBodies(frame);
    ApplyQualityTier(frame);
    ExtractSun(world, frame.Params);
    GatherSavedDepthCaches(world, frame);
    GatherDepthSources(world, frame);
    PublishWaterMaterials(world, feature, frame);
    PublishTypedInputs(world, feature, frame);
    PublishSplineRibbons(world, feature, frame);
    PublishWaterBodies(feature, frame);
    PublishSeabedDepth(feature, frame);
    ExtractFlowSources(world, *m_RenderServices, feature, frame);
    ExtractWaveMasks(world, *m_RenderServices, feature, frame);
    ExtractClipSources(world, feature, frame);
    ExtractAlbedoSources(world, feature, frame);
    ExtractUnderwaterVolumes(world, feature, frame);
    GatherWaveImpulses(world, feature, frame, deltaTime);
    PublishSurfaceTogglesAndTextures(*m_RenderServices, feature, frame);
    PublishCascadeConfig(feature, frame);

    if (frame.HasSurface)
        PublishWaveSimulations(world, feature, frame);

    PublishInputStats(feature, frame);
    if (!frame.HasSurface)
    {
        for (uint32 stream = 0u; stream < kMaxOceanLocalFFTStreams; ++stream)
            feature.SetLocalFFTParams(stream, OceanFFTParamsGPU{}, false, false);
        feature.SetPlanarReflectionScale(0.5f);
    }
    feature.SetHasOcean(frame.HasSurface);
    PublishWorldContent(feature, world.GetWorldId(), frame.HasSurface);
    feature.PublishDepthClaim();
    feature.ProcessQueuedSurfaceQueries();
}

void OceanExtractionSystem::ResolveRenderer(ECS::World& world, OceanExtractionFrame& frame)
{
    // Renderer-level config (quality tier, global wind, gravity multiplier, LOD
    // budget). Optional — when absent the tier defaults to High and the spectrum's
    // own wind is used. One per scene; the first enabled wins.
    GUID presetGuid = GUID::Null();
    world.Query<ECS::Read<Components::OceanRenderer>,
                ECS::Optional<Components::OceanPresetBinding>>()
        .Each(
            [&](ECS::EntityHandle entity, const Components::OceanRenderer& r,
                const Components::OceanPresetBinding* presetBinding)
            {
                if (frame.HaveRenderer)
                    return;
                frame.HaveRenderer = true;
                frame.Renderer = r;
                if (presetBinding)
                {
                    frame.RendererPresetEntity = entity;
                    presetGuid = presetBinding->Preset.ToGuid();
                    frame.HaveRendererPresetBinding = true;
                }
            });
    m_LastPresetAssetGuid = presetGuid;
    m_PresetAsset = m_Presets.Find(presetGuid);
    if (m_PresetAsset && frame.HaveRendererPresetBinding)
    {
        m_PresetAsset->Apply("Renderer", ECS::GetComponentTypeId<Components::OceanRenderer>(),
                             &frame.Renderer, sizeof(frame.Renderer),
                             PresetOverridesOf(world, frame.RendererPresetEntity));
    }
}

const OceanSettingsAsset* OceanExtractionSystem::ResolveSettingsAsset(
    const GUID& guid, OceanSettingsKind expectedKind, uint32 slot)
{
    bool changed = guid != m_LastSettingsAssetGuids[slot];
    if (!changed && !guid.IsNull() && !m_SettingsAssetPaths[slot].empty())
    {
        std::error_code ec;
        const auto writeTime =
            std::filesystem::last_write_time(m_SettingsAssetPaths[slot], ec);
        changed = !ec && writeTime != m_SettingsAssetWriteTimes[slot];
    }
    if (changed)
    {
        m_LastSettingsAssetGuids[slot] = guid;
        m_SettingsAssets[slot].reset();
        m_SettingsAssetPaths[slot].clear();
        m_SettingsAssetWriteTimes[slot] = {};
        if (!guid.IsNull())
        {
            AssetManager& manager = EngineCore::GetInstance().GetAssetManager();
            SharedPtr<Asset> asset = manager.GetAsset(guid);
            if (!asset || !asset->IsLoaded())
                asset = manager.LoadAssetAsync(guid).get();
            if (asset && asset->IsLoaded() &&
                (asset->GetType() == AssetType::OceanSettings ||
                 asset->GetType() == AssetType::Unknown))
            {
                auto settings = std::make_shared<OceanSettingsAsset>();
                if (settings->LoadJson(asset->GetPath()) && settings->Kind == expectedKind)
                {
                    m_SettingsAssets[slot] = std::move(settings);
                    m_SettingsAssetPaths[slot] = asset->GetPath();
                    std::error_code ec;
                    m_SettingsAssetWriteTimes[slot] = std::filesystem::last_write_time(
                        m_SettingsAssetPaths[slot], ec);
                    if (ec)
                        m_SettingsAssetWriteTimes[slot] = {};
                }
            }
        }
    }
    const auto& settings = m_SettingsAssets[slot];
    return settings && settings->Kind == expectedKind ? settings.get() : nullptr;
}

void OceanExtractionSystem::ResolveSettingsAssets(OceanRenderFeature& feature,
                                                  OceanExtractionFrame& frame)
{
    // Resolve the four reusable settings sections. Inline component fields stay
    // authoritative when no asset is assigned; an assigned asset replaces only
    // the matching typed section. Keeping the cache per slot also avoids parsing
    // JSON every extraction tick while still making a changed reference live.
    const OceanSettingsAsset* shadowSettings = nullptr;
    if (frame.HaveRenderer)
    {
        const Components::OceanRenderer& renderer = frame.Renderer;
        frame.CollisionSettings =
            ResolveSettingsAsset(renderer.AnimatedWavesCollisionSettings.ToGuid(),
                                 OceanSettingsKind::AnimatedWavesCollision, 0u);
        frame.DynamicSettings = ResolveSettingsAsset(renderer.DynamicWaveSettings.ToGuid(),
                                                     OceanSettingsKind::DynamicWaves, 1u);
        frame.FoamSettings =
            ResolveSettingsAsset(renderer.FoamSettings.ToGuid(), OceanSettingsKind::Foam, 2u);
        shadowSettings = ResolveSettingsAsset(renderer.ShadowSettings.ToGuid(),
                                              OceanSettingsKind::Shadows, 3u);
    }
    else
    {
        for (uint32 slot = 0u; slot < m_SettingsAssets.size(); ++slot)
        {
            m_LastSettingsAssetGuids[slot] = GUID::Null();
            m_SettingsAssets[slot].reset();
            m_SettingsAssetPaths[slot].clear();
            m_SettingsAssetWriteTimes[slot] = {};
        }
    }
    // Shadow settings are resolved here with the other live assets; the temporal
    // shadow cascade consumes this pointer once its render-graph stage is active.
    feature.GetShadows().SetSettings(shadowSettings ? shadowSettings->Shadows : OceanShadowSettings{});
}

void OceanExtractionSystem::SampleOceanClock(OceanRenderFeature& feature,
                                             OceanExtractionFrame& frame, float32 deltaTime)
{
    OceanTimeSample oceanClock{};
    if (!frame.HaveRenderer || frame.Renderer.TimeProvider == Components::OceanTimeProviderMode::Default)
    {
        oceanClock = m_DefaultTimeProvider.Sample(m_Time, deltaTime);
    }
    else if (frame.Renderer.TimeProvider == Components::OceanTimeProviderMode::NetworkOffset)
    {
        m_NetworkTimeProvider.SetNetworkOffset(frame.Renderer.NetworkTimeOffset);
        m_NetworkTimeProvider.SetRate(frame.Renderer.NetworkTimeRate);
        oceanClock = m_NetworkTimeProvider.Sample(m_Time, deltaTime);
    }
    else if (frame.Renderer.TimeProvider == Components::OceanTimeProviderMode::Timeline)
    {
        m_TimelineTimeProvider.SetTimelineTime(frame.Renderer.TimelineTime,
                                               frame.Renderer.TimelinePlaying,
                                               frame.Renderer.TimelinePlaybackRate);
        oceanClock = m_TimelineTimeProvider.Sample(m_Time, deltaTime);
    }
    else
    {
        m_CustomTimeProvider.SetScale(frame.Renderer.TimeScale);
        m_CustomTimeProvider.SetOffset(frame.Renderer.TimeOffset);
        const bool paused = frame.Renderer.TimeProvider == Components::OceanTimeProviderMode::Paused;
        m_CustomTimeProvider.SetFixedTime(paused || frame.Renderer.UseFixedTime,
                                          paused ? frame.Renderer.PausedTime : frame.Renderer.FixedTime);
        oceanClock = m_CustomTimeProvider.Sample(m_Time, deltaTime);
    }
    frame.OceanTime = oceanClock.Time;
    frame.OceanDeltaTime = oceanClock.DeltaTime;
    frame.Params.Time = frame.OceanTime;
    frame.Params.WaveOriginOffsetX = feature.GetWaveOriginOffsetX();
    frame.Params.WaveOriginOffsetZ = feature.GetWaveOriginOffsetZ();
}

void OceanExtractionSystem::ConfigureCollisionProvider(OceanRenderFeature& feature,
                                                       OceanExtractionFrame& frame)
{
    auto& collisionProvider = static_cast<OceanFeatureCollisionProvider&>(
        feature.GetCollisionProvider());
    const uint32 collisionMode = frame.CollisionSettings
                                     ? std::min(static_cast<uint32>(
                                                    frame.CollisionSettings->AnimatedWavesCollision.Provider),
                                                4u)
                                     : (frame.HaveRenderer ? std::min(frame.Renderer.CollisionProvider, 4u)
                                                     : 0u);
    collisionProvider.SetMode(static_cast<OceanCollisionProviderMode>(collisionMode));
    collisionProvider.SetMaxQueryPoints(
        frame.CollisionSettings
            ? std::max(frame.CollisionSettings->AnimatedWavesCollision.MaximumQueryPoints, 1u)
            : (frame.HaveRenderer ? std::max(frame.Renderer.MaxCollisionQueryCount, 1u) : 8192u));
    if (frame.CollisionSettings)
    {
        const auto& settings = frame.CollisionSettings->AnimatedWavesCollision;
        collisionProvider.SetFallbackPolicy(settings.AllowGPUQueries, settings.AllowBakedFFT,
                                            settings.AllowGerstnerFallback,
                                            settings.DefaultMinimumSpatialLength);
    }
    else
    {
        collisionProvider.SetFallbackPolicy(true, true, true, 0.0f);
    }

    const GUID collisionAssetGuid = frame.HaveRenderer
                                        ? frame.Renderer.FFTCollisionAsset.ToGuid()
                                        : GUID::Null();
    if (collisionAssetGuid != m_LastCollisionAssetGuid)
    {
        m_LastCollisionAssetGuid = collisionAssetGuid;
        m_CollisionAsset.reset();
        if (!collisionAssetGuid.IsNull())
        {
            AssetManager& manager = EngineCore::GetInstance().GetAssetManager();
            SharedPtr<Asset> asset = manager.GetAsset(collisionAssetGuid);
            if (!asset || !asset->IsLoaded())
                asset = manager.LoadAssetAsync(collisionAssetGuid).get();
            if (asset && asset->IsLoaded() &&
                (asset->GetType() == AssetType::OceanFFTCollision ||
                 asset->GetType() == AssetType::Unknown))
            {
                auto collision = std::make_shared<OceanFFTCollisionAsset>();
                if (collision->LoadBinary(asset->GetPath()))
                    m_CollisionAsset = std::move(collision);
            }
        }
        collisionProvider.SetBakedFFTAsset(m_CollisionAsset);
    }
    frame.CollisionProvider = &collisionProvider;
}

void OceanExtractionSystem::ExtractSurface(ECS::World& world, OceanRenderFeature& feature,
                                           OceanExtractionFrame& frame)
{
    world.Query<ECS::Read<Components::OceanSurface>,
                ECS::Optional<Components::OceanPresetBinding>,
                ECS::Optional<Components::Transform>,
                ECS::Optional<Components::WorldTransform>>()
        .Each(
        [&](ECS::EntityHandle, const Components::OceanSurface& authoredOcean,
            const Components::OceanPresetBinding* presetBinding,
            const Components::Transform* transform,
            const Components::WorldTransform* worldTransform)
        {
            Components::OceanSurface ocean = authoredOcean;
            ApplyBoundPreset(m_PresetAsset.get(), m_LastPresetAssetGuid, presetBinding,
                             "Surface", ocean);
            if (frame.HasSurface)
                return;
            frame.HasSurface = true;
            ApplySurface(ocean, transform, worldTransform, feature, frame);
        });
}

void OceanExtractionSystem::GatherWaterBodies(ECS::World& world, OceanExtractionFrame& frame)
{
    // Bounded water bodies: authoring-level lake/pool/river rectangles that drive
    // the existing clip, flow, and underwater-volume systems.
    world.Query<ECS::Read<Components::OceanWaterBody>, ECS::Read<Components::WorldTransform>,
                ECS::Optional<Components::OceanWaveSpectrum>,
                ECS::Optional<Components::OceanPresetBinding>>()
        .Each(
            [&](ECS::EntityHandle, const Components::OceanWaterBody& authoredWaterBody,
                const Components::WorldTransform& wt,
                const Components::OceanWaveSpectrum* localSpectrum,
                const Components::OceanPresetBinding* presetBinding)
            {
                Components::OceanWaterBody wb = authoredWaterBody;
                ApplyBoundPreset(m_PresetAsset.get(), m_LastPresetAssetGuid, presetBinding,
                                 "WaterBody", wb);
                AppendWaterBody(wb, wt, localSpectrum, frame);
            });
}

void OceanExtractionSystem::GatherPolygonWaterBodies(ECS::World& world,
                                                     OceanExtractionFrame& frame)
{
    // Polygon water bodies: exact filled footprints for the clip cascade and CPU
    // surface queries. Flow/underwater volume use the polygon's AABB as a cheap
    // conservative runtime approximation.
    world.Query<ECS::Read<Components::OceanPolygonWaterBody>, ECS::Read<Components::WorldTransform>,
                ECS::Optional<Components::OceanWaveSpectrum>,
                ECS::Optional<Components::OceanPresetBinding>>()
        .Each(
            [&](ECS::EntityHandle, const Components::OceanPolygonWaterBody& authoredWaterBody,
                const Components::WorldTransform& wt,
                const Components::OceanWaveSpectrum* localSpectrum,
                const Components::OceanPresetBinding* presetBinding)
            {
                Components::OceanPolygonWaterBody wb = authoredWaterBody;
                ApplyBoundPreset(m_PresetAsset.get(), m_LastPresetAssetGuid, presetBinding,
                                 "WaterBody", wb);
                AppendPolygonWaterBody(wb, wt, localSpectrum, frame);
            });
}

void OceanExtractionSystem::PublishWaterMaterials(ECS::World& world, OceanRenderFeature& feature,
                                                  const OceanExtractionFrame& frame)
{
    std::vector<OceanWaterMaterialGPU> waterMaterials;
    world.Query<ECS::Read<Components::OceanWaterBody>, ECS::Read<Components::WorldTransform>>()
        .Without<ECS::Disabled>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanWaterBody &body,
                  const Components::WorldTransform &wt) {
            if (body.ExtentX <= 0 || body.ExtentZ <= 0)
                return;
            auto *m = AppendWaterMaterial(m_Presets, *m_RenderServices,
                                          body.MaterialOverride.ToGuid(), body.MaterialPriority,
                                          entity.id, frame.MaterialBaseSurface, waterMaterials);
            if (!m)
                return;
            SetWaterMaterialRectangle(*m, body, wt);
        });
    world.Query<ECS::Read<Components::OceanPolygonWaterBody>, ECS::Read<Components::WorldTransform>>()
        .Without<ECS::Disabled>()
        .Each([&](ECS::EntityHandle entity, const Components::OceanPolygonWaterBody &body,
                  const Components::WorldTransform &wt) {
            if (body.PointCount < 3)
                return;
            auto *m = AppendWaterMaterial(m_Presets, *m_RenderServices,
                                          body.MaterialOverride.ToGuid(), body.MaterialPriority,
                                          entity.id, frame.MaterialBaseSurface, waterMaterials);
            if (!m)
                return;
            SetWaterMaterialPolygon(*m, body, wt);
        });
    std::sort(waterMaterials.begin(), waterMaterials.end(), [](const auto &a, const auto &b) {
        return a.Priority == b.Priority ? a.EntityId < b.EntityId : a.Priority < b.Priority;
    });
    feature.SetWaterMaterials(std::move(waterMaterials));
}

void OceanExtractionSystem::GatherWaveImpulses(ECS::World& world, OceanRenderFeature& feature,
                                               OceanExtractionFrame& frame, float32 deltaTime)
{
    // Tagged wave impulses → per-frame impulse list for the dynamic-wave sim.
    // The entity origin XZ supplies the impact center. Capped at
    // kMaxOceanWaveImpulses. Gathered only when the DynamicWaves toggle is on.
    if (!(frame.DynWavesEnabled && frame.OceanDeltaTime > 1e-6f))
        return;

    OceanInputRegistryFrame& inputs = frame.Inputs;
    world.Query<ECS::Read<Components::OceanWaveImpulse>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle, const Components::OceanWaveImpulse& imp,
                  const Components::WorldTransform& wt) { AddWaveImpulse(imp, wt, inputs); });

    // Interaction wakes/splashes: bodies tagged OceanWaterInteraction inject a
    // velocity-scaled impulse as they move (the reference's
    // SphereWaterInteraction), so boats/objects leave wakes automatically and
    // vertical entry/exit can splash. Speed is derived from the world-position
    // delta vs last frame (no physics dependency — works for kinematic and
    // simulated bodies). A still body injects nothing.
    if (deltaTime > 1e-4f)
    {
        world.Query<ECS::Read<Components::OceanWaterInteraction>,
                    ECS::Read<Components::WorldTransform>>()
            .Each(
                [&](ECS::EntityHandle entity, const Components::OceanWaterInteraction& inter,
                    const Components::WorldTransform& wt)
                {
                    InjectInteractionWake(entity.id, inter, wt, deltaTime, feature,
                                          m_PrevInteractionXYZ, inputs);
                });
    }
}

void OceanExtractionSystem::PublishWaveSimulations(ECS::World& world, OceanRenderFeature& feature,
                                                   OceanExtractionFrame& frame)
{
    const Components::OceanWaveSpectrum spec = ResolveGlobalSpectrum(world, frame);
    PublishGlobalSpectrum(feature, frame, spec);
    PublishLocalSpectra(feature, frame);
    ConfigureFoamSimulation(feature, frame);
    ConfigureDynamicWaveSimulation(feature, frame);

    // Advanced settings above can affect visible dynamic displacement after
    // the initial spectrum upload. Publish the final parameter block once
    // more so rendering and CPU collision consume identical values.
    feature.SetParams(frame.Params);
}

Components::OceanWaveSpectrum OceanExtractionSystem::ResolveGlobalSpectrum(
    ECS::World& world, const OceanExtractionFrame& frame) const
{
    Components::OceanWaveSpectrum spec{};
    Components::OceanWaveSpectrum assetSpec{};
    bool haveSpec = frame.HaveRenderer &&
                    TryLoadOceanWaveSpectrumAsset(frame.Renderer.SpectrumAsset, assetSpec);
    if (haveSpec)
    {
        spec = assetSpec;
        if (m_PresetAsset && frame.HaveRendererPresetBinding)
            m_PresetAsset->Apply(
                "Spectrum", ECS::GetComponentTypeId<Components::OceanWaveSpectrum>(),
                &spec, sizeof(spec), PresetOverridesOf(world, frame.RendererPresetEntity));
    }
    world.Query<ECS::Read<Components::OceanWaveSpectrum>,
                ECS::Optional<Components::OceanPresetBinding>>()
        .Each(
            [&](ECS::EntityHandle, const Components::OceanWaveSpectrum& authoredSpectrum,
                const Components::OceanPresetBinding* presetBinding)
            {
                if (haveSpec)
                    return;
                haveSpec = true;
                spec = authoredSpectrum;
                ApplyBoundPreset(m_PresetAsset.get(), m_LastPresetAssetGuid, presetBinding,
                                 "Spectrum", spec);
            });

    // Global wind: when an OceanRenderer is present it owns wind, including
    // GlobalWindSpeed == 0 for a flat calm sea. Build both representations from
    // the same resolved spectrum so fallback rendering and CPU queries retain
    // the authored phase/amplitude contract.
    if (frame.HaveRenderer)
    {
        spec.WindSpeed = ClampNonNegativeFinite(frame.Renderer.GlobalWindSpeed);
        spec.WindDirectionDegrees = frame.Renderer.GlobalWindDirection;
        spec.Turbulence = frame.Renderer.GlobalWindTurbulence;
    }
    else
    {
        spec.WindSpeed = ClampNonNegativeFinite(spec.WindSpeed);
    }
    return spec;
}

void OceanExtractionSystem::PublishGlobalSpectrum(OceanRenderFeature& feature,
                                                  OceanExtractionFrame& frame,
                                                  const Components::OceanWaveSpectrum& spec)
{
    OceanParamsGPU& params = frame.Params;
    const float windSpeed = ClampNonNegativeFinite(spec.WindSpeed);
    const float windDirDeg = spec.WindDirectionDegrees;
    feature.GetSprayGPU().SetSettings(frame.MaterialBaseSurface, windSpeed, windDirDeg);
    const float turbulence = spec.Turbulence;
    const float gravityMul =
        (frame.HaveRenderer && frame.Renderer.GravityMultiplier > 0.0f) ? frame.Renderer.GravityMultiplier : 1.0f;

    BuildGerstnerWaves(spec, params);

    // FFT cascade count gates surface sampling. A 0 count means the FFT failed
    // to initialize (or an old scene explicitly selected Gerstner), so the
    // surface and queries use the analytic set above.
    const bool fftRequested =
        params.WaveMode == static_cast<uint32>(Components::OceanWaveMode::FFT);
    const bool fftActive = fftRequested && feature.IsFFTReady();
    params.FFTCascadeCount = fftActive ? kOceanFFTCascades : 0u;

    // Shape FFT wave-shape controls (surface-side; applied to the sampled FFT
    // displacement by OceanShapeDisplacement in the vertex + macro-normal paths).
    params.Weight = spec.Weight;
    params.MaxHorizontalDisplacement = spec.MaxHorizontalDisplacement;
    params.MaxVerticalDisplacement = spec.MaxVerticalDisplacement;
    params.RespectShallowAttenuation = spec.RespectShallowWaterAttenuation;
    feature.SetParams(params);
    feature.SetUnderwaterSettings(frame.UnderwaterSettings);

    // FFT spectrum params (the reference parity). Gravity-scale is folded into time.
    OceanFFTParamsGPU fft{};
    fft.Resolution = kOceanFFTResolution;
    fft.CascadeCount = kOceanFFTCascades;
    fft.Gravity = 9.81f * gravityMul;
    fft.Time = frame.OceanTime * spec.GravityScale; // the reference scales sim time by gravityScale
    fft.WindSpeed = windSpeed;
    const float windRad = windDirDeg * 0.01745329252f;
    fft.WindDirX = std::cos(windRad);
    fft.WindDirZ = std::sin(windRad);
    fft.Turbulence = turbulence;
    fft.Chop = spec.Chop;
    fft.Multiplier = spec.Multiplier;
    fft.Period = spec.LoopPeriod;
    FillSpectrumLUT(spec.Multiplier, spec.SpectrumPower, fft.SpectrumPower);
    FillOctaveArray(spec.ChopScales, fft.ChopScales, 1.0f);
    FillOctaveArray(spec.GravityScales, fft.GravityScales, 1.0f);
    uint32 disableMask = 0u;
    for (uint32 i = 0; i < kOceanSpectrumOctaves; ++i)
        if (spec.OctaveDisabled[i])
            disableMask |= (1u << i);
    fft.OctaveDisableMask = disableMask;
    const bool dirty =
        UpdateGlobalSpectrumCache(spec, windSpeed, windDirDeg, turbulence, gravityMul, disableMask);

    feature.SetFFTParams(fft, dirty);
    if (feature.IsCombineReady())
        feature.GetCombineSim().SetFrameInputs(feature.GetDisplacementTexture(),
                                                feature.GetDisplacementSampler(),
                                                params.FFTCascadeCount,
                                                params.WaveOriginOffsetX,
                                                params.WaveOriginOffsetZ);
    frame.CollisionProvider->SetExpectedSpectrumHash(
        OceanFFTCollisionAsset::ComputeSpectrumHash(fft));
    frame.WindSpeed = windSpeed;
    frame.WindRadians = windRad;
    frame.FftActive = fftActive;
}

bool OceanExtractionSystem::UpdateGlobalSpectrumCache(const Components::OceanWaveSpectrum& spec,
                                                      float32 windSpeed, float32 windDirDeg,
                                                      float32 turbulence, float32 gravityMul,
                                                      uint32 disableMask)
{
    // Re-init the H0 spectrum only when a spectrum-affecting input changes.
    // The per-octave arrays + disable mask + the gravity multiplier feed the
    // init pass, so a change to any of them must re-bake H0.
    const bool octaveDirty =
        std::memcmp(spec.SpectrumPower, m_LastSpectrumPower, sizeof(m_LastSpectrumPower)) != 0 ||
        std::memcmp(spec.ChopScales, m_LastChopScales, sizeof(m_LastChopScales)) != 0 ||
        std::memcmp(spec.GravityScales, m_LastGravityScales, sizeof(m_LastGravityScales)) != 0 ||
        disableMask != m_LastOctaveDisableMask;
    const bool dirty = !m_HaveLastSpectrum ||
                       windSpeed != m_LastWindSpeed ||
                       windDirDeg != m_LastWindDirDeg ||
                       turbulence != m_LastTurbulence ||
                       spec.Multiplier != m_LastMultiplier ||
                       spec.LoopPeriod != m_LastPeriod ||
                       gravityMul != m_LastGravityMul ||
                       octaveDirty;
    m_HaveLastSpectrum = true;
    m_LastWindSpeed = windSpeed;
    m_LastWindDirDeg = windDirDeg;
    m_LastTurbulence = turbulence;
    m_LastMultiplier = spec.Multiplier;
    m_LastPeriod = spec.LoopPeriod;
    m_LastGravityMul = gravityMul;
    std::memcpy(m_LastSpectrumPower, spec.SpectrumPower, sizeof(m_LastSpectrumPower));
    std::memcpy(m_LastChopScales, spec.ChopScales, sizeof(m_LastChopScales));
    std::memcpy(m_LastGravityScales, spec.GravityScales, sizeof(m_LastGravityScales));
    m_LastOctaveDisableMask = disableMask;
    return dirty;
}

void OceanExtractionSystem::PublishLocalSpectra(OceanRenderFeature& feature,
                                                const OceanExtractionFrame& frame)
{
    const uint32 localStreamCount = frame.FftActive
        ? std::min<uint32>(static_cast<uint32>(frame.LocalFftSpecs.size()),
                           kMaxOceanLocalFFTStreams)
        : 0u;
    for (uint32 stream = 0u; stream < localStreamCount; ++stream)
        PublishLocalSpectrum(feature, stream, frame.LocalFftSpecs[stream], frame.OceanTime);
    for (uint32 stream = localStreamCount; stream < kMaxOceanLocalFFTStreams; ++stream)
        feature.SetLocalFFTParams(stream, OceanFFTParamsGPU{}, false, false);
}

void OceanExtractionSystem::PublishLocalSpectrum(OceanRenderFeature& feature, uint32 stream,
                                                 const Components::OceanWaveSpectrum& localFftSpec,
                                                 float32 oceanTime)
{
    OceanFFTParamsGPU localFft{};
    localFft.Resolution = kOceanFFTResolution;
    localFft.CascadeCount = kOceanFFTCascades;
    const float localGravityMul = localFftSpec.GravityScale;
    localFft.Gravity = 9.81f * localGravityMul;
    localFft.Time = oceanTime * localGravityMul;
    const float localWindSpeed = ClampNonNegativeFinite(localFftSpec.WindSpeed);
    const float localWindDirDeg = localFftSpec.WindDirectionDegrees;
    const float localTurbulence = localFftSpec.Turbulence;
    localFft.WindSpeed = localWindSpeed;
    const float localWindRad = localWindDirDeg * 0.01745329252f;
    localFft.WindDirX = std::cos(localWindRad);
    localFft.WindDirZ = std::sin(localWindRad);
    localFft.Turbulence = localTurbulence;
    localFft.Chop = localFftSpec.Chop;
    localFft.Multiplier = localFftSpec.Multiplier;
    localFft.Period = localFftSpec.LoopPeriod;
    FillSpectrumLUT(localFftSpec.Multiplier, localFftSpec.SpectrumPower,
                    localFft.SpectrumPower);
    FillOctaveArray(localFftSpec.ChopScales, localFft.ChopScales, 1.0f);
    FillOctaveArray(localFftSpec.GravityScales, localFft.GravityScales, 1.0f);
    uint32 localDisableMask = 0u;
    for (uint32 i = 0; i < kOceanSpectrumOctaves; ++i)
        if (localFftSpec.OctaveDisabled[i])
            localDisableMask |= (1u << i);
    localFft.OctaveDisableMask = localDisableMask;

    const bool localDirty =
        UpdateLocalSpectrumCache(stream, localFftSpec, localWindSpeed, localWindDirDeg,
                                 localTurbulence, localGravityMul, localDisableMask);

    feature.SetLocalFFTParams(stream, localFft, localDirty, true);
}

bool OceanExtractionSystem::UpdateLocalSpectrumCache(
    uint32 stream, const Components::OceanWaveSpectrum& localFftSpec, float32 localWindSpeed,
    float32 localWindDirDeg, float32 localTurbulence, float32 localGravityMul,
    uint32 localDisableMask)
{
    const bool localOctaveDirty =
        std::memcmp(localFftSpec.SpectrumPower, m_LastLocalSpectrumPower[stream],
                    sizeof(m_LastLocalSpectrumPower[stream])) != 0 ||
        std::memcmp(localFftSpec.ChopScales, m_LastLocalChopScales[stream],
                    sizeof(m_LastLocalChopScales[stream])) != 0 ||
        std::memcmp(localFftSpec.GravityScales, m_LastLocalGravityScales[stream],
                    sizeof(m_LastLocalGravityScales[stream])) != 0 ||
        localDisableMask != m_LastLocalOctaveDisableMask[stream];
    const bool localDirty = !m_HaveLastLocalSpectrum[stream] ||
                            localWindSpeed != m_LastLocalWindSpeed[stream] ||
                            localWindDirDeg != m_LastLocalWindDirDeg[stream] ||
                            localTurbulence != m_LastLocalTurbulence[stream] ||
                            localFftSpec.Multiplier != m_LastLocalMultiplier[stream] ||
                            localFftSpec.LoopPeriod != m_LastLocalPeriod[stream] ||
                            localGravityMul != m_LastLocalGravityMul[stream] ||
                            localOctaveDirty;
    m_HaveLastLocalSpectrum[stream] = true;
    m_LastLocalWindSpeed[stream] = localWindSpeed;
    m_LastLocalWindDirDeg[stream] = localWindDirDeg;
    m_LastLocalTurbulence[stream] = localTurbulence;
    m_LastLocalMultiplier[stream] = localFftSpec.Multiplier;
    m_LastLocalPeriod[stream] = localFftSpec.LoopPeriod;
    m_LastLocalGravityMul[stream] = localGravityMul;
    std::memcpy(m_LastLocalSpectrumPower[stream], localFftSpec.SpectrumPower,
                sizeof(m_LastLocalSpectrumPower[stream]));
    std::memcpy(m_LastLocalChopScales[stream], localFftSpec.ChopScales,
                sizeof(m_LastLocalChopScales[stream]));
    std::memcpy(m_LastLocalGravityScales[stream], localFftSpec.GravityScales,
                sizeof(m_LastLocalGravityScales[stream]));
    m_LastLocalOctaveDisableMask[stream] = localDisableMask;
    return localDirty;
}

void OceanExtractionSystem::PublishWorldContent(OceanRenderFeature& feature, uint64 worldId,
                                                bool hasSurface)
{
    Engine::Renderer::WorldContentDigestTable& content = feature.GetWorldContent();
    if (!m_WorldContentTracking)
        m_WorldContentTracking = content.Track(worldId);
    content.Set(worldId, hasSurface ? kOceanSurfaceContentDigest : 0u);
}

} // namespace GameEngine::Ocean
