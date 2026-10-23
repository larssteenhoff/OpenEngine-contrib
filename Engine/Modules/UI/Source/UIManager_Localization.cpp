#include "UI/UIManager.h"
#include "Localization/LocalizationCatalog.h"
#include "UIManager_Internal.h"
#include "UI/Controls/Label.h"
#include <algorithm>

namespace GameEngine
{
void UIManager::RegisterLocalizedLabel(Label* label)
{
    if (!label) return;
    if (std::find(m_LocalizedLabelRegistry.begin(), m_LocalizedLabelRegistry.end(), label) ==
        m_LocalizedLabelRegistry.end())
        m_LocalizedLabelRegistry.push_back(label);
}

void UIManager::UnregisterLocalizedLabel(Label* label)
{
    m_LocalizedLabelRegistry.erase(
        std::remove(m_LocalizedLabelRegistry.begin(), m_LocalizedLabelRegistry.end(), label),
        m_LocalizedLabelRegistry.end());
}

void UIManager::RefreshLocalizedText()
{
    SetTextLanguage(m_LocalizationCatalog ? m_LocalizationCatalog->GetLocale() : "und");
    for (Label* label : m_LocalizedLabelRegistry)
        label->RefreshLocalizedText();
}

void UIManager::SetLocalizationCatalog(std::shared_ptr<Localization::LocalizationCatalog> catalog)
{
    m_LocalizationSubscription.Reset();
    m_LocalizationCatalog = std::move(catalog);
    if (m_LocalizationCatalog)
        m_LocalizationSubscription = m_LocalizationCatalog->Changed.Subscribe([this]() { RefreshLocalizedText(); });
    RefreshLocalizedText();
}
} // namespace GameEngine
