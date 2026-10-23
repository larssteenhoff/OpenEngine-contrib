#pragma once
#include "Rendering/Text/FTUtils.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Rendering::Text
{
struct ShapingOutput;

// Unicode segmentation and font selection. Glyph atlas storage is owned by FontAtlas.
class TextShaper
{
public:
    TextShaper();
    ~TextShaper();
    bool SetFallbackFonts(std::vector<std::filesystem::path> fonts);
    // BCP-47 tag in ICU canonical form, or nullopt when ICU rejects the tag.
    static std::optional<std::string> CanonicalLanguageTag(std::string_view language);
    bool SetLanguage(std::string_view language);
    uint64_t GetFallbackIdentity() const;
    uint64_t GetRevision() const { return m_Revision; }
#if defined(GE_HAVE_FREETYPE)
    FT_Face ResolveGlyphFace(FT_Face primary, uint32_t& glyphId) const;
    bool HasColorFallback() const;
#endif
#if defined(GE_HAVE_FREETYPE) && defined(GE_HAVE_HARFBUZZ)
    void Shape(FreeTypeLib& library, FreeTypeFace& primary, HBFont& primaryFont,
               std::string_view utf8, unsigned atlasSize, ShapingOutput& out,
               float letterSpacing, size_t begin, size_t end);
#endif
private:
#if defined(GE_HAVE_FREETYPE) && defined(GE_HAVE_HARFBUZZ)
    // Warns once per primary font family and script that no face draws the text.
    void ReportMissingGlyphs(FT_Face primary, int32_t script);
#endif
    struct FallbackFace;
    std::vector<std::unique_ptr<FallbackFace>> m_FallbackFaces;
    // BCP-47 "und" = undetermined: no language-specific shaping preference.
    std::string m_Language = "und";
    uint64_t m_Revision = 0;
    // (primary family name, ICU script code) pairs already reported by ReportMissingGlyphs.
    std::set<std::pair<std::string, int32_t>> m_ReportedMissingGlyphs;
};
}
