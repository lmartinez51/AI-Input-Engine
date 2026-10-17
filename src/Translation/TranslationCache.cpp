#include "Translation/TranslationCache.h"

#include <algorithm>

namespace aiie {

TranslationCache::TranslationCache(size_t maxEntries, std::chrono::seconds ttl, size_t maxTextLength)
    : maxEntries_(maxEntries), ttl_(ttl), maxTextLength_(maxTextLength) {}

void TranslationCache::Configure(bool enabled,
                                size_t maxEntries,
                                std::chrono::seconds ttl,
                                size_t maxTextLength) {
    std::lock_guard lock(mutex_);
    enabled_ = enabled;
    maxEntries_ = maxEntries;
    ttl_ = ttl;
    maxTextLength_ = maxTextLength;
    if (maxEntries_ == 0) {
        items_.clear();
        index_.clear();
    } else {
        // If maxTextLength was lowered, purge existing entries that exceed the new threshold.
        if (maxTextLength_ > 0) {
            for (auto it = items_.begin(); it != items_.end(); ) {
                if (it->requestTextLength > maxTextLength_ || it->value.size() > maxTextLength_) {
                    index_.erase(it->key);
                    it = items_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        EvictIfNeeded();
    }
}

bool TranslationCache::IsExpired(const Entry& entry) const {
    if (ttl_.count() <= 0) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    return (now - entry.createdAt) > ttl_;
}

TranslationCache::Key TranslationCache::MakeKey(const TranslationRequest& request) {
    // The cache key strictly distinguishes provider, model, operation, language pair,
    // profile, custom instructions, and text content to prevent cache cross-pollution.
    std::wstring key;
    key.reserve(request.model.size() + request.sourceLanguage.size() + request.targetLanguage.size() +
                request.text.size() + request.customInstructions.size() + 48);
    key.append(std::to_wstring(static_cast<int>(request.providerId)));
    key.push_back(L'|');
    key.append(request.model);
    key.push_back(L'|');
    key.append(std::to_wstring(static_cast<int>(request.operation)));
    key.push_back(L'|');
    key.append(request.sourceLanguage);
    key.push_back(L'|');
    key.append(request.targetLanguage);
    key.push_back(L'|');
    key.append(std::to_wstring(static_cast<int>(request.profile)));
    key.push_back(L'|');
    key.append(request.customInstructions);
    key.push_back(L'|');
    key.append(request.text);
    return key;
}

std::optional<std::wstring> TranslationCache::Lookup(const TranslationRequest& request) const {
    std::lock_guard lock(mutex_);
    if (!enabled_) {
        return std::nullopt;
    }
    const Key key = MakeKey(request);
    const auto it = index_.find(key);
    if (it == index_.end()) {
        ++misses_;
        return std::nullopt;
    }

    // Lazy TTL check: if the entry has expired, purge it in O(1) and count as miss.
    if (IsExpired(*(it->second))) {
        items_.erase(it->second);
        index_.erase(it);
        ++misses_;
        return std::nullopt;
    }

    // If entry exceeds current payload length limit, purge in O(1) and count as miss.
    if (maxTextLength_ > 0 && (it->second->requestTextLength > maxTextLength_ || it->second->value.size() > maxTextLength_)) {
        items_.erase(it->second);
        index_.erase(it);
        ++misses_;
        return std::nullopt;
    }

    ++hits_;
    // O(1) recency update: splice the node to the front (MRU) without iterator invalidation or memory reallocation.
    items_.splice(items_.begin(), items_, it->second);
    return it->second->value;
}

void TranslationCache::Store(const TranslationRequest& request, std::wstring result) {
    std::lock_guard lock(mutex_);
    if (!enabled_ || maxEntries_ == 0 || request.text.empty()) {
        return;
    }
    const Key key = MakeKey(request);
    const auto it = index_.find(key);

    // Memory footprint guard: do not cache huge payloads (request text or result text).
    if (maxTextLength_ > 0 && (request.text.size() > maxTextLength_ || result.size() > maxTextLength_)) {
        if (it != index_.end()) {
            items_.erase(it->second);
            index_.erase(it);
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (it != index_.end()) {
        it->second->value = std::move(result);
        it->second->createdAt = now;
        it->second->requestTextLength = request.text.size();
        items_.splice(items_.begin(), items_, it->second);
        return;
    }

    // Insert new entry at the front (MRU).
    items_.push_front(Entry{key, std::move(result), now, request.text.size()});
    index_[items_.front().key] = items_.begin();
    EvictIfNeeded();
}

void TranslationCache::EvictIfNeeded() {
    // 1. Opportunistically purge expired entries from the back (LRU end).
    while (!items_.empty() && IsExpired(items_.back())) {
        index_.erase(items_.back().key);
        items_.pop_back();
    }

    // 2. Evict least recently used entries from the back in O(1) time.
    while (maxEntries_ > 0 && items_.size() > maxEntries_) {
        index_.erase(items_.back().key);
        items_.pop_back();
    }
}

void TranslationCache::Clear() {
    std::lock_guard lock(mutex_);
    items_.clear();
    index_.clear();
}

size_t TranslationCache::Size() const {
    std::lock_guard lock(mutex_);
    return items_.size();
}

size_t TranslationCache::Hits() const {
    std::lock_guard lock(mutex_);
    return hits_;
}

size_t TranslationCache::Misses() const {
    std::lock_guard lock(mutex_);
    return misses_;
}

std::chrono::seconds TranslationCache::TTL() const {
    std::lock_guard lock(mutex_);
    return ttl_;
}

size_t TranslationCache::MaxTextLength() const {
    std::lock_guard lock(mutex_);
    return maxTextLength_;
}

} // namespace aiie