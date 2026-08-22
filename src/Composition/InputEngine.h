#pragma once

#include "Configuration/AppConfig.h"
#include "Core/Dispatcher.h"
#include "Core/Logging.h"
#include "Translation/Debouncer.h"
#include "Translation/TranslationPipeline.h"
#include "Translation/TranslationTypes.h"

#include <memory>
#include <mutex>
#include <string>

namespace aiie {

// The backend adapts the engine to the input mechanism:
//  - HookBackend:  keystroke hook in the tray process (SendInput-based commit)
//  - TsfBackend:   TSF text service running inside the target application
class IInputBackend {
public:
    virtual ~IInputBackend() = default;

    // Replace the last `expectedSource.size()` characters of the current text
    // with `replacement`. Called on the input thread. The backend may verify
    // (read-back) that the source text is actually present before injecting.
    virtual void CommitReplacement(std::wstring replacement, const std::wstring& expectedSource) = 0;

    // Composition lifecycle notifications.
    virtual void OnBufferStarted() = 0;
    virtual void OnBufferChanged(std::wstring_view buffer) = 0;
    virtual void OnBufferCommitted() = 0;

    // Status indicator for the user.
    virtual void ShowStatus(std::wstring_view message) = 0;
    virtual void HideStatus() = 0;
};

enum class EngineState { Idle, Composing, Translating, Error };

struct EngineStatus {
    EngineState state = EngineState::Idle;
    bool enabled = false;
    size_t bufferLength = 0;
    std::wstring lastMessage;
};

// The composition state machine. Backend-agnostic: captures keystrokes,
// maintains the current composition buffer, debounces, submits asynchronous
// transformation requests, version-guards results, and commits replacements.
//
// Safety contract: on failure the user's typed text is never removed. The
// buffer is only ever replaced with a successful result for the exact text
// that is still present.
class InputEngine {
public:
    InputEngine(std::shared_ptr<IDispatcher> dispatcher, std::shared_ptr<IInputBackend> backend,
                std::shared_ptr<TranslationPipeline> pipeline, std::shared_ptr<Logger> logger);

    ~InputEngine();

    InputEngine(const InputEngine&) = delete;
    InputEngine& operator=(const InputEngine&) = delete;

    void ApplyConfig(const AppConfig& config);

    // --- Input thread API (keystroke events) ---
    void HandleCharacter(wchar_t c);
    void HandleBackspace();
    void HandleTerminator(wchar_t c); // . ! ? — immediate request
    void HandleCommitKey();           // Enter / navigation / control combos
    void SetEnabled(bool enabled);
    void FlushBuffer();               // commit current buffer as-is

    // --- Called from any thread ---
    void OnDebounceFired();
    void OnTranslationResult(int64_t requestId, TranslationResult result);

    // --- Queries ---
    EngineStatus Status() const;
    std::wstring Buffer() const; // for diagnostics/tests
    int64_t InflightRequestId() const { return inflightRequestId_; }

private:
    bool SubmitRequest();
    void ApplyReplacement(std::wstring replacement, size_t sourceLength);
    void UpdateState(EngineState state, std::wstring_view message);

    // Recursive: synchronous inline callbacks (cache hits, FireNow) can
    // re-enter the engine while it already holds the lock.
    mutable std::recursive_mutex mutex_;
    std::shared_ptr<IDispatcher> dispatcher_;
    std::shared_ptr<IInputBackend> backend_;
    std::shared_ptr<TranslationPipeline> pipeline_;
    std::shared_ptr<Logger> logger_;
    std::shared_ptr<Debouncer> debouncer_;

    AppConfig config_;
    bool enabled_ = true;

    std::wstring buffer_;         // current composition buffer (UTF-16)
    std::wstring lastCommitted_;  // previous sentence, used as translation context
    int64_t inflightRequestId_ = -1;
    std::wstring inflightSnapshot_;

    EngineState state_ = EngineState::Idle;
    std::wstring lastMessage_;
};

} // namespace aiie