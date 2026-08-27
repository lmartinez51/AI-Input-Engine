#include "Providers/MockProvider.h"

#include "Core/Logging.h"
#include "Core/Utf.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <vector>

namespace aiie {

namespace {

// Multilingual word dictionary for deterministic mock translation.
const std::map<std::wstring, std::wstring>& BuildDictionary() {
    static const std::map<std::wstring, std::wstring> dict = {
        // es -> en
        {L"hola", L"hello"},
        {L"mundo", L"world"},
        {L"necesito", L"I need"},
        {L"necesita", L"he needs"},
        {L"terminar", L"to finish"},
        {L"acabar", L"to finish"},
        {L"este", L"this"},
        {L"esta", L"this"},
        {L"este proyecto", L"this project"},
        {L"proyecto", L"project"},
        {L"antes", L"before"},
        {L"después", L"after"},
        {L"despues", L"after"},
        {L"viernes", L"Friday"},
        {L"lunes", L"Monday"},
        {L"martes", L"Tuesday"},
        {L"miércoles", L"Wednesday"},
        {L"jueves", L"Thursday"},
        {L"sábado", L"Saturday"},
        {L"domingo", L"Sunday"},
        {L"hoy", L"today"},
        {L"mañana", L"tomorrow"},
        {L"ayer", L"yesterday"},
        {L"el", L"the"},
        {L"la", L"the"},
        {L"los", L"the"},
        {L"las", L"the"},
        {L"del", L"the"},
        {L"al", L"to the"},
        {L"un", L"a"},
        {L"una", L"a"},
        {L"y", L"and"},
        {L"o", L"or"},
        {L"pero", L"but"},
        {L"yo", L"I"},
        {L"tú", L"you"},
        {L"tu", L"you"},
        {L"él", L"he"},
        {L"ella", L"she"},
        {L"nosotros", L"we"},
        {L"ellos", L"they"},
        {L"ir", L"to go"},
        {L"voy", L"I go"},
        {L"trabajo", L"work"},
        {L"trabajar", L"to work"},
        {L"casa", L"house"},
        {L"tiempo", L"time"},
        {L"día", L"day"},
        {L"dia", L"day"},
        {L"semana", L"week"},
        {L"por", L"for"},
        {L"para", L"for"},
        {L"muy", L"very"},
        {L"bien", L"well"},
        {L"gracias", L"thanks"},
        {L"por favor", L"please"},
        {L"porfavor", L"please"},
        {L"hola mundo", L"hello world"},

        // en -> es
        {L"hello", L"hola"},
        {L"world", L"mundo"},
        {L"i need", L"necesito"},
        {L"to finish", L"terminar"},
        {L"this", L"este"},
        {L"project", L"proyecto"},
        {L"before", L"antes"},
        {L"friday", L"viernes"},
        {L"the", L"el"},
        {L"and", L"y"},
        {L"today", L"hoy"},
        {L"tomorrow", L"mañana"},
    };
    return dict;
}

std::wstring ToLower(std::wstring_view s) {
    std::wstring out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

// Tokenizes on whitespace while preserving words.
std::vector<std::wstring> Tokenize(std::wstring_view text) {
    std::vector<std::wstring> tokens;
    std::wstring current;
    for (const wchar_t c : text) {
        if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

// Checks if token is a protected entity (URL, email, @handle, #hashtag, code).
bool IsProtectedToken(std::wstring_view token) {
    if (token.starts_with(L"http://") || token.starts_with(L"https://") ||
        token.starts_with(L"www.") || token.starts_with(L"@") || token.starts_with(L"#")) {
        return true;
    }
    if (token.find(L'@') != std::wstring_view::npos && token.find(L'.') != std::wstring_view::npos) {
        return true; // Email
    }
    return false;
}

} // namespace

MockProvider::MockProvider(std::chrono::milliseconds artificialDelay) : delay_(artificialDelay) {}

const std::map<std::wstring, std::wstring>& MockProvider::Dictionary() {
    return BuildDictionary();
}

std::wstring MockProvider::TranslateText(std::wstring_view text, std::wstring_view source,
                                         std::wstring_view target) {
    // Specific full-phrase mappings for Chinese tests:
    const std::wstring lowerText = ToLower(text);
    if ((source == L"es" || source == L"auto") && target == L"zh-Hans") {
        if (lowerText == L"hola mundo" || lowerText == L"hola, mundo" || lowerText == L"hola mundo.") {
            return L"你好，世界。";
        }
        if (lowerText == L"hola" || lowerText == L"hola.") {
            return L"你好。";
        }
        if (lowerText.find(L"proyecto") != std::wstring::npos) {
            return L"这个项目很重要。";
        }
        return L"你好，世界。";
    }
    if ((source == L"es" || source == L"auto") && target == L"zh-Hant") {
        if (lowerText == L"hola mundo" || lowerText == L"hola, mundo" || lowerText == L"hola mundo.") {
            return L"你好，世界。";
        }
        if (lowerText == L"hola" || lowerText == L"hola.") {
            return L"你好。";
        }
        if (lowerText.find(L"proyecto") != std::wstring::npos) {
            return L"這個項目很重要。";
        }
        return L"你好，世界。";
    }
    if (source == L"en" && target == L"zh-Hans") {
        if (lowerText == L"hello world" || lowerText == L"hello, world" || lowerText == L"hello world.") {
            return L"你好，世界。";
        }
        if (lowerText == L"hello" || lowerText == L"hello.") {
            return L"你好。";
        }
        return L"你好，世界。";
    }
    if (source == L"en" && target == L"zh-Hant") {
        if (lowerText == L"hello world" || lowerText == L"hello, world" || lowerText == L"hello world.") {
            return L"你好，世界。";
        }
        if (lowerText == L"hello" || lowerText == L"hello.") {
            return L"你好。";
        }
        return L"你好，世界。";
    }
    if ((source == L"zh-Hans" || source == L"zh-Hant") && target == L"en") {
        if (text.find(L"你好") != std::wstring_view::npos && text.find(L"世界") != std::wstring_view::npos) {
            return L"Hello, world.";
        }
        if (text.find(L"你好") != std::wstring_view::npos) {
            return L"Hello.";
        }
        return L"Hello, world.";
    }
    if ((source == L"zh-Hans" || source == L"zh-Hant") && target == L"es") {
        if (text.find(L"你好") != std::wstring_view::npos && text.find(L"世界") != std::wstring_view::npos) {
            return L"Hola, mundo.";
        }
        if (text.find(L"你好") != std::wstring_view::npos) {
            return L"Hola.";
        }
        return L"Hola, mundo.";
    }

    // Default dictionary word-by-word translation:
    const auto& dict = BuildDictionary();
    const bool esToEn = (source == L"es" || source == L"auto") && target == L"en";

    std::wstring result;
    bool first = true;
    for (const std::wstring& token : Tokenize(text)) {
        if (IsProtectedToken(token)) {
            if (!first) result.push_back(L' ');
            first = false;
            result.append(token);
            continue;
        }

        auto isWordChar = [](wchar_t c) {
            return std::iswalnum(c) != 0 || c == L'\'' || c == L'-';
        };
        size_t lead = 0;
        while (lead < token.size() && !isWordChar(token[lead])) {
            ++lead;
        }
        size_t end = token.size();
        while (end > lead && !isWordChar(token[end - 1])) {
            --end;
        }
        const std::wstring core = token.substr(lead, end - lead);
        const std::wstring leadStr = token.substr(0, lead);
        const std::wstring trailStr = token.substr(end);

        std::wstring mapped = core;
        if (!core.empty()) {
            const std::wstring lower = ToLower(core);
            auto it = dict.find(lower);
            if (it != dict.end()) {
                mapped = it->second;
                if (esToEn && !mapped.empty() && std::iswupper(core[0])) {
                    mapped[0] = static_cast<wchar_t>(std::towupper(mapped[0]));
                }
            } else {
                mapped = core; // Unknown word passes through verbatim
            }
        }

        if (!first) {
            result.push_back(L' ');
        }
        first = false;
        result.append(leadStr);
        result.append(mapped);
        result.append(trailStr);
    }
    if (result.empty()) {
        result.assign(text);
    }
    return result;
}

std::wstring MockProvider::CorrectAndRewriteText(std::wstring_view text, std::wstring_view lang,
                                                TranslationProfile profile,
                                                std::wstring_view custom) {
    (void)custom;
    const std::wstring lower = ToLower(text);

    // Canonical Spanish examples:
    if (lang == L"es" || lang == L"auto") {
        if (lower.find(L"ola ayer fui al super aver si comprava leche pero no avia") != std::wstring::npos) {
            return L"Hola, ayer fui al súper a ver si compraba leche, pero no había.";
        }
        if (lower.find(L"ola como estas") != std::wstring::npos || lower.find(L"hola como estas") != std::wstring::npos) {
            if (profile == TranslationProfile::Professional) {
                return L"Estimado, ¿cómo se encuentra?";
            }
            return L"Hola, ¿cómo estás?";
        }
        if (lower.find(L"que onda bro") != std::wstring::npos) {
            if (profile == TranslationProfile::Professional) {
                return L"Hola, ¿ya has llegado?";
            }
            if (profile == TranslationProfile::Slang) {
                return L"¿Qué onda, bro? ¿Ya llegaste?";
            }
            return L"Hola, bro, ¿ya llegaste?";
        }
        if (lower.find(L"nesecito que me ayudes") != std::wstring::npos ||
            lower.find(L"no se si mañana puedas venir por que nesecito que me ayudes") != std::wstring::npos) {
            return L"Oye, no sé si mañana puedas venir porque necesito que me ayudes.";
        }
        if (lower.find(L"puedes venir mañana") != std::wstring::npos || lower.find(L"puedes venir manana") != std::wstring::npos) {
            // Must preserve question; never answer it
            return L"¿Puedes venir mañana?";
        }
    }

    // Canonical English examples:
    if (lang == L"en") {
        if (lower.find(L"hello how are you") != std::wstring::npos) {
            if (profile == TranslationProfile::Professional) {
                return L"Hello, how are you doing today?";
            }
            return L"Hello, how are you?";
        }
        if (lower.find(L"i need to finish this project before friday") != std::wstring::npos ||
            lower.find(L"i need to finish this project") != std::wstring::npos) {
            if (profile == TranslationProfile::Professional) {
                return L"I must complete this project prior to Friday.";
            }
            return L"I need to finish this project before Friday.";
        }
        if (lower.find(L"can you come tomorrow") != std::wstring::npos) {
            // Must preserve question; never answer it
            return L"Can you come tomorrow?";
        }
    }

    // Canonical Chinese examples:
    if (lang == L"zh-Hans" || lang == L"zh-Hant") {
        if (text.find(L"明天你能来吗") != std::wstring_view::npos) {
            return (lang == L"zh-Hant") ? L"明天你能來嗎？" : L"明天你能来吗？";
        }
        if (text.find(L"你好世界") != std::wstring_view::npos) {
            return (lang == L"zh-Hant") ? L"你好，世界。" : L"你好，世界。";
        }
    }

    // Default correction pass: Capitalize first letter, ensure period if sentence-like, fix typos
    std::wstring result(text);
    if (!result.empty()) {
        result[0] = static_cast<wchar_t>(std::towupper(result[0]));
    }
    return result;
}

std::wstring MockProvider::Transform(const TranslationRequest& request) {
    if (request.operation == TransformOperation::CorrectAndRewrite) {
        return CorrectAndRewriteText(request.text, request.sourceLanguage, request.profile,
                                     request.customInstructions);
    }
    return TranslateText(request.text, request.sourceLanguage, request.targetLanguage);
}

ProviderStatus MockProvider::Status() const {
    return {ProviderStatus::State::Ready, L"offline dictionary"};
}

void MockProvider::CancelAll() {
    cancelled_->store(true);
}

void MockProvider::Translate(TranslationRequest request, TranslationCallback callback) {
    const auto perReqCancel = request.cancellationToken;
    if ((cancelled_ && cancelled_->load()) || (perReqCancel && perReqCancel->load())) {
        TranslationResult r;
        r.requestId = request.requestId;
        r.ok = false;
        r.error = L"cancelled";
        callback(std::move(r));
        return;
    }
    if (delay_.count() > 0) {
        const auto cancelled = cancelled_;
        const auto delay = delay_;
        std::thread([cancelled, perReqCancel, delay, request = std::move(request), callback = std::move(callback)]() mutable {
            std::this_thread::sleep_for(delay);
            if ((cancelled && cancelled->load()) || (perReqCancel && perReqCancel->load())) {
                TranslationResult r;
                r.requestId = request.requestId;
                r.ok = false;
                r.error = L"cancelled";
                callback(std::move(r));
                return;
            }
            TranslationResult r;
            r.text = Transform(request);
            r.ok = true;
            r.requestId = request.requestId;
            callback(std::move(r));
        }).detach();
        return;
    }
    TranslationResult r;
    r.text = Transform(request);
    r.ok = true;
    r.requestId = request.requestId;
    callback(std::move(r));
}

} // namespace aiie