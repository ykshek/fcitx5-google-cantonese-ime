#pragma once

#include "daemon_client.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class CandidateCache {
public:
    static constexpr std::size_t kMaxEstimatedBytes = 8U * 1024U * 1024U;

    explicit CandidateCache(
        std::size_t capacity = 64,
        std::chrono::seconds ttl = std::chrono::minutes(5),
        bool persistent = false, std::filesystem::path file = {});
    ~CandidateCache();

    CandidateCache(const CandidateCache &) = delete;
    CandidateCache &operator=(const CandidateCache &) = delete;

    std::optional<std::vector<GoogleCandidate>> get(const std::string &text,
                                                    const std::string &itc,
                                                    int num);
    void put(std::string text, std::string itc, int num,
             std::vector<GoogleCandidate> candidates);
    void configure(std::size_t capacity, std::chrono::seconds ttl,
                   bool persistent);
    std::size_t size() const;

private:
    struct Key {
        std::string text;
        std::string itc;
        int num;

        bool operator==(const Key &other) const;
    };

    struct KeyHash {
        std::size_t operator()(const Key &key) const;
    };

    struct Entry {
        Key key;
        std::vector<GoogleCandidate> candidates;
        std::size_t bytes;
        std::chrono::steady_clock::time_point insertedAt;
    };

    using Entries = std::list<Entry>;

    static std::size_t estimateBytes(
        const Key &key, const std::vector<GoogleCandidate> &candidates);
    void insertLocked(Key key, std::vector<GoogleCandidate> candidates);
    void eraseLocked(Entries::iterator entry);
    void clearLocked();
    void trimLocked();
    void loadLocked();
    void saveLocked();
    void clearDiskLocked();

    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::chrono::seconds ttl_;
    bool persistent_;
    std::size_t bytes_ = 0;
    std::filesystem::path file_;
    Entries entries_;
    std::unordered_map<Key, Entries::iterator, KeyHash> index_;
};

CandidateCache &candidate_cache();

// Configure the process-wide cache from the current Fcitx settings.
void configure_candidate_cache(std::size_t capacity, int ttlMinutes,
                               bool persistent);
