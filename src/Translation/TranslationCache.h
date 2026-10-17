#pragma once

#include "Translation/TranslationTypes.h"

#include <chrono>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace aiie {

// Bounded, in-memory translation cache with canonical O(1) LRU eviction,
// lazy TTL-based expiration, and text length bounding. Not persisted to disk
// (privacy: user text is not stored indefinitely without explicit configuration).
class TranslationCache {
public:
    explicit TranslationCache(size_t maxEntries = 200,
                             std::chrono::seconds ttl = std::chrono::seconds(1800),
                             size_t maxTextLength = 4000);

    void Configure(bool enabled,
                   size_t maxEntries,
                   std::chrono::seconds ttl = std::chrono::seconds(1800),
                   size_t maxTextLength = 4000);
    bool Enabled() const { return enabled_; }

    // Returns nullopt on miss or expiration.
    std::optional<std::wstring> Lookup(const TranslationRequest& request) const;
    void Store(const TranslationRequest& request, std::wstring result);

    void Clear();
    size_t Size() const;
    size_t Hits() const;
    size_t Misses() const;
    std::chrono::seconds TTL() const;
    size_t MaxTextLength() const;

private:
    using Key = std::wstring;

    struct Entry {
        Key key;
        std::wstring value;
        std::chrono::steady_clock::time_point createdAt;
        size_t requestTextLength = 0;
    };

    static Key MakeKey(const TranslationRequest& request);
    void EvictIfNeeded();
    bool IsExpired(const Entry& entry) const;

    mutable std::mutex mutex_;
    bool enabled_ = true;
    size_t maxEntries_ = 200;
    std::chrono::seconds ttl_{1800};
    size_t maxTextLength_ = 4000;

    // Canonical O(1) LRU data structure:
    // items_: front = most recently used (MRU), back = least recently used (LRU/oldest).
    mutable std::list<Entry> items_;
    mutable std::unordered_map<Key, std::list<Entry>::iterator> index_;

    mutable size_t hits_ = 0;
    mutable size_t misses_ = 0;
};

} // namespace aiie