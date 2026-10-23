#pragma once

#include <string>
#include <memory>
#include "Localization/LocalizationCatalog.h"
#include "UI/ITextMeasurable.h"
#include "UI/UIElement.h"
#include "UI/UIManagerRef.h"
namespace GameEngine {

class Label : public UIElement, public ITextMeasurable {
public:
    Label() { m_TextMeasurable = this; }
    ~Label() override;

    // Resolve a stable, nonzero message key using the owning UIManager's catalog.
    // fallback is literal text shown without a catalog or when resolution fails.
    // arguments are ICU message arguments: the named values that fill the
    // placeholders of the message pattern, e.g. {"count", int64_t{2}} for
    // "{count, plural, one {# coin} other {# coins}}".
    void BindLocalization(StringId key, std::string fallback,
                          Localization::MessageArguments arguments = {});
    // Replaces the stored ICU message arguments and re-resolves the text.
    // Call it when the application value changes, not per frame; a locale or
    // catalog change re-resolves with the stored arguments on its own.
    void SetLocalizationArguments(Localization::MessageArguments arguments);
    // Removes the binding; the label keeps the text it shows until the next SetText.
    void ClearLocalization();
    // Re-resolves the bound key against the owning manager's catalog.
    void RefreshLocalizedText();

    // Text API specific to Label
    const std::string& GetText() const { return m_Text; }
    // Shows literal text and removes any localization binding, so a catalog change does not
    // overwrite it.
    void SetText(const std::string& t);
    // For frequently-updated debug/perf overlays where the label's Yoga layout box
    // is known to be stable (e.g. fixed width / absolute positioned), allow a
    // paint-only update that avoids triggering a Yoga relayout.
	
		    // Provide intrinsic text measurement so UIManager/Yoga can size labels
		    // without knowing about Label specifically.
		    void GetTextMeasureInfo(ITextMeasurable::TextMeasureInfo& info,
		                           const ResolvedStyle& style) const override
	    {
	        (void)style;
	        info.HasText = !m_Text.empty();
	        info.Text = m_Text;
	        info.ExplicitFontSize = 0.0f; // use style.fontSize
	    }

	    bool AllowWrapForMeasure() const override { return true; }

	    // Expose label text to generic UIManager helpers (geometry overrides,
	    // debug overlays) without requiring it to know about Label.
	    const std::string& GetTextContent() const override { return m_Text; }

private:
    // Changes the displayed text and keeps the binding.
    void ShowText(const std::string& t);
    // UXML parse/reconcile hook: map localization-key and literal text to the same binding API.
    void OnAuthoredAttributesApplied(const std::unordered_map<std::string, std::string>& attributes) override;
    void OnOwnerManagerChanged(UIManager* owner) override;
    // Keeps the owner's localized-label registry in step with the key and the owner.
    void UpdateLocalizationRegistration();
    // The manager whose localized-label registry holds this label; empty once it is destroyed.
    UIManagerRef m_RegisteredLocalizationManager;
    StringId m_LocalizationKey = 0;
    std::string m_LocalizationFallback;
    Localization::MessageArguments m_LocalizationArguments;
    std::string m_Text;
};

} // namespace GameEngine

