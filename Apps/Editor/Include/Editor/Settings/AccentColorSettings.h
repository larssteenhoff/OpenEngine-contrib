#pragma once

#include "Editor/Settings/TrackedEditorWindows.h"

#include <cstdint>
#include <optional>

namespace GameEngine
{
class UIManager;

namespace Editor
{

// The editor accent colour (Settings > UI Colors). The default accent and its
// derived variables live in tokens.css; this owner overrides them with a
// `:root` sheet only while the user has chosen a colour, in every editor window
// that has been applied, so a change reaches all of them.
class AccentColorSettings
{
public:
    static AccentColorSettings& Get();

    // The accent in effect: the colour being previewed, else the saved one,
    // else the theme default.
    uint32_t GetAccentColor() const;

    // Save `argb` as the user's accent and apply it everywhere.
    void SetAccentColor(uint32_t argb);

    // Show `argb` everywhere without saving it (colour-picker drag).
    void PreviewAccentColor(uint32_t argb);

    // Drop a preview and show the saved state again (colour-picker cancel).
    void CancelPreview();

    // Forget the user's accent: the preference is removed and every window
    // falls back to the theme default.
    void ResetAccentColor();

    // Push the current accent into `ui` and track it for later changes. Call
    // once per editor window after its theme is attached: the accent sheet
    // must follow the theme in sheet order to win the `:root` tie.
    void ApplyTo(UIManager* ui);

private:
    AccentColorSettings();
    std::optional<uint32_t> ActiveOverride() const;
    void ApplyToWindow(UIManager& ui);
    void ApplyToTrackedWindows();

    std::optional<uint32_t> m_SavedAccent;
    std::optional<uint32_t> m_PreviewAccent;
    TrackedEditorWindows m_Windows;
};

} // namespace Editor
} // namespace GameEngine
