#pragma once

#include "daemon_client.h"

#include <chrono>
#include <cstddef>
#include <iterator>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class CandidateCache {
public:
    // Keep both a configurable entry limit and a bounded estimated footprint.
    static constexpr std::size_t kMaxEstimatedBytes = 8U * 1024U * 1024U;
    static constexpr auto kTtl = std::chrono::minutes(5);

    explicit CandidateCache(std::size_t capacity = 64) : capacity_(capacity) {}

    std::optional<std::vector<GoogleCandidate>> get(const std::string &text,
                                                    const std::string &itc,
                                                    int num) {
        std::lock_guard<std::mutex> lock(mutex_);
        const Key key{text, itc, num};
        const auto found = index_.find(key);
        if (found == index_.end()) return std::nullopt;

        auto entry = found->second;
        if (std::chrono::steady_clock::now() - entry->insertedAt >= kTtl) {
            erase(entry);
            return std::nullopt;
        }

        entries_.splice(entries_.begin(), entries_, entry);
        return entries_.front().candidates;
    }

    void put(std::string text, std::string itc, int num,
             std::vector<GoogleCandidate> candidates) {
        if (candidates.empty()) return;

        Key key{text, itc, num};
        const std::size_t bytes = estimateBytes(key, candidates);
        if (bytes > kMaxEstimatedBytes) return;

        std::lock_guard<std::mutex> lock(mutex_);
        if (capacity_ == 0) return;

        const auto existing = index_.find(key);
        if (existing != index_.end()) erase(existing->second);

        entries_.push_front(Entry{std::move(key), std::move(candidates),
                                  bytes, std::chrono::steady_clock::now()});
        index_.emplace(entries_.front().key, entries_.begin());
        bytes_ += bytes;
        trim();
    }

    void setCapacity(std::size_t capacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = capacity;
        trim();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    struct Key {
        std::string text;
        std::string itc;
        int num;

        bool operator==(const Key &other) const {
            return text == other.text && itc == other.itc && num == other.num;
        }
    };

    struct KeyHash {
        std::size_t operator()(const Key &key) const {
            std::size_t hash = std::hash<std::string>{}(key.text);
            hash ^= std::hash<std::string>{}(key.itc) + 0x9e3779b9U +
                    (hash << 6U) + (hash >> 2U);
            hash ^= std::hash<int>{}(key.num) + 0x9e3779b9U +
                    (hash << 6U) + (hash >> 2U);
            return hash;
        }
    };

    struct Entry {
        Key key;
        std::vector<GoogleCandidate> candidates;
        std::size_t bytes;
        std::chrono::steady_clock::time_point insertedAt;
    };

    using Entries = std::list<Entry>;

    static std::size_t estimateBytes(
        const Key &key, const std::vector<GoogleCandidate> &candidates) {
        std::size_t bytes = sizeof(Entry) + 2 * (sizeof(Key) + key.text.size() +
                                                 key.itc.size()) +
                            4 * sizeof(void *) +
                            sizeof(std::pair<const Key, Entries::iterator>) +
                            candidates.size() * sizeof(GoogleCandidate);
        for (const auto &candidate : candidates) {
            bytes += candidate.text.size() + candidate.annotation.size();
        }
        return bytes;
    }

    void erase(Entries::iterator entry) {
        bytes_ -= entry->bytes;
        index_.erase(entry->key);
        entries_.erase(entry);
    }

    void trim() {
        while (!entries_.empty() &&
               (entries_.size() > capacity_ ||
                bytes_ > kMaxEstimatedBytes)) {
            erase(std::prev(entries_.end()));
        }
    }

    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::size_t bytes_ = 0;
    Entries entries_;
    std::unordered_map<Key, Entries::iterator, KeyHash> index_;
};
