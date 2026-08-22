#pragma once

#include "Translation/TranslationTypes.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aiie {

// Bounded, in-memory translation cache. Keys include source language, target
// language, mode and input text. Not persisted to disk (privacy: user text is
// not stored indefinitely without explicit configuration).
class TranslationCache {
public:
    explicit TranslationCache(size_t maxEntries = 200);

    void Configure(bool enabled, size_t maxEntries);
    bool Enabled() const { return enabled_; }

    // Returns nullptr on miss.
    const std::wstring* Lookup(const TranslationRequest& request) const;
    void Store(const TranslationRequest& request, std::wstring result);

    void Clear();
    size_t Size() const;
    size_t Hits() const;
    size_t Misses() const;

private:
    using Key = std::wstring;
    static Key MakeKey(const TranslationRequest& request);
    void EvictIfNeeded();

    mutable std::mutex mutex_;
    bool enabled_ = true;
    size_t maxEntries_ = 200;
    std::map<Key, std::wstring> entries_;
    mutable std::vector<Key> order_;        // LRU list: front = oldest (touched by const Lookup)
    mutable size_t hits_ = 0;
    mutable size_t misses_ = 0;
};

} // namespace aiie