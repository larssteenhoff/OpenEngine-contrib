// GPU pin for the colour-keyed contrast in Shaders/UI/text_mask_gamma.glsl:
// the shader's own level table and its ramp, read back off real renders. The
// CPU goldens (TextMaskGammaGoldenTests) check a mirror of the formula; this
// checks the compiled shader.
//
// Each paint renders twice through the editor's configuration (EncodedSrgb
// attachment, encoded blend target): at contrast 0 the encoded blend weight is
// the raw coverage c, at contrast 1 it is c + k c (1 - c). The paint draws
// over black, so a pixel's weight is its value over the paint's, and k is
// recovered from the partially covered pixels.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"

#include "UI/UITargetSpace.h"

using GameEngine::UITesting::IsolatedUIFixture;
using namespace GameEngine;

namespace
{
constexpr char kXml[] = R"(<uielement id="root">
  <label id="text">llllllllllll</label>
</uielement>)";

constexpr char kCssFmt[] = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; background-color: #000000; }
#text { font-family: Roboto; font-size: 40px; line-height: 60px; color: %s; margin: 20px 20px 20px 20.3px; }
)";

std::vector<uint8_t> RenderAtContrast(const std::string& paint, float contrast, bool& deviceAvailable)
{
    char css[512];
    snprintf(css, sizeof(css), kCssFmt, paint.c_str());
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, css);
    deviceAvailable = fx.DeviceAvailable();
    if (!built)
        return {};
    fx.Manager().SetTextContrast(contrast);
    fx.Manager().SetTextBlendGamma(1.0f);
    return UITesting::RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
}

// Mean k over pixels whose raw weight is in [0.2, 0.8], from the green
// channel (the brightest channel of every paint here).
double RecoveredContrast(const std::vector<uint8_t>& raw, const std::vector<uint8_t>& boosted, int paintGreen, int& samples)
{
    double sum = 0.0;
    samples = 0;
    for (size_t i = 1; i < raw.size(); i += 4)
    {
        const double c = static_cast<double>(raw[i]) / paintGreen;
        if (c < 0.2 || c > 0.8)
            continue;
        const double a = static_cast<double>(boosted[i]) / paintGreen;
        sum += (a - c) / (c * (1.0 - c));
        ++samples;
    }
    return samples > 0 ? sum / samples : 0.0;
}
} // namespace

TEST(TextContrastReadback, ShaderAppliesChromeContrastLevelsAndTheirRamp)
{
    struct Paint
    {
        const char* Css;
        int Green;
        double Contrast;
    };
    // Greys either side of the 63 | 64 and 191 | 192 level edges carry their
    // measured levels; #b7baff has encoded luminance 191.50 / 255, the centre
    // of the ramp across 191 | 192, so it carries the mean of 0.53 and 0.85.
    constexpr Paint kPaints[] = {
        {"#383838", 0x38, -0.14}, {"#404040", 0x40, 0.20}, {"#bebebe", 0xbe, 0.53},
        {"#c0c0c0", 0xc0, 0.85},  {"#b7baff", 0xba, 0.69},
    };
    for (const Paint& paint : kPaints)
    {
        bool deviceAvailable = false;
        const std::vector<uint8_t> raw = RenderAtContrast(paint.Css, 0.0f, deviceAvailable);
        if (!deviceAvailable)
            GTEST_SKIP() << "No Vulkan device available";
        const std::vector<uint8_t> boosted = RenderAtContrast(paint.Css, 1.0f, deviceAvailable);
        ASSERT_EQ(raw.size(), boosted.size());
        ASSERT_FALSE(raw.empty());
        int samples = 0;
        const double k = RecoveredContrast(raw, boosted, paint.Green, samples);
        ASSERT_GE(samples, 50) << paint.Css;
        // 8-bit readback over a 0x38-high paint resolves coverage to ~0.02;
        // the levels sit >= 0.16 apart.
        EXPECT_NEAR(k, paint.Contrast, 0.04) << paint.Css << " (" << samples << " pixels)";
    }
}
