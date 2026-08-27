#pragma once

#include "Configuration/AppConfig.h"
#include "Translation/TranslationCache.h"
#include "Translation/TranslationTypes.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace aiie {

// Orchestrates asynchronous transformation requests:
//   - assigns monotonically increasing request ids
//   - consults the cache
//   - forwards to the active provider
//   - enforces "latest request wins": callbacks for superseded requests are
//     delivered with ok=false (stale results never reach the engine)
//   - enforces a maximum pending request count (drops oldest)
//
// Callbacks fire on the provider's thread. Callers marshal to their own
// thread via a dispatcher. The engine additionally guards with its own buffer
// version check (belt and braces).
class TranslationPipeline {
public:
    explicit TranslationPipeline(std::shared_ptr<ITranslationProvider> provider,
                                 std::shared_ptr<TranslationCache> cache,
                                 int requestTimeoutMs = 30000);
    ~TranslationPipeline();

    void SetProvider(std::shared_ptr<ITranslationProvider> provider);
    std::shared_ptr<ITranslationProvider> Provider() const { return provider_; }

    // Submits a request. Returns the assigned request id (0 if rejected).
    int64_t Submit(TranslationRequest request, TranslationCallback callback);

    // Cancels everything in flight.
    void CancelAll();

    // Provider status (for UI).
    ProviderStatus Status() const;

    // Capacity queries (for diagnostics & tests)
    size_t PendingAllowed() const;
    size_t PendingSpeculative() const;
    size_t TotalPending() const;

    static constexpr size_t kMaxSpeculativePending = 1;
    static constexpr size_t kMaxAllowedPending = 4;
    static constexpr size_t kMaxPending = kMaxSpeculativePending + kMaxAllowedPending;

private:
    struct InflightSlot {
        int64_t id = 0;
        CommitAuthority authority = CommitAuthority::Forbidden;
        std::shared_ptr<std::atomic<bool>> cancellationToken;
    };

    void Finish(TranslationRequest request, TranslationResult result, TranslationCallback callback);

    std::shared_ptr<ITranslationProvider> provider_;
    std::shared_ptr<TranslationCache> cache_;
    int requestTimeoutMs_;

    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    mutable std::mutex mutex_;
    std::atomic<int64_t> nextId_{1};
    int64_t latestRequested_ = 0; // guarded by mutex_
    size_t pendingAllowed_ = 0;   // guarded by mutex_
    size_t pendingSpeculative_ = 0; // guarded by mutex_
    std::vector<InflightSlot> inflightSlots_; // guarded by mutex_
};

} // namespace aiie