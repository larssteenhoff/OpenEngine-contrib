#include "Editor/Settings/AccentColorSettings.h"

#include "Editor/Settings/SettingsStore.h"
#include "UI/ThemeAccent.h"
#include "UI/UIAccentStyleHelper.h"

#include <string>

namespace GameEngine::Editor
{

AccentColorSettings& AccentColorSettings::Get()
{
    static AccentColorSettings instance;
    return instance;
}

AccentColorSettings::AccentColorSettings()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    int64_t stored = 0;
    if (prefs.TryGetInt64(UI::AccentStyleHelper::kPrefKeyAccentColor, stored))
        m_SavedAccent = static_cast<uint32_t>(stored);
}

uint32_t AccentColorSettings::GetAccentColor() const
{
    return ActiveOverride().value_or(UI::kDefaultAccentArgb);
}

void AccentColorSettings::SetAccentColor(uint32_t argb)
{
    m_SavedAccent = argb;
    m_PreviewAccent.reset();

    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    prefs.SetInt64(UI::AccentStyleHelper::kPrefKeyAccentColor, static_cast<int64_t>(argb));
    prefs.Save(&error);

    ApplyToTrackedWindows();
}

void AccentColorSettings::PreviewAccentColor(uint32_t argb)
{
    if (m_PreviewAccent == argb)
        return;
    m_PreviewAccent = argb;
    ApplyToTrackedWindows();
}

void AccentColorSettings::CancelPreview()
{
    if (!m_PreviewAccent)
        return;
    m_PreviewAccent.reset();
    ApplyToTrackedWindows();
}

void AccentColorSettings::ResetAccentColor()
{
    m_SavedAccent.reset();
    m_PreviewAccent.reset();

    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    prefs.Remove(UI::AccentStyleHelper::kPrefKeyAccentColor);
    prefs.Save(&error);

    ApplyToTrackedWindows();
}

void AccentColorSettings::ApplyTo(UIManager* ui)
{
    if (!ui)
        return;
    m_Windows.Add(ui);
    ApplyToWindow(*ui);
}

std::optional<uint32_t> AccentColorSettings::ActiveOverride() const
{
    return m_PreviewAccent ? m_PreviewAccent : m_SavedAccent;
}

// With no chosen colour the window carries no accent sheet, so tokens.css
// supplies the default and every derived variable.
void AccentColorSettings::ApplyToWindow(UIManager& ui)
{
    const std::optional<uint32_t> accent = ActiveOverride();
    m_Windows.SetOwnedStylesheet(
        ui, accent ? UI::AccentStyleHelper::BuildAccentColorStylesheet(*accent) : nullptr);
}

void AccentColorSettings::ApplyToTrackedWindows()
{
    m_Windows.ForEachLive([this](UIManager& ui) { ApplyToWindow(ui); });
}

} // namespace GameEngine::Editor
