#include "DebugServer/CaptureDebugHandlers.h"

#include "Editor/CaptureOutputDirectory.h"
#include "DebugServer/DebugHandlerHelpers.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/RenderDocCapture.h"
#include "DebugServer/ScreenshotCaptureWait.h"
#include "DebugServer/WindowPrintCapture.h"

#include "Core/Engine.h"
#include "EditorApplication.h"
#include "EditorContext.h"
#include "EditorPanelIds.h"
#include "Image/Encode.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Engine/Rendering/DrawStreamLookupKey.h"
#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Logger/Logger.h"
#include "Panels/AssetViewPanel.h"
#include "Panels/SceneViewPanel.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGGraph.h"
#include "Rendering/Core/RenderGraph/RGTypes.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "SceneViewController.h"
#include "UI/Controls/Mount.h"
#include "UI/Layout/Docking.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UITextureSpace.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

// In-place gamma encode (linear → sRGB) for pixels already quantized to RGBA8.
// Only for sources that are 8-bit to begin with (no precision left to recover).
// Float sources must encode in float inside ConvertToRGBA8 BEFORE the quantize:
// encoding after quantizing spreads one 1/255 linear quantum across ~4 output
// LSB near black, manufacturing banding the screen never shows.
static void ApplySRGBEncodeRGBA8(std::vector<uint8_t>& rgba)
{
    auto encode = [](uint8_t v) -> uint8_t
    {
        float linear = static_cast<float>(v) / 255.0f;
        float srgb = (linear <= 0.0031308f)
                         ? 12.92f * linear
                         : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        srgb = std::max(0.0f, std::min(1.0f, srgb));
        return static_cast<uint8_t>(srgb * 255.0f + 0.5f);
    };
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        rgba[i + 0] = encode(rgba[i + 0]);
        rgba[i + 1] = encode(rgba[i + 1]);
        rgba[i + 2] = encode(rgba[i + 2]);
        // alpha is left untouched
    }
}

// Interleaved gradient noise, ported verbatim from encode_srgb.frag.
// Coordinates are pixel centers (x+0.5, y+0.5) to mirror gl_FragCoord.
static float InterleavedGradientNoiseAt(uint32_t px, uint32_t py, float offset)
{
    auto fract = [](float v) -> float { return v - std::floor(v); };
    const float x = static_cast<float>(px) + 0.5f + offset;
    const float y = static_cast<float>(py) + 0.5f + offset * 0.7f;
    return fract(52.9829189f * fract(0.06711056f * x + 0.00583715f * y));
}

// Per-pixel triangular-PDF dither, ported verbatim from encode_srgb.frag
// (InterleavedGradientNoise + inverse-CDF remap). The screen's sRGB encode pass
// dithers its 8-bit quantize with exactly this pattern; captures quantizing the
// same linear source must match or they show staircase banding the display
// never had. Returns a value in (-1, 1).
static float TriangularDitherAt(uint32_t px, uint32_t py)
{
    const float u = InterleavedGradientNoiseAt(px, py, 0.0f);
    const float o = 2.0f * u - 1.0f;
    const float s = (o > 0.0f) ? 1.0f : ((o < 0.0f) ? -1.0f : 0.0f);
    return s * (1.0f - std::sqrt(std::max(0.0f, 1.0f - std::abs(o))));
}

// Gradient-aware deband on the encoded float plane — the CPU mirror of
// encode_srgb.frag's pre-dither DebandEncoded, kept so captures continue to
// match the screen (the same parity contract as TriangularDitherAt above):
// identical noise fields, tap geometry, iteration schedule, and all-channel
// threshold gate, with clamp-to-edge bilinear taps mirroring the pass's
// sampler. One deliberate divergence: the shader encodes each bilinear tap of
// the LINEAR source, this taps the pre-ENCODED plane — the two differ only by
// transfer-curve curvature across a tap's bilinear footprint, sub-LSB on the
// shallow gradients the gate opens for, and the gate is shut at real edges
// either way. `thresholdLsb` is the pass-published effective gate — the SAME
// volume/env resolution the screen's backbuffer encode used (see
// GetBackbufferOutputDebandThresholdLsb), so a volume toggle or a GE_DEBAND
// relaunch changes screen and captures together. Radius stays process-wide.
//
// Reached only on frames whose composite is still LINEAR — an HDR-display
// capture, where the terminal pass remains the single encode and debands
// full-frame. On SDR frames each world view debanded inside its own image, so the
// composite arrives finalized and the conversion below requantizes it without
// running either filter.
static std::vector<float> DebandEncodedPlane(
    const std::vector<float>& enc, uint32_t width, uint32_t height, float thresholdLsb)
{
    const float threshold = thresholdLsb / 255.0f;
    const float baseRadius = Rendering::Passes::GetOutputDebandSettings().RadiusPx;
    constexpr int kIterations = 3; // encode_srgb.frag kDebandIterations
    constexpr float kGolden = 0.61803399f;
    constexpr float kTwoPi = 6.2831853f;

    auto fract = [](float v) -> float { return v - std::floor(v); };
    auto sampleBilinear = [&](float sx, float sy, float rgb[3])
    {
        // Pixel-center coordinates in, clamp-to-edge bilinear out.
        const float fx = std::clamp(sx - 0.5f, 0.0f, static_cast<float>(width - 1));
        const float fy = std::clamp(sy - 0.5f, 0.0f, static_cast<float>(height - 1));
        const uint32_t x0 = static_cast<uint32_t>(fx);
        const uint32_t y0 = static_cast<uint32_t>(fy);
        const uint32_t x1 = std::min(x0 + 1, width - 1);
        const uint32_t y1 = std::min(y0 + 1, height - 1);
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);
        const float* p00 = &enc[(static_cast<size_t>(y0) * width + x0) * 3];
        const float* p10 = &enc[(static_cast<size_t>(y0) * width + x1) * 3];
        const float* p01 = &enc[(static_cast<size_t>(y1) * width + x0) * 3];
        const float* p11 = &enc[(static_cast<size_t>(y1) * width + x1) * 3];
        for (int c = 0; c < 3; ++c)
        {
            const float top = p00[c] + (p10[c] - p00[c]) * tx;
            const float bot = p01[c] + (p11[c] - p01[c]) * tx;
            rgb[c] = top + (bot - top) * ty;
        }
    };

    std::vector<float> out(enc.size());
    for (uint32_t py = 0; py < height; ++py)
    {
        for (uint32_t px = 0; px < width; ++px)
        {
            const float u1 = InterleavedGradientNoiseAt(px, py, 5.0f);
            const float u2 = InterleavedGradientNoiseAt(px, py, 11.0f);
            const float cx = static_cast<float>(px) + 0.5f;
            const float cy = static_cast<float>(py) + 0.5f;
            float res[3];
            const float* center = &enc[(static_cast<size_t>(py) * width + px) * 3];
            res[0] = center[0];
            res[1] = center[1];
            res[2] = center[2];
            for (int i = 1; i <= kIterations; ++i)
            {
                const float r1 = fract(u1 + static_cast<float>(i) * kGolden);
                const float r2 = fract(u2 + static_cast<float>(i) * kGolden);
                const float dist = baseRadius * static_cast<float>(i) * (0.25f + 0.75f * r1);
                const float angle = kTwoPi * r2;
                const float ox = dist * std::cos(angle);
                const float oy = dist * std::sin(angle);
                float t0[3], t1[3], t2[3], t3[3];
                sampleBilinear(cx + ox, cy + oy, t0);
                sampleBilinear(cx - oy, cy + ox, t1);
                sampleBilinear(cx - ox, cy - oy, t2);
                sampleBilinear(cx + oy, cy - ox, t3);
                // Gate against the ORIGINAL center (mirrors DebandEncoded):
                // bounds total displacement to one threshold by construction.
                float avg[3];
                bool pass = true;
                for (int c = 0; c < 3; ++c)
                {
                    avg[c] = 0.25f * (t0[c] + t1[c] + t2[c] + t3[c]);
                    pass = pass && std::abs(avg[c] - center[c]) < threshold;
                }
                if (pass)
                {
                    res[0] = avg[0];
                    res[1] = avg[1];
                    res[2] = avg[2];
                }
            }
            float* dst = &out[(static_cast<size_t>(py) * width + px) * 3];
            dst[0] = res[0];
            dst[1] = res[1];
            dst[2] = res[2];
        }
    }
    return out;
}

// Encode-filter state a capture should mirror: the effective deband gate the
// screen's backbuffer encode declared. Snapshotted on the render thread right
// after the target window's encode declaration (the render callback) so a sibling
// window's later declaration can't clobber the value before the deferred
// conversion runs; null falls back to the process-wide publish.
struct CaptureEncodeFilterState
{
    float DebandThresholdLsb = 0.0f;
};

// Encoded-domain finish for float color planes (3 floats per pixel, linear,
// already range-scaled): transfer curve, optional deband, TPDF dither,
// quantize to RGBA8. Splitting the encode from the quantize (vs the inline
// toU8 in ConvertToRGBA8) lets the deband tap a fully-encoded plane the way
// the screen's shader taps its source; with the deband inactive the op order
// (curve, +dither, clamp-quantize) is identical to the inline path.
static void EncodeDitherQuantizePlane(
    std::vector<float>& plane, uint32_t width, uint32_t height,
    bool srgbEncode, bool dither, bool deband, const CaptureEncodeFilterState* filterState,
    std::vector<uint8_t>& rgba)
{
    // Captures always quantize to 8 bits whatever depth the swapchain runs at,
    // so the mirror's dither is sized to ITS OWN quantizer — not to the screen's.
    constexpr float kCaptureDitherLsb = 1.0f / 255.0f;
    // BOTH filters are conditioned on `srgbEncode`, which asks "does this
    // conversion own the transfer curve?" — and therefore also "has anything
    // quantized these values for a destination yet?". A linear source (an
    // HDR-display frame's composite, a float data plane) has not been, so the
    // mirror encodes it and runs the screen's filters. An already-encoded source
    // arrives FINALIZED: every world view applied the OETF, debanded and dithered
    // at the presented step inside its own image, and the chrome around it is CSS
    // bytes that quantize exactly. Filtering that a second time would displace
    // world pixels the screen kept and grain chrome the screen left byte-stable,
    // so the conversion is a pure requantize — which makes screen-vs-capture
    // parity exact on chrome by construction. The residual it accepts: an 8-bit
    // capture of a world dithered for a 10-bit screen keeps only a quarter of
    // that noise, so a shallow gradient can contour in the PNG where the screen
    // shows none. Fixing that belongs upstream (finalize at the capture's step),
    // not in a filter here that cannot tell world from chrome.
    if (srgbEncode)
    {
        for (float& v : plane)
            v = (v <= 0.0031308f) ? 12.92f * v
                                  : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    }
    // The gate is the pass-published effective threshold (volume AND env
    // resolved), 0 whenever the screen's filter is off — captures and screen
    // stay in lockstep through every combination of lever and override.
    const float effectiveDebandLsb =
        filterState ? filterState->DebandThresholdLsb
                    : Rendering::Passes::GetBackbufferOutputDebandThresholdLsb();
    if (deband && srgbEncode && effectiveDebandLsb > 0.0f)
        plane = DebandEncodedPlane(plane, width, height, effectiveDebandLsb);

    const bool ditherThisPlane = dither && srgbEncode;
    for (uint32_t py = 0; py < height; ++py)
    {
        for (uint32_t px = 0; px < width; ++px)
        {
            const size_t i = static_cast<size_t>(py) * width + px;
            const float d =
                ditherThisPlane ? TriangularDitherAt(px, py) * kCaptureDitherLsb : 0.0f;
            for (int c = 0; c < 3; ++c)
            {
                const float v = plane[i * 3 + c] + d;
                rgba[i * 4 + c] =
                    static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)));
            }
        }
    }
}

// Convert raw readback bytes into RGBA8 for PNG encoding.
// Handles depth (linearize 0-1 → grayscale), float (tonemap), and integer formats.
// srgbEncode applies the linear→sRGB transfer curve (same constants as
// ApplySRGBEncodeRGBA8) — in float before the quantize for float/packed sources,
// in 8-bit for sources that are already quantized.
// dither adds a 1-LSB (1/255) TPDF offset in output space right before any
// float→8-bit quantize, matching the screen's encode pass. It never applies to
// sources that are already 8-bit — there is no sub-LSB information to recover,
// noise would only corrupt them.
// deband mirrors the screen's pre-dither deband on sRGB-encoded float sources
// (see DebandEncodedPlane); like dither it recovers/reshapes sub-LSB structure
// and never applies to already-quantized sources.
static std::vector<uint8_t> ConvertToRGBA8(
    const void* raw, uint32_t width, uint32_t height,
    Rendering::TextureFormat fmt, float rangeMin, float rangeMax, bool srgbEncode,
    bool dither, bool deband, const CaptureEncodeFilterState* filterState = nullptr)
{
    using TF = Rendering::TextureFormat;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<uint8_t> rgba(pixelCount * 4, 255);

    constexpr float kDitherLsb = 1.0f / 255.0f;

    // d is the per-pixel TPDF offset (already scaled to the 1/255 output LSB;
    // 0 when dithering is off), applied after the transfer curve — the same
    // order as encode_srgb.frag (encode, dither in sRGB space, quantize).
    auto toU8 = [srgbEncode](float v, float d) -> uint8_t
    {
        if (srgbEncode)
            v = (v <= 0.0031308f) ? 12.92f * v
                                  : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        v += d;
        return static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)));
    };

    auto ditherAt = [dither, width](size_t i) -> float
    {
        if (!dither)
            return 0.0f;
        const uint32_t px = static_cast<uint32_t>(i % width);
        const uint32_t py = static_cast<uint32_t>(i / width);
        return TriangularDitherAt(px, py) * kDitherLsb;
    };

    float scale = (rangeMax > rangeMin) ? 1.0f / (rangeMax - rangeMin) : 1.0f;

    if (fmt == TF::RGBA8_UNORM || fmt == TF::RGBA8_SRGB)
    {
        std::memcpy(rgba.data(), raw, pixelCount * 4);
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::BGRA8_UNORM || fmt == TF::BGRA8_SRGB)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            rgba[i * 4 + 0] = src[i * 4 + 2]; // B→R
            rgba[i * 4 + 1] = src[i * 4 + 1]; // G→G
            rgba[i * 4 + 2] = src[i * 4 + 0]; // R→B
            rgba[i * 4 + 3] = src[i * 4 + 3]; // A→A
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::D32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float v = (src[i] - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::D24_UNORM_S8_UINT || fmt == TF::X8_D24_UNORM_PACK32)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float depth = static_cast<float>(src[i] & 0x00FFFFFFu) / 16777215.0f;
            float v = (depth - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::D16_UNORM)
    {
        auto* src = static_cast<const uint16_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float depth = static_cast<float>(src[i]) / 65535.0f;
            float v = (depth - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R16G16B16A16_FLOAT)
    {
        // half-float: use bit manipulation to convert to float
        auto halfToFloat = [](uint16_t h) -> float
        {
            uint32_t sign = (h >> 15) & 1;
            uint32_t exp = (h >> 10) & 0x1F;
            uint32_t mantissa = h & 0x3FF;
            if (exp == 0)
                return sign ? -0.0f : 0.0f;
            if (exp == 31)
                return sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
            float f = std::ldexp(static_cast<float>(mantissa) / 1024.0f + 1.0f, static_cast<int>(exp) - 15);
            return sign ? -f : f;
        };

        auto* src = static_cast<const uint16_t*>(raw);
        std::vector<float> plane(pixelCount * 3);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            plane[i * 3 + 0] = (halfToFloat(src[i * 4 + 0]) - rangeMin) * scale;
            plane[i * 3 + 1] = (halfToFloat(src[i * 4 + 1]) - rangeMin) * scale;
            plane[i * 3 + 2] = (halfToFloat(src[i * 4 + 2]) - rangeMin) * scale;
        }
        EncodeDitherQuantizePlane(plane, width, height, srgbEncode, dither, deband, filterState, rgba);
        return rgba;
    }

    if (fmt == TF::R16_FLOAT)
    {
        auto halfToFloat = [](uint16_t h) -> float
        {
            uint32_t sign = (h >> 15) & 1;
            uint32_t exp = (h >> 10) & 0x1F;
            uint32_t mantissa = h & 0x3FF;
            if (exp == 0)
                return sign ? -0.0f : 0.0f;
            if (exp == 31)
                return sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
            float f = std::ldexp(static_cast<float>(mantissa) / 1024.0f + 1.0f, static_cast<int>(exp) - 15);
            return sign ? -f : f;
        };

        auto* src = static_cast<const uint16_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float v = (halfToFloat(src[i]) - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const uint8_t g = toU8((src[i] - rangeMin) * scale, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R32G32B32A32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        std::vector<float> plane(pixelCount * 3);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            plane[i * 3 + 0] = (src[i * 4 + 0] - rangeMin) * scale;
            plane[i * 3 + 1] = (src[i * 4 + 1] - rangeMin) * scale;
            plane[i * 3 + 2] = (src[i * 4 + 2] - rangeMin) * scale;
        }
        EncodeDitherQuantizePlane(plane, width, height, srgbEncode, dither, deband, filterState, rgba);
        return rgba;
    }

    if (fmt == TF::R11G11B10_FLOAT)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            // Unpack R11G11B10 — 11-bit R, 11-bit G, 10-bit B (all unsigned float)
            uint32_t bits = src[i];
            auto unpack11 = [](uint32_t v) -> float
            {
                uint32_t exp = (v >> 6) & 0x1F;
                uint32_t mantissa = v & 0x3F;
                if (exp == 0)
                    return 0.0f;
                if (exp == 31)
                    return 65536.0f;
                return std::ldexp(static_cast<float>(mantissa) / 64.0f + 1.0f, static_cast<int>(exp) - 15);
            };
            auto unpack10 = [](uint32_t v) -> float
            {
                uint32_t exp = (v >> 5) & 0x1F;
                uint32_t mantissa = v & 0x1F;
                if (exp == 0)
                    return 0.0f;
                if (exp == 31)
                    return 65536.0f;
                return std::ldexp(static_cast<float>(mantissa) / 32.0f + 1.0f, static_cast<int>(exp) - 15);
            };
            float r = (unpack11(bits & 0x7FF) - rangeMin) * scale;
            float g = (unpack11((bits >> 11) & 0x7FF) - rangeMin) * scale;
            float b = (unpack10((bits >> 22) & 0x3FF) - rangeMin) * scale;
            const float d = ditherAt(i);
            rgba[i * 4 + 0] = toU8(r, d);
            rgba[i * 4 + 1] = toU8(g, d);
            rgba[i * 4 + 2] = toU8(b, d);
        }
        return rgba;
    }

    if (fmt == TF::R8_UNORM)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            rgba[i * 4 + 0] = src[i];
            rgba[i * 4 + 1] = src[i];
            rgba[i * 4 + 2] = src[i];
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::RGB10A2_UNORM)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            uint32_t bits = src[i];
            float r = static_cast<float>(bits & 0x3FF) / 1023.0f;
            float g = static_cast<float>((bits >> 10) & 0x3FF) / 1023.0f;
            float b = static_cast<float>((bits >> 20) & 0x3FF) / 1023.0f;
            const float d = ditherAt(i);
            rgba[i * 4 + 0] = toU8(r, d);
            rgba[i * 4 + 1] = toU8(g, d);
            rgba[i * 4 + 2] = toU8(b, d);
        }
        return rgba;
    }

    // Fallback: copy raw bytes as RGBA8 (best effort).
    size_t bpp = Rendering::BytesPerPixel(fmt);
    if (bpp > 0)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        size_t copyBytes = std::min(bpp, size_t(4));
        for (size_t i = 0; i < pixelCount; ++i)
        {
            std::memcpy(&rgba[i * 4], &src[i * bpp], copyBytes);
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
    }
    return rgba;
}

// Crop an RGBA8 buffer to a sub-rectangle. Clamps the rect to the source extents.
// Returns an empty buffer if the rect is fully out of bounds.
static std::vector<uint8_t> CropRGBA8(
    const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
    int32_t x, int32_t y, int32_t w, int32_t h,
    uint32_t& outW, uint32_t& outH)
{
    outW = 0;
    outH = 0;
    if (srcW == 0 || srcH == 0 || w <= 0 || h <= 0)
        return {};

    // Clamp against the source bounds.
    int32_t x0 = std::max(0, x);
    int32_t y0 = std::max(0, y);
    int32_t x1 = std::min<int32_t>(static_cast<int32_t>(srcW), x + w);
    int32_t y1 = std::min<int32_t>(static_cast<int32_t>(srcH), y + h);
    if (x1 <= x0 || y1 <= y0)
        return {};

    const uint32_t cw = static_cast<uint32_t>(x1 - x0);
    const uint32_t ch = static_cast<uint32_t>(y1 - y0);
    std::vector<uint8_t> out(static_cast<size_t>(cw) * ch * 4);
    for (uint32_t row = 0; row < ch; ++row)
    {
        const uint8_t* srcRow = src.data() + (static_cast<size_t>(y0 + row) * srcW + x0) * 4;
        uint8_t* dstRow = out.data() + static_cast<size_t>(row) * cw * 4;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(cw) * 4);
    }
    outW = cw;
    outH = ch;
    return out;
}

// Base64 encoder for binary data (used by take_screenshot to return pixel data).
static std::string Base64Encode(const uint8_t* data, size_t length)
{
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    result.reserve(((length + 2) / 3) * 4);

    for (size_t i = 0; i < length; i += 3)
    {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < length)
            n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < length)
            n |= static_cast<uint32_t>(data[i + 2]);

        result.push_back(kTable[(n >> 18) & 0x3F]);
        result.push_back(kTable[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < length) ? kTable[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < length) ? kTable[n & 0x3F] : '=');
    }
    return result;
}

// ---------------------------------------------------------------------------
// capture_resource support
// ---------------------------------------------------------------------------

// Base64 of the whole buffer travels over the debug-server socket; a multi-MB
// GPUScene buffer would stall the channel for a diagnostic.
static constexpr uint64_t kMaxBufferCaptureBytes = 1ull << 20;
// Decoded arrays are for eyeballing values, not bulk transfer.
static constexpr size_t kMaxDecodedElements = 4096;
// Raw texel captures ship bytes verbatim over the same socket.
static constexpr uint64_t kMaxRawCaptureBytes = 4ull << 20;
// A frame declares hundreds of resources; a miss reports a bounded sample.
static constexpr size_t kMaxReportedCandidates = 64;

// Reported in the capture response so a caller can tell a 1-channel depth map
// from a 4-channel colour target without parsing the format name.
static uint32_t FormatChannelCount(Rendering::TextureFormat fmt)
{
    using TF = Rendering::TextureFormat;
    switch (fmt)
    {
    case TF::R8_UNORM:
    case TF::R8_UINT:
    case TF::R8_SINT:
    case TF::R16_FLOAT:
    case TF::R16_UINT:
    case TF::R16_SINT:
    case TF::R32_FLOAT:
    case TF::R32_UINT:
    case TF::R32_SINT:
    case TF::D32_FLOAT:
    case TF::D16_UNORM:
    case TF::S8_UINT:
    case TF::X8_D24_UNORM_PACK32:
        return 1;
    case TF::R8G8_UNORM:
    case TF::R8G8_UINT:
    case TF::R8G8_SINT:
    case TF::R16G16_FLOAT:
    case TF::R16G16_UINT:
    case TF::R16G16_SINT:
    case TF::R32G32_FLOAT:
    case TF::R32G32_UINT:
    case TF::R32G32_SINT:
    case TF::D24_UNORM_S8_UINT:
    case TF::D32_SFLOAT_S8_UINT:
        return 2;
    case TF::R32G32B32_UINT:
    case TF::R32G32B32_SINT:
    case TF::R11G11B10_FLOAT:
        return 3;
    default:
        return 4;
    }
}

// A resource declared in the frame under inspection.
struct CaptureResourceMatch
{
    uint32_t Id = 0xFFFFFFFFu;
    bool IsTexture = true;
    std::string Name;
};

// Resolve a capture_resource `name` against the resources declared in `frame`.
//
// An exact name wins outright. Otherwise a case-insensitive substring match is
// accepted only when it is UNIQUE — a debug tool that silently picks one of
// several "SceneView.Color"-ish targets would attribute the wrong pixels, which
// is worse than failing. Both failure modes fill `outCandidates` so the caller
// can report what the frame actually declared instead of a bare "not found".
static bool ResolveCaptureResource(const Rendering::RenderGraph::RGFrame& frame,
                                   const std::string& requested,
                                   CaptureResourceMatch& outMatch,
                                   std::vector<std::string>& outCandidates)
{
    namespace RG = Rendering::RenderGraph;
    const auto& graph = frame.Graph();
    const size_t count = graph.ResourceCount();

    auto lower = [](std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string needle = lower(requested);

    std::vector<CaptureResourceMatch> partial;
    for (uint32_t id = 0; id < count; ++id)
    {
        const auto& desc = graph.ResourceDesc(id);
        if (!desc.Name)
            continue;
        if (desc.Kind == RG::RGResourceKind::AccelerationStructure)
            continue; // no bytes the graph owns to capture
        const bool isTexture = desc.Kind == RG::RGResourceKind::Texture;
        const std::string name = desc.Name;
        if (name == requested)
        {
            outMatch = CaptureResourceMatch{id, isTexture, name};
            return true;
        }
        if (lower(name).find(needle) != std::string::npos)
            partial.push_back(CaptureResourceMatch{id, isTexture, name});
    }

    if (partial.size() == 1)
    {
        outMatch = partial.front();
        return true;
    }

    // Ambiguous → the near misses are the useful report; empty → everything is.
    const auto& report = partial;
    if (!report.empty())
    {
        for (const auto& m : report)
            outCandidates.push_back(m.Name);
    }
    else
    {
        for (uint32_t id = 0; id < count; ++id)
            if (graph.ResourceDesc(id).Name)
                outCandidates.push_back(graph.ResourceDesc(id).Name);
    }
    return false;
}

// Scene colour is stored at the frame's pre-exposure P (pre_exposure_snapshot.comp). For a
// texture holding it (the pipeline's PreExposedTextures: the view's colour target, the render
// graph's "preExposed" textures, the ones nodes mark), declare a readback of its view's
// ExposureHistory into the same frame, so the capture can report P and return scene-linear
// values; null for any other resource.
static std::shared_ptr<Rendering::RGBufferReadbackTicket> RequestPreExposureReadback(
    Rendering::IDevice* device, Rendering::RenderGraph::RGFrame& frame, uint32_t textureResourceId)
{
    auto* rs = EngineCore::GetInstance().GetRenderServices();
    const auto* pipeline = rs ? rs->Spine().PipelineInstanceForFrame(frame) : nullptr;
    if (!pipeline)
        return nullptr;
    const Rendering::RenderGraph::RGBuffer source = pipeline->FindPreExposureSource(textureResourceId);
    if (!source.IsValid())
        return nullptr;
    return Rendering::RequestBufferReadbackRG(device, frame, source, 0,
                                              Engine::Renderer::ExposureReadbackFeature::kStateBytes,
                                              "CaptureResource.PreExposure");
}

// ---------------------------------------------------------------------------
// take_screenshot support
// ---------------------------------------------------------------------------

struct ScreenshotCropDesc
{
    bool valid = false;
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
    std::string label;
};

// Monotonic per-session sequence: every capture writes a fresh file so a stale
// PNG from an earlier (possibly failed) capture can never masquerade as the
// new one when a caller re-reads a remembered filePath. It restarts at 1 in
// every process, which is why the names live under a process-scoped directory
// (CaptureOutputDirectory) rather than in one shared per-machine folder.
static std::atomic<uint32_t> s_CaptureSequence{0};

// Encode and publish one owned capture. Each request gets its own file because
// completed readbacks can encode concurrently on different workers.
static json EncodeCapturePng(const std::vector<uint8_t>& rgba8, uint32_t width, uint32_t height,
                             const std::string& fileStem)
{
    auto png = Image::EncodePng(rgba8, width, height);
    if (png.empty())
        return Editor::RefuseRequest("PNG encoding failed");

    const std::filesystem::path filePath = Editor::CaptureOutputDirectory() /
        (fileStem + "_" + std::to_string(++s_CaptureSequence) + ".png");
    std::ofstream output(filePath, std::ios::binary);
    output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    output.close();
    if (!output)
        return Editor::RefuseRequest("Capture write failed: " + filePath.string());
    return json{{"filePath", filePath.string()}, {"pngBase64", Base64Encode(png.data(), png.size())}};
}

static json FinishResourceCaptureResult(Rendering::ViewReadbackResult readback,
                                        float rangeMin, float rangeMax, json metadata)
{
    // Named resources have no color-space stamp. Visualize their declared range
    // without a transfer curve; raw captures retain values outside that range.
    auto rgba8 = ConvertToRGBA8(readback.pixels.data(), readback.width, readback.height,
                                readback.format, rangeMin, rangeMax,
                                /*srgbEncode*/ false, /*dither*/ false, /*deband*/ false);
    std::string safeName = metadata.at("resourceName").get<std::string>();
    for (char& character : safeName)
        if (character == '/' || character == '\\' || character == ':' || character == '?' ||
            character == '*' || character == ' ')
            character = '_';
    json result = EncodeCapturePng(rgba8, readback.width, readback.height, "resource_" + safeName);
    if (!Editor::IsRefusal(result))
        result.update(metadata);
    return result;
}

// Crop, encode, persist, and fill the take_screenshot response (either the
// success payload or a refusal). Touches no editor state, so the rendergraph
// path runs it on a job: the encode costs seconds for a full window in DebugFast.
// `method` records which path produced the pixels: "rendergraph" (GPU readback
// of the frame composite) or "printwindow" (OS-compositor picture of the window,
// served only to a request that passed allowWindowCapture).
// `pendingMaterialTextureBinds` records whether the frame's materials had
// finished binding — the same class of self-description as `method`, and the
// one that tells a transient frame from a broken one.
static void FinishScreenshotResult(json& outResult,
                                   std::vector<uint8_t>&& rgba8,
                                   uint32_t srcW, uint32_t srcH,
                                   const ScreenshotCropDesc& crop,
                                   const std::string& target,
                                   const char* method,
                                   size_t pendingMaterialTextureBinds)
{
    uint32_t outW = srcW;
    uint32_t outH = srcH;
    std::vector<uint8_t> finalPixels;
    if (crop.valid)
    {
        finalPixels = CropRGBA8(rgba8, srcW, srcH, crop.x, crop.y, crop.w, crop.h, outW, outH);
        if (finalPixels.empty())
        {
            outResult = Editor::RefuseRequest("Crop produced empty image (rect outside window for target='" + target + "')");
            return;
        }
    }
    else
    {
        finalPixels = std::move(rgba8);
    }

    outResult = EncodeCapturePng(finalPixels, outW, outH, "screenshot_" + target);
    if (Editor::IsRefusal(outResult))
        return;
    outResult.update(json{
        {"width", outW},
        {"height", outH},
        {"sourceWidth", srcW},
        {"sourceHeight", srcH},
        {"target", target},
        {"format", "png"},
        {"method", method},
        {"pendingMaterialTextureBinds", pendingMaterialTextureBinds}
    });
}

void RegisterCaptureDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    // 13. take_screenshot
    // Screenshot via async GPU readback.
    //
    // Targets (parameter "target"):
    //   "window"   — full editor window (UI + scene viewport composited). Default.
    //   "viewport" — scene viewport texture only.
    //   "panel"    — full window readback cropped to the dock panel rect ("panelId").
    //   "element"  — full window readback cropped to a UI element rect ("elementId").
    //   "rect"           — full window readback cropped to a custom rect ("x","y","w","h").
    //   "asset_preview"  — live video preview texture from Asset View (direct GPU readback).
    //
    // For window/panel/element/rect targets we readback the editor's presentation target.
    // asset_preview bypasses the UI composite and reads the video player's GPU texture.
    //
    // The readback is declared into the RenderGraph frame at the right point — after all
    // normal passes are declared but before Execute. We use EnqueueRenderCallback()
    // which fires from the frame driver pre-Execute on the pure RenderGraph path. A deferred
    // poll then checks the ticket's TryGet() each frame until the GPU copy completes.
    server.RegisterHandler("take_screenshot", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        std::string requestId = ctx.id;

        // Parse parameters up-front (before scheduling render callback).
        const std::string target = ctx.params.value("target", std::string("window"));
        const std::string panelId = ctx.params.value("panelId", std::string());
        const std::string elementId = ctx.params.value("elementId", std::string());
        const int32_t rectX = ctx.params.value("x", 0);
        const int32_t rectY = ctx.params.value("y", 0);
        const int32_t rectW = ctx.params.value("w", 0);
        const int32_t rectH = ctx.params.value("h", 0);
        // coords: "logical" (default) treats x/y/w/h as logical UI pixels —
        // matches get_ui_tree / get_panel_tree output and panel/element bounds.
        // "physical" treats them as raw pixel coords on the saved PNG, useful
        // when post-processing a previously-saved screenshot. Only meaningful
        // for target='rect'.
        const std::string coords = ctx.params.value("coords", std::string("logical"));
        // dither: default ON — captures quantize the linear readback with the same
        // TPDF the screen's sRGB encode pass applies, so screenshots match what the
        // eye sees. Harness/golden consumers that diff or hash pixels numerically
        // should pass dither=false for deterministic staircase-quantized output.
        // GE_OUTPUT_DITHER=0 turns the screen's dither off, so it has to turn the
        // mirror's off too — otherwise that A/B silently compares two identically
        // dithered captures. The step stays the capture's own 8-bit LSB (the PNG
        // quantizes at 1/255 in every output mode); only the enable is shared.
        const bool dither =
            ctx.params.value("dither", true) && Rendering::Passes::IsOutputDitherEnabled();
        // deband: default ON — same parity rationale as dither, for the screen's
        // pre-dither gradient deband (also process-gated by GE_DEBAND). Pass
        // deband=false to profile the raw quantization structure of the source.
        const bool deband = ctx.params.value("deband", true);
        // allowWindowCapture: opt in to an OS window capture (PrintWindow) when the
        // render graph cannot hand over the frame. Off by default — a window
        // capture is a picture of what the compositor holds, not the frame the
        // render graph produced, and it is a plausible PNG either way, so serving
        // one unasked turns a rendering failure into evidence the caller trusts.
        // The response states which one it is in `method`.
        const bool allowWindowCapture = ctx.params.value("allowWindowCapture", false);

        // Validate target.
        if (target != "window" && target != "viewport" && target != "panel"
            && target != "element" && target != "rect" && target != "asset_preview")
        {
            return Editor::RefuseRequest("Unknown target '" + target + "'. Use one of: window, viewport, panel, element, rect, asset_preview.");
        }
        if (coords != "logical" && coords != "physical")
            return Editor::RefuseRequest("coords must be 'logical' or 'physical'");
        if (target == "panel" && panelId.empty())
            return Editor::RefuseRequest("target='panel' requires 'panelId'");
        if (target == "element" && elementId.empty())
            return Editor::RefuseRequest("target='element' requires 'elementId'");
        if (target == "rect" && (rectW <= 0 || rectH <= 0))
            return Editor::RefuseRequest("target='rect' requires positive 'w' and 'h'");

        auto cropHolder = std::make_shared<ScreenshotCropDesc>();
        // What the readback pixels hold, stated by whichever declaration arm below
        // schedules the copy. Disengaged means "nobody stated a space" — the
        // conversion refuses rather than guessing from the format, so a new capture
        // target cannot inherit a silent default.
        auto sourceSpaceHolder = std::make_shared<std::optional<UI::UITextureSpace>>();
        // RenderGraph arm (8c-4): ticket readback — declared against the frame driver's
        // published per-frame capture context.
        auto ticketHolder = std::make_shared<std::shared_ptr<Rendering::RGReadbackTicket>>();
        auto assetPreviewPathHolder = std::make_shared<std::string>();
        // Frame the readback was declared on — reported for staleness diagnosis.
        auto frameIndexHolder = std::make_shared<uint64_t>(0);
        // Deband gate + scope snapshotted at readback declaration (right after
        // this frame's encode declared, same render thread) so the deferred
        // conversion mirrors THE frame it read — not whichever window
        // published last while the GPU finished.
        auto encodeFilterStateHolder = std::make_shared<std::optional<CaptureEncodeFilterState>>();

        LOG_DEBUG("Screenshot: request received target={} requestId={}", target, requestId);

        // How long this request keeps asking the render graph for a frame, and
        // what the caller is told when it stops. The attempt below reports into
        // it from the render phase; the poll drives it.
        auto wait = std::make_shared<Editor::ScreenshotCaptureWait>(allowWindowCapture);

        // One render-phase attempt at declaring the readback (pre-Execute on the
        // pure RenderGraph path, where the frame driver publishes the capture
        // context). The poll re-arms it every frame until the wait ends, so a
        // pane that is one frame behind — a scene view catching up on extraction —
        // is a retry rather than a failed capture.
        //
        // At most one attempt is in flight: the poll runs once per tick and the
        // render phase drains the whole callback queue at once, so queuing an
        // attempt per tick would make a starved frame loop spend the whole
        // refusal budget inside a single rendered frame.
        auto attemptQueued = std::make_shared<bool>(false);
        auto attempt = std::make_shared<std::function<void()>>();
        *attempt = [&app, wait, attemptQueued, ticketHolder, cropHolder, sourceSpaceHolder,
                    assetPreviewPathHolder, frameIndexHolder, encodeFilterStateHolder, target,
                    panelId, elementId, rectX, rectY, rectW, rectH, coords]()
        {
            *attemptQueued = false;
            // The poll can end the wait with this attempt still queued, and the
            // request is answered by then: declaring a readback nobody consumes
            // would cost the frame another composite for nothing.
            if (!wait->GiveUpReason().empty())
                return;
            // Every attempt re-declares from scratch: a crop from an earlier
            // frame must never ride along with this frame's pixels.
            *cropHolder = ScreenshotCropDesc{};

            if (app.m_Windows.empty())
            {
                wait->RecordBlocked("the editor has no window");
                return;
            }

            auto* wndCtx = app.m_Windows[0].get();
            if (!wndCtx->renderCtx)
            {
                wait->RecordBlocked("the editor window has no render context");
                return;
            }

            auto* device = wndCtx->renderCtx->GetDevice();
            if (!device)
            {
                wait->RecordBlocked("the editor window has no render device");
                return;
            }

            // 8c-4: RenderGraph capture context, filled by the frame driver right
            // before this callback runs; ids are frame-local and valid only
            // for the published incarnation.
            auto* rg2Frame = wndCtx->rg2Capture.frame;
            const bool rg2 =
                rg2Frame && wndCtx->rg2Capture.frameIndex == rg2Frame->FrameIndex();
            if (!rg2)
            {
                wait->RecordRefusal("the frame published no render-graph capture context");
                return;
            }
            *frameIndexHolder = wndCtx->rg2Capture.frameIndex;

            // ---- ASSET PREVIEW path: readback live video frame texture ---------
            if (target == "asset_preview")
            {
                auto* assetViewPanel = FindPanel<AssetViewPanel>(app.m_PanelStorage);
                if (!assetViewPanel)
                {
                    wait->RecordBlocked("the Asset View panel is not open");
                    return;
                }

                Rendering::TextureHandle previewTexture{};
                Rendering::TextureFormat previewFormat = Rendering::TextureFormat::RGBA8_UNORM;
                if (!assetViewPanel->TryGetVideoPreviewReadbackTexture(previewTexture, previewFormat))
                {
                    wait->RecordBlocked(
                        "the Asset View has no uploaded video frame — select an .mp4 first");
                    return;
                }

                *assetPreviewPathHolder = assetViewPanel->GetPreviewDebugState().previewPath;
                *ticketHolder = Rendering::RequestDeviceTextureReadbackRG(
                    device, *rg2Frame, previewTexture,
                    Rendering::ResourceState::ShaderResource, "AssetViewVideoPreview");
                // Decoded video frames are authored sRGB — the same stamp the Asset
                // View panel registers this texture to the UI with.
                *sourceSpaceHolder = UI::UITextureSpace::SrgbAuthored();
                return;
            }

            // ---- VIEWPORT path: scene-view pipeline-output readback ------------
            if (target == "viewport")
            {
                if (!wndCtx->scene)
                {
                    wait->RecordBlocked("the editor window has no scene view controller");
                    return;
                }
                auto* renderServices = app.m_EditorContext ? app.m_EditorContext->RenderServices : nullptr;
                if (!renderServices)
                {
                    wait->RecordBlocked("the editor has no render services");
                    return;
                }
                // The active scene view may be a maximized non-Perspective quad pane
                // (a secondary controller), not ctx->scene. Read back the controller
                // that's actually rendering the visible viewport, else the readback
                // targets a suspended view and never schedules.
                SceneViewController* activeSceneCtrl = wndCtx->scene.get();
                if (wndCtx->ui)
                    if (auto* root = wndCtx->ui->GetRootElement())
                        if (auto* mount = dynamic_cast<Mount*>(root->FindById(HashStringId(EditorPanelIds::MountSceneView))))
                            if (auto* svPanel = dynamic_cast<SceneViewPanel*>(mount->GetTarget()))
                                if (auto* ac = svPanel->GetSceneControllerForSlot(svPanel->GetActiveViewportSlot()))
                                    activeSceneCtrl = ac;
                Rendering::ViewId viewId = activeSceneCtrl ? activeSceneCtrl->GetViewId() : 0u;
                if (viewId == 0)
                {
                    wait->RecordRefusal("the scene view has no registered view yet");
                    return;
                }
                // The scene view's pipeline output — the texture the UI
                // samples (post-FX FinalColor).
                const auto out = renderServices->GetPipelineOutputRG(*rg2Frame, viewId);
                if (!out.IsValid())
                {
                    // The pane declared no pipeline output this frame: its tab is
                    // behind another, its rect collapsed, or its view is a frame
                    // behind on extraction. The first two do not clear on their
                    // own; the wait tells them apart by how long the refusal lasts.
                    wait->RecordRefusal(
                        "the scene view produced no render-graph output — its tab may be behind "
                        "another, its pane collapsed, or the view is still catching up");
                    return;
                }
                *ticketHolder = Rendering::RequestTextureReadbackRG(
                    device, *rg2Frame, out.Out, "ViewportScreenshot");
                // The pipeline's own stamp for what it wrote into FinalColor —
                // LINEAR, overlays and gizmos included. The view's own finalize
                // is downstream of this point, so a viewport capture does its own
                // encode and its own 8-bit filters rather than inheriting the
                // screen's, which are sized to the display's step. Same content,
                // filtered for the file's depth.
                *sourceSpaceHolder = renderServices->GetPipelineOutputSpaceRG();
                return;
            }

            // ---- WINDOW / PANEL / ELEMENT / RECT: presentation-target readback --
            // Every pure editor frame routes through Editor.FinalLinear, so
            // the full composite is always readable there.
            if (wndCtx->rg2Capture.finalLinearId != 0xFFFFFFFFu)
            {
                Rendering::RenderGraph::RGTexture finalLinear{};
                finalLinear.Id = wndCtx->rg2Capture.finalLinearId;
                *ticketHolder = Rendering::RequestTextureReadbackRG(
                    device, *rg2Frame, finalLinear, "WindowScreenshot");
                // The frame driver publishes the space the composite's bytes
                // hold under the declared UI target (#767/#784): SrgbAuthored
                // on SDR frames (encoded blend — no second OETF at readback),
                // DisplayLinearSdr on HDR-display capture frames.
                *sourceSpaceHolder = wndCtx->rg2Capture.compositeSpace;
                // This window's encode declared just before this callback ran —
                // the publish is exactly the state this readback's pixels see.
                *encodeFilterStateHolder = CaptureEncodeFilterState{
                    Rendering::Passes::GetBackbufferOutputDebandThresholdLsb()};
            }
            else
            {
                wait->RecordRefusal("the frame declared no composite to read back");
                return;
            }

            // Compute crop rect for panel/element/rect targets (image-space crop).
            // GetLayoutX/Y/Width/Height returns logical pixels; the readback is in
            // physical pixels — scale by content scale.
            if (target == "window")
                return;

            float layoutX = 0.0f;
            float layoutY = 0.0f;
            float layoutW = 0.0f;
            float layoutH = 0.0f;
            std::string label;
            bool found = false;

            if (target == "panel")
            {
                if (app.m_Docking)
                {
                    if (UIElement* panel = app.m_Docking->GetPanel(panelId))
                    {
                        layoutX = panel->GetLayoutX();
                        layoutY = panel->GetLayoutY();
                        layoutW = panel->GetLayoutWidth();
                        layoutH = panel->GetLayoutHeight();
                        label = "panel:" + panelId;
                        found = true;
                    }
                }
            }
            else if (target == "element")
            {
                if (wndCtx->ui)
                {
                    if (UIElement* root = wndCtx->ui->GetRootElement())
                    {
                        if (UIElement* el = root->FindById(HashStringId(elementId)))
                        {
                            layoutX = el->GetLayoutX();
                            layoutY = el->GetLayoutY();
                            layoutW = el->GetLayoutWidth();
                            layoutH = el->GetLayoutHeight();
                            label = "element:" + elementId;
                            found = true;
                        }
                    }
                }
            }
            else if (target == "rect")
            {
                layoutX = static_cast<float>(rectX);
                layoutY = static_cast<float>(rectY);
                layoutW = static_cast<float>(rectW);
                layoutH = static_cast<float>(rectH);
                label = "rect";
                found = true;
            }

            if (!found || layoutW <= 0.0f || layoutH <= 0.0f)
            {
                LOG_WARNING("Screenshot: crop target not found or empty — returning full window");
                return;
            }

            // panel/element values come from layout (always logical). For
            // rect, honor the caller's coords choice.
            const bool applyScale = (target != "rect") || (coords == "logical");
            const float scale = applyScale && wndCtx->ui
                                    ? std::max(0.01f, wndCtx->ui->GetContentScale())
                                    : 1.0f;
            cropHolder->valid = true;
            cropHolder->x = static_cast<int32_t>(std::floor(layoutX * scale));
            cropHolder->y = static_cast<int32_t>(std::floor(layoutY * scale));
            cropHolder->w = static_cast<int32_t>(std::ceil(layoutW * scale));
            cropHolder->h = static_cast<int32_t>(std::ceil(layoutH * scale));
            cropHolder->label = std::move(label);
            LOG_DEBUG("Screenshot: crop {} -> ({},{},{},{})",
                      cropHolder->label, cropHolder->x, cropHolder->y, cropHolder->w, cropHolder->h);
        };
        *attemptQueued = true;
        server.EnqueueRenderCallback([attempt]() { (*attempt)(); });

        // Deferred poll: each frame, re-arm the attempt until the readback is
        // declared, then wait for the GPU copy. The wait decides when to stop and
        // says why; a window capture is served only to a caller that asked for
        // one, because it is a picture of the window rather than the frame the
        // render graph produced.
        const auto requestStart = std::chrono::steady_clock::now();
        // Naming the opt-in flag helps only a caller that has not already passed
        // it, and only where an OS window capture can stand in: asset_preview
        // reads a video texture no picture of the window substitutes for.
        const bool offerWindowCaptureHint = !allowWindowCapture && target != "asset_preview";
        // The job that converts, encodes and serializes the finished readback.
        auto encodeHolder = std::make_shared<JobSystem::TaskHandle>();
        server.EnqueueDeferredResponse(requestId,
            [&app, &server, attempt, attemptQueued, wait, ticketHolder, requestStart, cropHolder,
             sourceSpaceHolder,
             assetPreviewPathHolder, frameIndexHolder, encodeFilterStateHolder, encodeHolder, target,
             panelId, elementId, rectX, rectY, rectW, rectH, coords, dither, deband,
             offerWindowCaptureHint, requestId](json& outResult) -> bool
            {
                auto reportFailure = [&](const std::string& reason,
                                         bool offerWindowCapture = false) -> bool
                {
                    LOG_WARNING("Screenshot ({}): {}", target, reason);
                    outResult =
                        Editor::RefuseRequest(Editor::ScreenshotFailureMessage(reason, offerWindowCapture),
                                              json{{"fallbackReason", reason}});
                    return true;
                };

                // The readback has been handed to the encode job: wait for its line.
                if (encodeHolder->IsValid())
                {
                    std::string jobError;
                    if (!Editor::PollHandlerReplyJob(*encodeHolder, outResult, jobError))
                        return false;
                    if (!jobError.empty())
                        return reportFailure("the screenshot encode job did not complete: " + jobError);
                    return true;
                }

                auto windowCapture = [&](const std::string& reason) -> bool
                {
                    // asset_preview reads a video texture no window capture can
                    // substitute for — it fails whatever the caller allowed.
                    if (target == "asset_preview")
                        return reportFailure(reason + "; asset_preview has no window-capture substitute");
                    if (app.m_Windows.empty() || !app.m_Windows[0] || !app.m_Windows[0]->window)
                        return reportFailure(reason + "; no window to capture either");
                    auto* wndCtx = app.m_Windows[0].get();
                    std::vector<uint8_t> rgba8;
                    uint32_t srcW = 0;
                    uint32_t srcH = 0;
                    if (!CaptureWindowClientAreaRGBA8(wndCtx->window.get(), rgba8, srcW, srcH))
                        return reportFailure(reason +
                                             "; the window capture also failed (non-Windows "
                                             "platform, hidden window, or capture error)");
                    LOG_WARNING("Screenshot ({}): {} — served the window capture the request allowed",
                                target, reason);

                    // The render callback that normally fills cropHolder may
                    // never have run on this path — compute the crop from the
                    // live layout instead. viewport degrades to the scene-view
                    // mount's on-screen region of the composite.
                    ScreenshotCropDesc crop;
                    float layoutX = 0.0f;
                    float layoutY = 0.0f;
                    float layoutW = 0.0f;
                    float layoutH = 0.0f;
                    bool found = false;
                    if (target == "panel" && app.m_Docking)
                    {
                        if (UIElement* panel = app.m_Docking->GetPanel(panelId))
                        {
                            layoutX = panel->GetLayoutX();
                            layoutY = panel->GetLayoutY();
                            layoutW = panel->GetLayoutWidth();
                            layoutH = panel->GetLayoutHeight();
                            crop.label = "panel:" + panelId;
                            found = true;
                        }
                    }
                    else if ((target == "element" || target == "viewport") && wndCtx->ui)
                    {
                        if (UIElement* root = wndCtx->ui->GetRootElement())
                        {
                            const std::string lookupId =
                                target == "viewport" ? std::string(EditorPanelIds::MountSceneView)
                                                     : elementId;
                            if (UIElement* el = root->FindById(HashStringId(lookupId)))
                            {
                                layoutX = el->GetLayoutX();
                                layoutY = el->GetLayoutY();
                                layoutW = el->GetLayoutWidth();
                                layoutH = el->GetLayoutHeight();
                                crop.label = "element:" + lookupId;
                                found = true;
                            }
                        }
                    }
                    else if (target == "rect")
                    {
                        layoutX = static_cast<float>(rectX);
                        layoutY = static_cast<float>(rectY);
                        layoutW = static_cast<float>(rectW);
                        layoutH = static_cast<float>(rectH);
                        crop.label = "rect";
                        found = true;
                    }
                    if (found && layoutW > 0.0f && layoutH > 0.0f)
                    {
                        const bool applyScale = (target != "rect") || (coords == "logical");
                        const float scale = applyScale && wndCtx->ui
                                                ? std::max(0.01f, wndCtx->ui->GetContentScale())
                                                : 1.0f;
                        crop.valid = true;
                        crop.x = static_cast<int32_t>(std::floor(layoutX * scale));
                        crop.y = static_cast<int32_t>(std::floor(layoutY * scale));
                        crop.w = static_cast<int32_t>(std::ceil(layoutW * scale));
                        crop.h = static_cast<int32_t>(std::ceil(layoutH * scale));
                    }

                    FinishScreenshotResult(outResult, std::move(rgba8), srcW, srcH, crop, target,
                                           "printwindow", PendingMaterialTextureBinds());
                    outResult["fallbackReason"] = reason;
                    return true;
                };

                Rendering::ViewReadbackResult result;
                bool readbackReady = false;
                // RenderGraph arm: deterministic ticket completion (8c-1). The
                // attempt creates the ticket in the render phase.
                if (*ticketHolder)
                {
                    if ((*ticketHolder)->IsConsumed())
                    {
                        // A cancelled ticket (declaring frame abandoned) can never
                        // resolve — drop it and let a later frame declare again.
                        ticketHolder->reset();
                        wait->RecordRefusal("the frame that declared the readback was abandoned "
                                            "before submit");
                    }
                    else
                    {
                        wait->RecordReadbackDeclared();
                        readbackReady = (*ticketHolder)->TryGet(result);
                    }
                }

                if (!readbackReady)
                {
                    // The same condition the render loop skips a window on: with no
                    // framebuffer there is no frame to declare a readback into, and
                    // no later frame changes that on its own.
                    if (!*ticketHolder && !app.m_Windows.empty() && app.m_Windows[0] &&
                        app.m_Windows[0]->window)
                    {
                        int fbW = 0;
                        int fbH = 0;
                        app.m_Windows[0]->window->GetFramebufferSize(fbW, fbH);
                        if (fbW <= 0 || fbH <= 0)
                            wait->RecordBlocked("the editor window has no framebuffer (minimized "
                                                "or hidden), so it renders no frame to capture");
                    }
                    const double elapsed =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - requestStart)
                            .count();
                    const auto outcome = wait->Step(elapsed);
                    if (outcome == Editor::ScreenshotCaptureWait::Outcome::ServeWindowCapture)
                        return windowCapture(wait->GiveUpReason());
                    if (outcome == Editor::ScreenshotCaptureWait::Outcome::Fail)
                        return reportFailure(wait->GiveUpReason(), offerWindowCaptureHint);
                    // Keep waiting: the next rendered frame gets its own attempt.
                    if (!*ticketHolder && !*attemptQueued)
                    {
                        *attemptQueued = true;
                        server.EnqueueRenderCallback([attempt]() { (*attempt)(); });
                    }
                    return false;
                }

                LOG_DEBUG("Screenshot: readback complete {}x{} fmt={}", result.width, result.height,
                          static_cast<uint32_t>(result.format));

                // The declaring arm states the space; a scheduled ticket without one is
                // a capture target that forgot to, not a licence to infer.
                if (!*sourceSpaceHolder)
                    return reportFailure("the readback declared no source colour space");

                // The frame's share ends with the readback: conversion, PNG encode,
                // file write, base64 and serialization run on a job, and a later poll
                // sends the line it produced.
                *encodeHolder = Editor::SubmitHandlerReplyJob(
                    EngineCore::GetInstance().GetJobSystem(), requestId,
                    [readback = std::move(result), srgbEncode = !UI::IsEncodedAtRest(**sourceSpaceHolder),
                     dither, deband,
                     filterState = encodeFilterStateHolder->value_or(CaptureEncodeFilterState{
                         Rendering::Passes::GetBackbufferOutputDebandThresholdLsb()}),
                     crop = *cropHolder, target,
                     frameIndex = *frameIndexHolder, previewPath = *assetPreviewPathHolder,
                     pendingBinds = PendingMaterialTextureBinds()]() -> json
                    {
                        auto rgba8 = ConvertToRGBA8(readback.pixels.data(), readback.width, readback.height,
                                                    readback.format, 0.0f, 1.0f, srgbEncode, dither, deband,
                                                    &filterState);
                        json reply;
                        FinishScreenshotResult(reply, std::move(rgba8), readback.width, readback.height, crop,
                                               target, "rendergraph", pendingBinds);
                        if (!Editor::IsRefusal(reply))
                        {
                            reply["frameIndex"] = frameIndex;
                            if (target == "asset_preview" && !previewPath.empty())
                                reply["previewPath"] = previewPath;
                        }
                        return reply;
                    });
                if (!encodeHolder->IsValid())
                    return reportFailure("the job system is shutting down; the capture was not encoded");
                return false;
            },
            Editor::kScreenshotPollBackstop);

        return EditorDebugServer::DeferredMarker(); });

    // Shadow/draw-stream readback diagnostics: returns compact hashes only.
    // Intended for backend-specific flicker investigations where screenshots
    // show instability but CPU-side culling/cascade data is stable.
    server.RegisterHandler("capture_shadow_diagnostics", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        const std::string requestId = ctx.id;
        const int requestedViewId = ctx.params.value("viewId", 0);
        const int requestedCascade = ctx.params.value("cascade", -1);
        const uint32_t maxStreams = std::max(1u, ctx.params.value("maxStreams", 8u));
        const uint32_t maxRecords = std::max(1u, ctx.params.value("maxRecords", 64u));

        struct ShadowTicket
        {
            uint32_t cascade = 0;
            std::shared_ptr<Rendering::RGReadbackTicket> ticket;
        };
        struct BufferTicket
        {
            std::string label;
            std::shared_ptr<Rendering::RGBufferReadbackTicket> ticket;
        };
        struct StreamTickets
        {
            uint64_t streamKey = 0;
            uint32_t materialIndex = 0;
            uint32_t meshIndex = 0;
            // The resolved axes the range was published under (depth-class
            // sentinel or color class, and pool group under consolidation).
            uint32_t classKey = 0;
            uint32_t meshKey = 0;
            uint32_t maxDrawCount = 0;
            uint32_t recordsRead = 0;
            BufferTicket count;
            BufferTicket commands;
            BufferTicket indirection;
        };
        struct CaptureState
        {
            Rendering::ViewId viewId = 0;
            uint32_t shadowResolution = 0;
            std::vector<ShadowTicket> shadows;
            std::vector<StreamTickets> streams;
            std::string setupError;
        };
        auto state = std::make_shared<CaptureState>();

        auto fnv64 = [](const uint8_t* data, size_t size) -> uint64_t
        {
            uint64_t hash = 1469598103934665603ull;
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= static_cast<uint64_t>(data[i]);
                hash *= 1099511628211ull;
            }
            return hash;
        };
        (void)fnv64;

        server.EnqueueRenderCallback([&app, state, requestedViewId, requestedCascade, maxStreams, maxRecords]()
        {
            if (app.m_Windows.empty())
            {
                state->setupError = "No editor window";
                return;
            }
            auto* wndCtx = app.m_Windows[0].get();
            auto* renderServices = app.m_EditorContext ? app.m_EditorContext->RenderServices : nullptr;
            if (!renderServices)
            {
                state->setupError = "No RenderServices";
                return;
            }
            auto* device = renderServices->GetDevice();
            if (!device)
            {
                state->setupError = "No render device";
                return;
            }
            auto* rg2Frame = wndCtx->rg2Capture.frame;
            const bool rg2 = rg2Frame && wndCtx->rg2Capture.frameIndex == rg2Frame->FrameIndex();
            if (!rg2)
            {
                state->setupError = "No RenderGraph capture context this frame";
                return;
            }

            Rendering::ViewId viewId = static_cast<Rendering::ViewId>(std::max(0, requestedViewId));
            if (viewId == 0)
            {
                SceneViewController* activeSceneCtrl = wndCtx->scene.get();
                if (wndCtx->ui)
                    if (auto* root = wndCtx->ui->GetRootElement())
                        if (auto* mount = dynamic_cast<Mount*>(root->FindById(HashStringId(EditorPanelIds::MountSceneView))))
                            if (auto* svPanel = dynamic_cast<SceneViewPanel*>(mount->GetTarget()))
                                if (auto* ac = svPanel->GetSceneControllerForSlot(svPanel->GetActiveViewportSlot()))
                                    activeSceneCtrl = ac;
                viewId = activeSceneCtrl ? activeSceneCtrl->GetViewId() : 0u;
            }
            if (viewId == 0)
            {
                state->setupError = "Invalid viewId";
                return;
            }
            state->viewId = viewId;

            const auto shadowArr = renderServices->GetShadowMapArrayRG(*rg2Frame, viewId);
            if (shadowArr.IsValid())
            {
                const auto& desc = rg2Frame->Graph().ResourceDesc(shadowArr.Id);
                state->shadowResolution = desc.Width;
                const uint32_t layers = std::max(1u, desc.ArrayLayers);
                const uint32_t firstCascade =
                    requestedCascade >= 0 ? static_cast<uint32_t>(requestedCascade) : 0u;
                const uint32_t endCascade =
                    requestedCascade >= 0 ? std::min(layers, firstCascade + 1u) : layers;
                for (uint32_t c = firstCascade; c < endCascade; ++c)
                {
                    ShadowTicket st{};
                    st.cascade = c;
                    st.ticket = Rendering::RequestTextureSubresourceReadbackRG(
                        device, *rg2Frame, shadowArr, 0, c, 0, 0, 0, 0, "ShadowCascadeDiag");
                    if (st.ticket)
                        state->shadows.push_back(std::move(st));
                }
            }

            auto* drawStream = renderServices->GetDrawStreamBuilder();
            const auto& frameRG = renderServices->FrameRG();
            const bool orderingValid =
                frameRG.For.IsFor(*rg2Frame) &&
                frameRG.DrawStreamOrdering.IsValid();
            if (drawStream && orderingValid)
            {
                const auto keys = renderServices->GetEntityBatchKeys(viewId);
                const uint8_t cascadeIdx =
                    requestedCascade >= 0
                        ? static_cast<uint8_t>(requestedCascade)
                        : 0u;
                // Same resolved axes the shadow recorder looks up on: the
                // shared-depth class sentinel for eligible casters, and the
                // pool group under draw consolidation. Both merges are
                // many-keys-to-one-range, so keys that land on a range already
                // sampled are skipped rather than spending a readback slot.
                const std::span<const uint32_t> meshPoolGroups =
                    renderServices->MeshPoolGroupSpanForDraws();
                std::unordered_set<uint64_t> sampledStreams;
                uint32_t emitted = 0;
                for (const auto& key : keys)
                {
                    if (emitted >= maxStreams)
                        break;
                    const Engine::Renderer::DrawStreamLookupKey lookup =
                        Engine::Renderer::ResolveDrawStreamLookupKey(
                            key, cascadeIdx,
                            renderServices->Materials().GetMaterialDepthClass(key.materialIndex),
                            meshPoolGroups);
                    const uint64_t streamKey = Rendering::GPUDrawStreamBuilder::MakeStreamKey(
                        static_cast<uint32_t>(viewId), cascadeIdx, lookup.classKey, lookup.meshKey,
                        Rendering::GPUDrawStreamBuilder::SlicePhase::A);
                    if (!sampledStreams.insert(streamKey).second)
                        continue;
                    const auto range = drawStream->FindBatchDrawRange(
                        static_cast<uint32_t>(viewId), cascadeIdx, lookup.classKey, lookup.meshKey,
                        Rendering::GPUDrawStreamBuilder::SlicePhase::A);
                    if (!range.IsValid())
                        continue;

                    const uint32_t records =
                        std::min(maxRecords, std::max(1u, range.even.maxDrawCount));
                    StreamTickets stream{};
                    stream.streamKey = streamKey;
                    stream.materialIndex = key.materialIndex;
                    stream.meshIndex = key.meshIndex;
                    stream.classKey = lookup.classKey;
                    stream.meshKey = lookup.meshKey;
                    stream.maxDrawCount = range.even.maxDrawCount;
                    stream.recordsRead = records;

                    const std::string prefix =
                        "ShadowStreamDiag." + std::to_string(static_cast<uint32_t>(viewId)) +
                        ".c" + std::to_string(static_cast<uint32_t>(cascadeIdx)) +
                        ".m" + std::to_string(lookup.meshKey) +
                        ".mat" + std::to_string(lookup.classKey);
                    // Shared arena buffers: read this batch's region at the
                    // range's byte offsets. Indirection region starts at the
                    // same record index (one uint per record).
                    const size_t indirByteOffset =
                        range.even.cmdByteOffset / (5u * sizeof(uint32_t)) * sizeof(uint32_t);
                    const auto cmdRG = rg2Frame->ImportExternalBuffer("ShadowStreamDiag.Cmd", range.recordBuffer);
                    const auto countRG = rg2Frame->ImportExternalBuffer("ShadowStreamDiag.Count", range.countBuffer);
                    const auto indirRG = rg2Frame->ImportExternalBuffer(
                        "ShadowStreamDiag.Indirection", drawStream->GetSharedIndirectionBuffer());

                    stream.commands.label = "commands";
                    stream.commands.ticket = Rendering::RequestBufferReadbackRG(
                        device, *rg2Frame, cmdRG, range.even.cmdByteOffset,
                        static_cast<size_t>(records) * 5u * sizeof(uint32_t),
                        (prefix + ".cmd").c_str());
                    stream.count.label = "count";
                    stream.count.ticket = Rendering::RequestBufferReadbackRG(
                        device, *rg2Frame, countRG, range.even.countByteOffset, sizeof(uint32_t),
                        (prefix + ".count").c_str());
                    stream.indirection.label = "indirection";
                    stream.indirection.ticket = Rendering::RequestBufferReadbackRG(
                        device, *rg2Frame, indirRG, indirByteOffset,
                        static_cast<size_t>(records) * sizeof(uint32_t),
                        (prefix + ".indir").c_str());
                    state->streams.push_back(std::move(stream));
                    ++emitted;
                }
            }
        });

        auto pollCount = std::make_shared<int>(0);
        server.EnqueueDeferredResponse(requestId,
            [state, pollCount, fnv64](json& outResult) -> bool
            {
                ++(*pollCount);
                if (!state->setupError.empty())
                {
                    outResult = Editor::RefuseRequest(state->setupError);
                    return true;
                }
                if (state->shadows.empty() && state->streams.empty())
                {
                    if (*pollCount > 5)
                    {
                        outResult = Editor::RefuseRequest("No shadow or stream readbacks were scheduled");
                        return true;
                    }
                    return false;
                }

                std::vector<Rendering::ViewReadbackResult> shadowResults;
                shadowResults.resize(state->shadows.size());
                for (size_t i = 0; i < state->shadows.size(); ++i)
                {
                    if (!state->shadows[i].ticket)
                        continue;
                    if (state->shadows[i].ticket->IsConsumed())
                    {
                        outResult = Editor::RefuseRequest("A shadow readback ticket was cancelled");
                        return true;
                    }
                    if (!state->shadows[i].ticket->TryGet(shadowResults[i]))
                        return false;
                }

                struct StreamReadback
                {
                    Rendering::BufferReadbackResult count;
                    Rendering::BufferReadbackResult commands;
                    Rendering::BufferReadbackResult indirection;
                };
                std::vector<StreamReadback> streamResults;
                streamResults.resize(state->streams.size());
                auto tryBuffer = [&](const BufferTicket& ticket, Rendering::BufferReadbackResult& out) -> bool
                {
                    if (!ticket.ticket)
                        return true;
                    if (ticket.ticket->IsConsumed())
                    {
                        outResult = Editor::RefuseRequest("A buffer readback ticket was cancelled");
                        return false;
                    }
                    return ticket.ticket->TryGet(out);
                };
                for (size_t i = 0; i < state->streams.size(); ++i)
                {
                    if (!tryBuffer(state->streams[i].count, streamResults[i].count))
                        return false;
                    if (!tryBuffer(state->streams[i].commands, streamResults[i].commands))
                        return false;
                    if (!tryBuffer(state->streams[i].indirection, streamResults[i].indirection))
                        return false;
                }

                json shadows = json::array();
                for (size_t i = 0; i < state->shadows.size(); ++i)
                {
                    const auto& rb = shadowResults[i];
                    float minDepth = std::numeric_limits<float>::infinity();
                    float maxDepth = -std::numeric_limits<float>::infinity();
                    uint64_t nonZero = 0;
                    if (rb.format == Rendering::TextureFormat::D32_FLOAT && !rb.pixels.empty())
                    {
                        const size_t count = rb.pixels.size() / sizeof(float);
                        const float* values = reinterpret_cast<const float*>(rb.pixels.data());
                        for (size_t n = 0; n < count; ++n)
                        {
                            minDepth = std::min(minDepth, values[n]);
                            maxDepth = std::max(maxDepth, values[n]);
                            if (values[n] != 0.0f)
                                ++nonZero;
                        }
                    }
                    shadows.push_back({
                        {"cascade", state->shadows[i].cascade},
                        {"width", rb.width},
                        {"height", rb.height},
                        {"format", static_cast<uint32_t>(rb.format)},
                        {"byteCount", static_cast<uint64_t>(rb.pixels.size())},
                        {"hash", fnv64(rb.pixels.data(), rb.pixels.size())},
                        {"minDepth", std::isfinite(minDepth) ? minDepth : 0.0f},
                        {"maxDepth", std::isfinite(maxDepth) ? maxDepth : 0.0f},
                        {"nonZeroDepthSamples", nonZero}
                    });
                }

                json streams = json::array();
                for (size_t i = 0; i < state->streams.size(); ++i)
                {
                    const auto& src = state->streams[i];
                    const auto& rb = streamResults[i];
                    uint32_t drawCount = 0;
                    if (rb.count.bytes.size() >= sizeof(uint32_t))
                        std::memcpy(&drawCount, rb.count.bytes.data(), sizeof(uint32_t));

                    json commands = json::array();
                    const size_t commandCount = rb.commands.bytes.size() / (5u * sizeof(uint32_t));
                    const size_t previewCount = std::min<size_t>(commandCount, 8u);
                    const uint32_t* words = reinterpret_cast<const uint32_t*>(rb.commands.bytes.data());
                    for (size_t c = 0; c < previewCount; ++c)
                    {
                        const uint32_t* cmd = words + c * 5u;
                        int32_t vertexOffset = 0;
                        std::memcpy(&vertexOffset, &cmd[3], sizeof(int32_t));
                        commands.push_back({
                            {"indexCount", cmd[0]},
                            {"instanceCount", cmd[1]},
                            {"firstIndex", cmd[2]},
                            {"vertexOffset", vertexOffset},
                            {"firstInstance", cmd[4]}
                        });
                    }

                    json indirection = json::array();
                    const size_t indirCount = rb.indirection.bytes.size() / sizeof(uint32_t);
                    const size_t indirPreview = std::min<size_t>(indirCount, 16u);
                    const uint32_t* indir = reinterpret_cast<const uint32_t*>(rb.indirection.bytes.data());
                    for (size_t n = 0; n < indirPreview; ++n)
                        indirection.push_back(indir[n]);

                    streams.push_back({
                        {"streamKey", src.streamKey},
                        {"materialIndex", src.materialIndex},
                        {"meshIndex", src.meshIndex},
                        {"drawStreamClassKey", src.classKey},
                        {"drawStreamMeshKey", src.meshKey},
                        {"drawCount", drawCount},
                        {"maxDrawCount", src.maxDrawCount},
                        {"recordsRead", src.recordsRead},
                        {"countHash", fnv64(rb.count.bytes.data(), rb.count.bytes.size())},
                        {"commandHash", fnv64(rb.commands.bytes.data(), rb.commands.bytes.size())},
                        {"indirectionHash", fnv64(rb.indirection.bytes.data(), rb.indirection.bytes.size())},
                        {"commands", std::move(commands)},
                        {"indirection", std::move(indirection)}
                    });
                }

                outResult = {
                    {"viewId", static_cast<uint32_t>(state->viewId)},
                    {"shadowResolution", state->shadowResolution},
                    {"shadows", std::move(shadows)},
                    {"streams", std::move(streams)}
                };
                return true;
            });

        return EditorDebugServer::DeferredMarker(); });

    // 26. capture_resource
    // Capture a named render-graph resource — a texture as a PNG, a buffer as
    // bytes — from the live immediate-mode frame.
    //
    // The readback is declared into the frame at the same seam take_screenshot
    // uses: RunRenderCallbacks fires after every normal pass is declared and
    // before Execute, so the whole frame's resource table is visible and the
    // capture reads each resource's FINAL contents for the frame.
    //
    // Correctness rests on the declared CopySrc read, not on pass ordering: the
    // RAW edge from the last writer is what sequences the copy after the
    // producer, and PreventCulling (inside the readback helper) keeps a producer
    // alive that nothing else consumes — capturing a resource is what makes it
    // live, so an otherwise-culled target still realizes and runs.
    server.RegisterHandler("capture_resource", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string resourceName = ctx.params.value("name", "");
        if (resourceName.empty())
            return Editor::RefuseRequest("Provide a 'name' parameter (use get_render_graph_resources to list names)");

        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        struct CaptureState
        {
            std::string requestedName;
            std::string resolvedName;
            float rangeMin = 0.0f;
            float rangeMax = 1.0f;
            uint32_t mip = 0;
            uint32_t layer = 0;
            uint64_t bufferOffset = 0;
            uint64_t bufferBytes = 0;
            std::string decode;
            uint32_t srcX = 0, srcY = 0, regionW = 0, regionH = 0;
            // Return the texels as they sit in memory instead of an 8-bit PNG.
            // An HDR intermediate does not survive the PNG path: values outside
            // [rangeMin,rangeMax] CLAMP, so a bright region reads as "identical"
            // in two captures that actually differ. Attribution needs the numbers.
            bool raw = false;

            bool isTexture = true;
            Rendering::TextureFormat format = Rendering::TextureFormat::RGBA8_UNORM;
            uint64_t frameIndex = 0;
            bool writtenThisFrame = false;
            std::shared_ptr<Rendering::RGReadbackTicket> textureTicket;
            std::shared_ptr<Rendering::RGBufferReadbackTicket> bufferTicket;
            // Set for a texture holding pre-exposed scene colour: its view's ExposureHistory.
            std::shared_ptr<Rendering::RGBufferReadbackTicket> preExposureTicket;
            bool preExposed = false;
            json error;       // set by the render callback; ends the poll
            bool resolved = false;
            uint32_t polls = 0;
            JobSystem::TaskHandle encodeJob;
        };
        auto state = std::make_shared<CaptureState>();
        state->requestedName = resourceName;
        state->rangeMin = ctx.params.value("rangeMin", 0.0f);
        state->rangeMax = ctx.params.value("rangeMax", 1.0f);
        state->mip = ctx.params.value("mip", 0u);
        state->layer = ctx.params.value("layer", 0u);
        state->bufferOffset = ctx.params.value("offset", 0ull);
        state->bufferBytes = ctx.params.value("byteCount", 0ull);
        state->decode = ctx.params.value("decode", std::string("hex"));
        state->srcX = ctx.params.value("x", 0u);
        state->srcY = ctx.params.value("y", 0u);
        state->regionW = ctx.params.value("w", 0u);
        state->regionH = ctx.params.value("h", 0u);
        state->raw = ctx.params.value("raw", false);

        // Declaration phase: runs on the render thread at the RenderGraph capture seam.
        server.EnqueueRenderCallback([&app, state]()
        {
            if (state->resolved || app.m_Windows.empty())
                return;

            auto* wndCtx = app.m_Windows[0].get();
            auto* device = (wndCtx && wndCtx->renderCtx) ? wndCtx->renderCtx->GetDevice() : nullptr;
            auto* frame = wndCtx ? wndCtx->rg2Capture.frame : nullptr;
            if (!device || !frame || wndCtx->rg2Capture.frameIndex != frame->FrameIndex())
                return; // no live frame this tick — the poll retries on the next one

            state->resolved = true;
            state->frameIndex = frame->FrameIndex();

            CaptureResourceMatch match;
            std::vector<std::string> candidates;
            if (!ResolveCaptureResource(*frame, state->requestedName, match, candidates))
            {
                if (candidates.size() > kMaxReportedCandidates)
                    candidates.resize(kMaxReportedCandidates);
                state->error = Editor::RefuseRequest("No unique render-graph resource matches '" + state->requestedName +
                                                         "' this frame (exact name, else a unique case-insensitive substring)",
                                                     json{{"candidates", candidates}});
                return;
            }
            state->resolvedName = match.Name;
            state->isTexture = match.IsTexture;

            // A persistent/imported resource that NO pass writes this frame still
            // has contents — whatever the last frame that did write it left in the
            // pool. Copying that back succeeds and looks like a fresh capture, so
            // a disabled feature reads as "unchanged" rather than "not produced".
            // Report the distinction instead of letting the caller infer it.
            // Our own readback declares a read only, so this never counts itself.
            state->writtenThisFrame = false;
            for (const auto& a : frame->Graph().Accesses())
            {
                if (a.Resource == match.Id && Rendering::RenderGraph::IsWrite(a.Access))
                {
                    state->writtenThisFrame = true;
                    break;
                }
            }

            const auto& desc = frame->Graph().ResourceDesc(match.Id);

            if (match.IsTexture)
            {
                if (desc.SampleCount > 1)
                {
                    state->error = Editor::RefuseRequest("'" + match.Name + "' is multisampled (" +
                                                             std::to_string(desc.SampleCount) +
                                                             " samples) — copy-to-buffer is invalid on an MSAA image; "
                                                             "capture its resolve target instead",
                                                         json{{"resourceName", match.Name}});
                    return;
                }
                const auto fmt = static_cast<Rendering::TextureFormat>(desc.Format);
                if (Rendering::BytesPerPixel(fmt) == 0)
                {
                    state->error = Editor::RefuseRequest("'" + match.Name + "' has no linear texel size (" +
                                                             std::string(Rendering::ToString(fmt)) +
                                                             ") — block-compressed resources cannot be read back",
                                                         json{{"resourceName", match.Name}});
                    return;
                }
                const uint32_t mipCount = desc.MipLevels > 0 ? desc.MipLevels : 1u;
                const uint32_t layerCount = desc.ArrayLayers > 0 ? desc.ArrayLayers : 1u;
                if (state->mip >= mipCount || state->layer >= layerCount)
                {
                    state->error = Editor::RefuseRequest("subresource out of range for '" + match.Name + "'",
                                                         json{{"resourceName", match.Name},
                                                              {"mipLevels", mipCount},
                                                              {"arrayLayers", layerCount}});
                    return;
                }
                state->format = fmt;

                Rendering::RenderGraph::RGTexture tex = frame->FindTexture(match.Name.c_str());
                if (!tex.IsValid())
                {
                    state->error = Editor::RefuseRequest("'" + match.Name + "' is not a texture this frame");
                    return;
                }
                // A raw capture ships texels verbatim, so it must be a region: a
                // full 3072x902 RGBA16F target is 22 MB of bytes (30 MB base64)
                // and would stall the debug channel.
                if (state->raw)
                {
                    const uint32_t mipW = std::max(1u, desc.Width >> state->mip);
                    const uint32_t mipH = std::max(1u, desc.Height >> state->mip);
                    const uint32_t w = state->regionW != 0 ? state->regionW : mipW;
                    const uint32_t h = state->regionH != 0 ? state->regionH : mipH;
                    const uint64_t bytes = static_cast<uint64_t>(w) * h * Rendering::BytesPerPixel(fmt);
                    if (bytes > kMaxRawCaptureBytes)
                    {
                        state->error = Editor::RefuseRequest("raw capture of '" + match.Name + "' would be " +
                                                                 std::to_string(bytes) + " bytes (limit " +
                                                                 std::to_string(kMaxRawCaptureBytes) +
                                                                 ") — narrow it with x/y/w/h",
                                                             json{{"resourceName", match.Name},
                                                                  {"width", mipW},
                                                                  {"height", mipH},
                                                                  {"bytesPerPixel", Rendering::BytesPerPixel(fmt)}});
                        return;
                    }
                }

                state->textureTicket = Rendering::RequestTextureSubresourceReadbackRG(
                    device, *frame, tex, state->mip, state->layer, state->srcX, state->srcY,
                    state->regionW, state->regionH, match.Name.c_str());
                if (!state->textureTicket)
                {
                    state->error = Editor::RefuseRequest("Failed to declare a readback of '" + match.Name +
                                                             "' (region out of bounds?)",
                                                         json{{"resourceName", match.Name}});
                    return;
                }
                state->preExposureTicket = RequestPreExposureReadback(device, *frame, match.Id);
                state->preExposed = state->preExposureTicket != nullptr;
                return;
            }

            // ---- buffer ----
            const uint64_t size = desc.SizeBytes;
            if (size == 0 || state->bufferOffset >= size)
            {
                state->error = Editor::RefuseRequest("'" + match.Name + "' offset " +
                                                         std::to_string(state->bufferOffset) +
                                                         " is outside its " + std::to_string(size) + " bytes",
                                                     json{{"resourceName", match.Name}});
                return;
            }
            uint64_t want = state->bufferBytes != 0 ? state->bufferBytes : size - state->bufferOffset;
            want = std::min(want, size - state->bufferOffset);
            want = std::min(want, kMaxBufferCaptureBytes);
            state->bufferBytes = want;

            Rendering::RenderGraph::RGBuffer buf = frame->FindBuffer(match.Name.c_str());
            if (!buf.IsValid())
            {
                state->error = Editor::RefuseRequest("'" + match.Name + "' is not a buffer this frame");
                return;
            }
            state->bufferTicket = Rendering::RequestBufferReadbackRG(
                device, *frame, buf, static_cast<size_t>(state->bufferOffset),
                static_cast<size_t>(want), match.Name.c_str());
            if (!state->bufferTicket)
                state->error = Editor::RefuseRequest("Failed to declare a readback of '" + match.Name + "'",
                                                     json{{"resourceName", match.Name}});
        });

        // Resolution phase: poll until the declaring frame's copy has completed.
        server.EnqueueDeferredResponse(ctx.id,
            [state, requestId = ctx.id](json& outResult) -> bool
            {
                if (state->encodeJob.IsValid())
                {
                    std::string jobError;
                    if (!Editor::PollHandlerReplyJob(state->encodeJob, outResult, jobError))
                        return false;
                    if (!jobError.empty())
                        outResult = Editor::RefuseRequest("The resource encode job did not complete: " + jobError);
                    return true;
                }
                ++state->polls;
                if (!state->error.is_null())
                {
                    outResult = state->error;
                    return true;
                }
                if (!state->textureTicket && !state->bufferTicket)
                {
                    // The callback needs a live frame; if none arrives the editor
                    // is not driving frames (minimized / paused render loop).
                    constexpr uint32_t kMaxPollsAwaitingFrame = 10;
                    if (state->polls > kMaxPollsAwaitingFrame)
                    {
                        outResult = Editor::RefuseRequest("No render-graph frame was declared while the "
                                                          "capture was pending — is the editor rendering?");
                        return true;
                    }
                    return false;
                }

                constexpr uint32_t kMaxPollsAwaitingGpu = 25;

                if (state->bufferTicket)
                {
                    if (state->bufferTicket->IsConsumed())
                    {
                        outResult = Editor::RefuseRequest("Declaring frame was abandoned before submit");
                        return true;
                    }
                    Rendering::BufferReadbackResult r;
                    if (!state->bufferTicket->TryGet(r))
                    {
                        if (state->polls > kMaxPollsAwaitingGpu)
                        {
                            outResult = Editor::RefuseRequest("Buffer readback never completed");
                            return true;
                        }
                        return false;
                    }
                    outResult = json{
                        {"resourceName", state->resolvedName},
                        {"requestedName", state->requestedName},
                        {"kind", "buffer"},
                        {"frameIndex", state->frameIndex},
                        {"offset", state->bufferOffset},
                        {"byteCount", r.bytes.size()},
                        {"writtenThisFrame", state->writtenThisFrame},
                        {"bytesBase64", Base64Encode(r.bytes.data(), r.bytes.size())}};
                    if (!state->writtenThisFrame)
                        outResult["staleWarning"] =
                            "No pass wrote '" + state->resolvedName +
                            "' this frame — these bytes are whatever the last frame that "
                            "did write it left in the pool, not a current value";
                    if (state->decode == "float")
                    {
                        std::vector<float> v(std::min(r.bytes.size() / sizeof(float),
                                                      kMaxDecodedElements));
                        std::memcpy(v.data(), r.bytes.data(), v.size() * sizeof(float));
                        outResult["floats"] = v;
                    }
                    else if (state->decode == "uint")
                    {
                        std::vector<uint32_t> v(std::min(r.bytes.size() / sizeof(uint32_t),
                                                         kMaxDecodedElements));
                        std::memcpy(v.data(), r.bytes.data(), v.size() * sizeof(uint32_t));
                        outResult["uints"] = v;
                    }
                    return true;
                }

                if (state->textureTicket->IsConsumed())
                {
                    outResult = Editor::RefuseRequest("Declaring frame was abandoned before submit");
                    return true;
                }
                // Both copies were declared into the same frame; take the pre-exposure first so
                // the texture's result is consumed exactly once.
                float preExposure = 1.0f;
                if (state->preExposureTicket)
                {
                    if (state->preExposureTicket->IsConsumed())
                    {
                        outResult = Editor::RefuseRequest("Declaring frame was abandoned before submit");
                        return true;
                    }
                    Rendering::BufferReadbackResult exposureBytes;
                    if (!state->preExposureTicket->TryGet(exposureBytes))
                    {
                        if (state->polls > kMaxPollsAwaitingGpu)
                        {
                            outResult = Editor::RefuseRequest("Pre-exposure readback never completed");
                            return true;
                        }
                        return false;
                    }
                    Engine::Renderer::ExposureReadbackFeature::State exposureState{};
                    if (exposureBytes.bytes.size() >= sizeof(exposureState))
                        std::memcpy(&exposureState, exposureBytes.bytes.data(), sizeof(exposureState));
                    preExposure = Engine::Renderer::ExposureReadbackFeature::ResolvePreExposure(exposureState);
                    state->preExposureTicket.reset();
                }

                Rendering::ViewReadbackResult result;
                if (!state->textureTicket->TryGet(result))
                {
                    if (state->polls > kMaxPollsAwaitingGpu)
                    {
                        outResult = Editor::RefuseRequest("Texture readback never completed");
                        return true;
                    }
                    return false;
                }
                // preExposed: the texels hold scene colour stored at preExposure. The PNG maps
                // scene-linear values (the range is applied to texel / preExposure); raw texels
                // are returned as they sit in memory, so a consumer divides them by preExposure.
                const bool preExposed = state->preExposed;

                if (state->raw)
                {
                    outResult = json{
                        {"resourceName", state->resolvedName},
                        {"requestedName", state->requestedName},
                        {"kind", "texture"},
                        {"encoding", "raw"},
                        {"width", result.width},
                        {"height", result.height},
                        {"x", state->srcX},
                        {"y", state->srcY},
                        {"format", Rendering::ToString(result.format)},
                        {"bytesPerPixel", Rendering::BytesPerPixel(result.format)},
                        {"channels", FormatChannelCount(result.format)},
                        {"mip", state->mip},
                        {"layer", state->layer},
                        {"frameIndex", state->frameIndex},
                        {"writtenThisFrame", state->writtenThisFrame},
                        {"preExposed", preExposed},
                        {"preExposure", preExposure},
                        {"byteCount", result.pixels.size()},
                        {"rawBase64", Base64Encode(result.pixels.data(), result.pixels.size())}};
                    if (!state->writtenThisFrame)
                        outResult["staleWarning"] =
                            "No pass wrote '" + state->resolvedName +
                            "' this frame — these texels are whatever the last frame that "
                            "did write it left in the pool, not a current image";
                    return true;
                }

                json metadata{
                    {"resourceName", state->resolvedName},
                    {"requestedName", state->requestedName},
                    {"kind", "texture"},
                    {"width", result.width},
                    {"height", result.height},
                    {"format", Rendering::ToString(result.format)},
                    {"channels", FormatChannelCount(result.format)},
                    {"mip", state->mip},
                    {"layer", state->layer},
                    {"frameIndex", state->frameIndex},
                    {"writtenThisFrame", state->writtenThisFrame},
                    {"preExposed", preExposed},
                    {"preExposure", preExposure}};
                if (!state->writtenThisFrame)
                    metadata["staleWarning"] =
                        "No pass wrote '" + state->resolvedName +
                        "' this frame — these pixels are whatever the last frame that "
                        "did write it left in the pool, not a current image";
                state->encodeJob = Editor::SubmitHandlerReplyJob(
                    EngineCore::GetInstance().GetJobSystem(), requestId,
                    [readback = std::move(result), rangeMin = state->rangeMin * preExposure,
                     rangeMax = state->rangeMax * preExposure, metadata = std::move(metadata)]() mutable
                    {
                        return FinishResourceCaptureResult(std::move(readback), rangeMin, rangeMax,
                                                           std::move(metadata));
                    });
                if (!state->encodeJob.IsValid())
                {
                    outResult = Editor::RefuseRequest(
                        "The job system is shutting down; the resource capture was not encoded");
                    return true;
                }
                return false;
            }, Editor::kScreenshotPollBackstop);

        return EditorDebugServer::DeferredMarker(); });

    // trigger_capture — RenderDoc frame capture via in-app API.
    // Resolves RenderDoc lazily through app so handlers only need to be
    // registered once (RenderDoc is initialized after first few frames).
    server.RegisterHandler("trigger_capture", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rd = app.m_RenderDoc.get();
        if (!rd || !rd->IsAvailable())
            return Editor::RefuseRequest("RenderDoc not available — the editor is not running under "
                                         "RenderDoc. Relaunch it with launch_editor renderdoc=true, or "
                                         "under 'renderdoccmd capture'.");

        uint32_t countBefore = rd->GetCaptureCount();

        rd->TriggerCapture();

        // RenderDoc publishes the capture only once the .rdc is fully written.
        // The render loop stalls while it serializes, so this costs far fewer
        // frames than the wall clock suggests — a measured 133 MB capture took
        // ~2.4 s yet consumed only tens of frames, landing right on the 30-frame
        // default and timing out intermittently. get_capture_status recovers the
        // path when the budget does run out.
        constexpr int kCapturePollBudgetFrames = 300;

        // The capture completes after the next present. Poll for the new
        // capture file over several frames via the deferred response mechanism.
        auto state = std::make_shared<uint32_t>(countBefore);
        server.EnqueueDeferredResponse(ctx.id,
            [&app, state](json& outResult) -> bool
            {
                auto* rd = app.m_RenderDoc.get();
                if (!rd)
                    return false;
                uint32_t current = rd->GetCaptureCount();
                if (current <= *state)
                    return false; // not ready yet

                std::string path = rd->GetLastCapturePath();
                outResult = {
                    {"captured", true},
                    {"captureIndex", current - 1},
                    {"filePath", path}
                };
                return true;
            },
            kCapturePollBudgetFrames);

        return EditorDebugServer::DeferredMarker(); });

    // get_capture_status — RenderDoc availability plus the newest capture on
    // disk. Answers on the calling frame, so it recovers the .rdc path when a
    // trigger_capture call timed out before RenderDoc finished writing.
    server.RegisterHandler("get_capture_status", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        auto* rd = app.m_RenderDoc.get();
        const bool available = rd && rd->IsAvailable();
        return json{
            {"available", available},
            {"captureCount", available ? rd->GetCaptureCount() : 0u},
            {"lastCapturePath", available ? rd->GetLastCapturePath() : std::string{}}
        }; });

    // get_pass_descriptors — capture descriptor set bindings at draw time for a named pass.
    server.RegisterHandler("get_pass_descriptors", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        std::string passName = ctx.params.value("passName", "");
        if (passName.empty())
            return Editor::RefuseRequest("Provide a 'passName' parameter");

        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        auto* wndCtx = app.m_Windows[0].get();
        auto* device = (wndCtx && wndCtx->renderCtx) ? wndCtx->renderCtx->GetDevice() : nullptr;
        if (!device)
            return Editor::RefuseRequest("No render device");

        // Enable capture for the next frame, filtered to the requested pass.
        device->DebugClearDescriptorCaptures();
        device->DebugSetDescriptorCaptureEnabled(true, passName);

        std::string requestId = ctx.id;
        auto frameCounter = std::make_shared<int>(0);

        server.EnqueueDeferredResponse(requestId,
            [device, passName, frameCounter](json& outResult) -> bool
            {
                ++(*frameCounter);
                // Wait one full frame for the capture to execute.
                if (*frameCounter < 2)
                    return false;

                // Disable capture.
                device->DebugSetDescriptorCaptureEnabled(false, "");

                auto captures = device->DebugGetDescriptorCaptures();

                json draws = json::array();
                for (const auto& entry : captures)
                {
                    json bindings = json::array();
                    for (const auto& b : entry.Bindings)
                    {
                        json bj;
                        bj["binding"] = b.Binding;
                        bj["resourceType"] = b.ResourceType;
                        bj["resourceHandle"] = "0x" + ([](uint64_t v) {
                            char buf[20];
                            snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
                            return std::string(buf);
                        })(b.ResourceHandle);
                        if (!b.DebugName.empty())
                            bj["debugName"] = b.DebugName;
                        bindings.push_back(std::move(bj));
                    }

                    json drawEntry;
                    drawEntry["passName"] = entry.PassName;
                    drawEntry["drawIndex"] = entry.DrawIndex;
                    drawEntry["descriptorSet"] = "0x" + ([](uint64_t v) {
                        char buf[20];
                        snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
                        return std::string(buf);
                    })(entry.DescriptorSetHandle);
                    drawEntry["setIndex"] = entry.SetIndex;
                    drawEntry["bindings"] = std::move(bindings);
                    draws.push_back(std::move(drawEntry));
                }

                outResult = {
                    {"passName", passName},
                    {"drawCount", draws.size()},
                    {"draws", std::move(draws)}
                };
                return true;
            });

        return EditorDebugServer::DeferredMarker(); });
}

} // namespace GameEngine
