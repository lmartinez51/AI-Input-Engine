#pragma once

#include "Translation/TranslationTypes.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace aiie {

// Deterministic offline provider used to validate the whole pipeline without
// network access. Supports Translate (ES, EN, zh-Hans, zh-Hant) and CorrectAndRewrite
// with tone profiles, question safety, and token preservation.
class MockProvider : public ITranslationProvider {
public:
    explicit MockProvider(std::chrono::milliseconds artificialDelay = std::chrono::milliseconds(0));

    std::wstring_view Name() const override { return L"mock"; }
    std::wstring_view DisplayName() const override { return L"Mock (offline demo)"; }
    bool RequiresNetwork() const override { return false; }
    ProviderStatus Status() const override;
    void CancelAll() override;
    void Translate(TranslationRequest request, TranslationCallback callback) override;

    // Executes deterministic transformation according to request operation & parameters.
    static std::wstring Transform(const TranslationRequest& request);

    // Direct translation helper; exposed for tests.
    static std::wstring TranslateText(std::wstring_view text, std::wstring_view source,
                                      std::wstring_view target);

    // Direct rewrite helper; exposed for tests.
    static std::wstring CorrectAndRewriteText(std::wstring_view text, std::wstring_view lang,
                                             TranslationProfile profile = TranslationProfile::Neutral,
                                             std::wstring_view custom = L"");

    // Word-level mapping used by TranslateText; exposed for tests.
    static const std::map<std::wstring, std::wstring>& Dictionary();

private:
    std::chrono::milliseconds delay_;
    std::shared_ptr<std::atomic<bool>> cancelled_ = std::make_shared<std::atomic<bool>>(false);
};

} // namespace aiie