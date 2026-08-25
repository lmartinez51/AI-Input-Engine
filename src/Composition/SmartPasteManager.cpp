#include "Composition/SmartPasteManager.h"

#include "Core/Logging.h"

#include <format>

namespace aiie {

SmartPasteManager::SmartPasteManager(std::shared_ptr<TranslationPipeline> pipeline,
                                     std::shared_ptr<StatusWindow> status,
                                     ConfigStore* configStore,
                                     std::shared_ptr<IDispatcher> dispatcher)
    : pipeline_(std::move(pipeline)),
      status_(std::move(status)),
      configStore_(configStore),
      dispatcher_(std::move(dispatcher)),
      alive_(std::make_shared<std::atomic<bool>>(true)) {}

SmartPasteManager::~SmartPasteManager() {
    if (alive_) {
        alive_->store(false);
    }
}

bool SmartPasteManager::Trigger(HWND targetHwndOverride) {
    if (!configStore_) {
        return false;
    }

    const AppConfig cfg = configStore_->Get();
    if (!cfg.enabled) {
        if (status_) {
            status_->Show(L"AI Input: disabled");
        }
        return false;
    }

    if (!cfg.smartPasteEnabled) {
        if (status_) {
            status_->Show(L"Smart Paste: disabled in settings");
        }
        return false;
    }

    if (busy_.load()) {
        if (status_) {
            status_->Show(L"Smart Paste: busy");
        }
        return false;
    }

    // Step 1: Inspect clipboard formats
    const auto inspect = ClipboardHelper::InspectClipboard();
    if (inspect.hasUnsupportedNonText) {
        if (status_) {
            status_->Show(L"Smart Paste: clipboard contains non-text data");
        }
        Logger::Instance().Info(cat::Input, L"Smart Paste aborted: clipboard contains non-text data");
        return false;
    }

    if (!inspect.hasText) {
        if (status_) {
            status_->Show(L"Smart Paste: clipboard is empty");
        }
        return false;
    }

    // Step 2: Read clipboard Unicode text and original sequence number
    std::wstring clipboardText;
    DWORD origSequence = 0;
    if (!ClipboardHelper::ReadUnicodeText(clipboardText, origSequence, nullptr) || clipboardText.empty()) {
        if (status_) {
            status_->Show(L"Smart Paste: clipboard is empty");
        }
        return false;
    }

    // Step 3: Capture Target Context before starting async request
    TargetContext target;
    target.targetHwnd = targetHwndOverride ? targetHwndOverride : GetForegroundWindow();
    if (!target.targetHwnd) {
        target.targetHwnd = GetDesktopWindow();
    }
    target.threadId = GetWindowThreadProcessId(target.targetHwnd, &target.processId);

    GUITHREADINFO gti = { sizeof(gti) };
    if (target.threadId && GetGUIThreadInfo(target.threadId, &gti) && gti.hwndFocus) {
        target.focusHwnd = gti.hwndFocus;
    } else {
        target.focusHwnd = target.targetHwnd;
    }

    // Step 4: Build standard TranslationRequest reusing current live configuration
    TranslationRequest req;
    req.text = clipboardText;
    req.operation = cfg.operation;
    req.sourceLanguage = cfg.sourceLanguage;
    req.targetLanguage = (cfg.operation == TransformOperation::CorrectAndRewrite)
                             ? cfg.sourceLanguage
                             : cfg.targetLanguage;
    req.profile = cfg.profile;
    req.customInstructions = cfg.customProfileInstructions;
    req.mode = cfg.mode;
    req.useCache = cfg.cacheEnabled;
    req.requestId = ++reqCounter_;

    const int64_t currentId = req.requestId;
    currentRequestId_.store(currentId);
    busy_.store(true);

    if (status_) {
        status_->Show(L"Smart Paste: translating...");
    }

    Logger::Instance().Info(cat::Input,
                           L"Smart Paste request #{} initiated ({} chars, target={:#x}, pid={})",
                           currentId, clipboardText.size(),
                           reinterpret_cast<uintptr_t>(target.targetHwnd), target.processId);

    if (Logger::Instance().LogRawText()) {
        Logger::Instance().Debug(cat::Input, L"Smart Paste request #{} text: '{}'", currentId, clipboardText);
    }

    // Step 5: Submit request through existing TranslationPipeline
    auto alive = alive_;
    pipeline_->Submit(std::move(req), [this, alive, currentId, target, origSequence,
                                       originalText = std::move(clipboardText)](TranslationResult result) {
        if (!alive || !alive->load()) {
            return;
        }
        if (dispatcher_) {
            dispatcher_->Post([this, alive, currentId, target, origSequence,
                               originalText = std::move(originalText),
                               result = std::move(result)]() mutable {
                if (!alive || !alive->load()) {
                    return;
                }
                OnTranslationCompleted(currentId, target, origSequence,
                                       std::move(originalText), std::move(result));
            });
        } else {
            OnTranslationCompleted(currentId, target, origSequence,
                                   std::move(originalText), std::move(result));
        }
    });

    return true;
}

void SmartPasteManager::OnTranslationCompleted(int64_t requestId,
                                               TargetContext target,
                                               DWORD origSequence,
                                               std::wstring originalText,
                                               TranslationResult result) {
    if (requestId != currentRequestId_.load()) {
        Logger::Instance().Debug(cat::Input, L"Smart Paste result #{} discarded (superseded by #{})",
                                requestId, currentRequestId_.load());
        return;
    }

    busy_.store(false);

    // Error Safety: If translation failed, do NOT touch target or clipboard
    if (!result.ok) {
        Logger::Instance().Warn(cat::Input, L"Smart Paste request #{} failed: {}", requestId,
                                result.errorDetail.empty() ? result.error : result.errorDetail);
        if (status_) {
            status_->Show(std::format(L"Smart Paste failed: {}", result.error));
        }
        return;
    }

    // Target Validation: Validate target window is still active foreground window
    if (target.targetHwnd != GetDesktopWindow()) {
        const HWND currentFg = GetForegroundWindow();
        if (currentFg != target.targetHwnd) {
            Logger::Instance().Warn(cat::Input,
                                    L"Smart Paste #{} cancelled: target window changed (orig={:#x}, current={:#x})",
                                    requestId, reinterpret_cast<uintptr_t>(target.targetHwnd),
                                    reinterpret_cast<uintptr_t>(currentFg));
            if (status_) {
                status_->Show(L"Smart Paste cancelled: target window changed");
            }
            return;
        }

        DWORD currentPid = 0;
        if (currentFg) {
            GetWindowThreadProcessId(currentFg, &currentPid);
            if (currentPid && currentPid != target.processId) {
                Logger::Instance().Warn(cat::Input,
                                        L"Smart Paste #{} cancelled: target process changed (orig={}, current={})",
                                        requestId, target.processId, currentPid);
                if (status_) {
                    status_->Show(L"Smart Paste cancelled: target process changed");
                }
                return;
            }
        }
    }

    // Clipboard Sequence Validation & Safe Paste
    if (status_) {
        status_->Show(L"Smart Paste: pasting...");
    }

    const bool pasted = ClipboardHelper::ExecuteSafePasteAndRestore(
        target.targetHwnd, result.text, originalText, origSequence);

    if (pasted) {
        if (status_) {
            status_->Show(L"Smart Paste: done");
        }
        Logger::Instance().Info(cat::Input, L"Smart Paste #{} completed successfully", requestId);
    } else {
        if (status_) {
            status_->Show(L"Smart Paste cancelled: clipboard was modified externally");
        }
        Logger::Instance().Warn(cat::Input, L"Smart Paste #{} aborted during paste sequence check", requestId);
    }
}

} // namespace aiie
