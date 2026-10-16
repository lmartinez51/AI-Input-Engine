#include "Translation/TranslationCache.h"

#include "Core/Utf.h"

#include <algorithm>

namespace aiie {

TranslationCache::TranslationCache(size_t maxEntries) : maxEntries_(maxEntries) {}

void TranslationCache::Configure(bool enabled, size_t maxEntries) {
    std::lock_guard lock(mutex_);
    enabled_ = enabled;
    maxEntries_ = maxEntries;
    if (maxEntries_ == 0) {
        entries_.clear();
        order_.clear();
    }
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
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        ++misses_;
        return std::nullopt;
    }
    ++hits_;
    // Move to back of recency order.
    if (const auto oit = std::find(order_.begin(), order_.end(), key); oit != order_.end()) {
        order_.erase(oit);
        order_.push_back(key);
    }
    return it->second;
}

void TranslationCache::Store(const TranslationRequest& request, std::wstring result) {
    std::lock_guard lock(mutex_);
    if (!enabled_ || request.text.empty()) {
        return;
    }
    const Key key = MakeKey(request);
    if (entries_.count(key)) {
        entries_[key] = std::move(result);
        if (const auto oit = std::find(order_.begin(), order_.end(), key); oit != order_.end()) {
            order_.erase(oit);
        }
        order_.push_back(key);
        return;
    }
    entries_[key] = std::move(result);
    order_.push_back(key);
    EvictIfNeeded();
}

void TranslationCache::EvictIfNeeded() {
    while (maxEntries_ > 0 && order_.size() > maxEntries_) {
        const Key oldest = order_.front();
        order_.erase(order_.begin());
        entries_.erase(oldest);
    }
}

void TranslationCache::Clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    order_.clear();
}

size_t TranslationCache::Size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

size_t TranslationCache::Hits() const {
    std::lock_guard lock(mutex_);
    return hits_;
}

size_t TranslationCache::Misses() const {
    std::lock_guard lock(mutex_);
    return misses_;
}

} // namespace aiie