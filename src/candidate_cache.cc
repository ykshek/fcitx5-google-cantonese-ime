#include "candidate_cache.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fcitx-utils/log.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
using json = nlohmann::json;

constexpr std::uintmax_t kMaxCacheFileBytes = 16U * 1024U * 1024U;

std::filesystem::path defaultCacheFile() {
    std::filesystem::path base;
    if (const char *xdgCacheHome = std::getenv("XDG_CACHE_HOME");
        xdgCacheHome && *xdgCacheHome) {
        base = xdgCacheHome;
        if (!base.is_absolute()) base.clear();
    }
    if (base.empty()) {
        if (const char *home = std::getenv("HOME"); home && *home) {
            base = std::filesystem::path(home) / ".cache";
        }
    }
    if (base.empty()) return {};
    return base / "fcitx5-google-cantonese-ime" / "candidate-cache.json";
}

bool writeAll(int fd, const std::string &contents) {
    std::size_t written = 0;
    while (written < contents.size()) {
        const ssize_t result =
            ::write(fd, contents.data() + written, contents.size() - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        written += static_cast<std::size_t>(result);
    }
    return true;
}
} // namespace

CandidateCache::CandidateCache(std::size_t capacity, std::chrono::seconds ttl,
                               bool persistent, std::filesystem::path file)
    : capacity_(capacity), ttl_(ttl), persistent_(persistent),
      file_(file.empty() ? defaultCacheFile() : std::move(file)) {
    if (persistent_ && capacity_ > 0) loadLocked();
}

CandidateCache::~CandidateCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (persistent_ && capacity_ > 0) saveLocked();
}

bool CandidateCache::Key::operator==(const Key &other) const {
    return text == other.text && itc == other.itc && num == other.num;
}

std::size_t CandidateCache::KeyHash::operator()(const Key &key) const {
    std::size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<std::string>{}(key.itc) + 0x9e3779b9U +
            (hash << 6U) + (hash >> 2U);
    hash ^= std::hash<int>{}(key.num) + 0x9e3779b9U +
            (hash << 6U) + (hash >> 2U);
    return hash;
}

std::size_t CandidateCache::estimateBytes(
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

std::optional<std::vector<GoogleCandidate>>
CandidateCache::get(const std::string &text, const std::string &itc, int num) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Key key{text, itc, num};
    const auto found = index_.find(key);
    if (found == index_.end()) return std::nullopt;

    auto entry = found->second;
    if (!persistent_ &&
        std::chrono::steady_clock::now() - entry->insertedAt >= ttl_) {
        eraseLocked(entry);
        return std::nullopt;
    }

    entries_.splice(entries_.begin(), entries_, entry);
    return entries_.front().candidates;
}

void CandidateCache::put(std::string text, std::string itc, int num,
                         std::vector<GoogleCandidate> candidates) {
    if (candidates.empty()) return;

    Key key{std::move(text), std::move(itc), num};
    std::lock_guard<std::mutex> lock(mutex_);
    if (capacity_ == 0) return;

    insertLocked(std::move(key), std::move(candidates));
    if (persistent_) saveLocked();
}

void CandidateCache::configure(std::size_t capacity, std::chrono::seconds ttl,
                               bool persistent) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool capacityChanged = capacity != capacity_;
    const bool enablingPersistence = !persistent_ && persistent;
    capacity_ = capacity;
    ttl_ = ttl;
    persistent_ = persistent;

    if (!persistent_) clearDiskLocked();
    if (capacity_ == 0) {
        clearLocked();
        if (persistent_) clearDiskLocked();
        return;
    }
    if (enablingPersistence) {
        clearLocked();
        loadLocked();
        return;
    }
    trimLocked();
    if (persistent_ && capacityChanged) saveLocked();
}

std::size_t CandidateCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void CandidateCache::insertLocked(
    Key key, std::vector<GoogleCandidate> candidates) {
    if (candidates.empty()) return;
    const std::size_t bytes = estimateBytes(key, candidates);
    if (bytes > kMaxEstimatedBytes) return;

    const auto existing = index_.find(key);
    if (existing != index_.end()) eraseLocked(existing->second);

    entries_.push_front(Entry{std::move(key), std::move(candidates), bytes,
                              std::chrono::steady_clock::now()});
    index_.emplace(entries_.front().key, entries_.begin());
    bytes_ += bytes;
    trimLocked();
}

void CandidateCache::eraseLocked(Entries::iterator entry) {
    bytes_ -= entry->bytes;
    index_.erase(entry->key);
    entries_.erase(entry);
}

void CandidateCache::clearLocked() {
    index_.clear();
    entries_.clear();
    bytes_ = 0;
}

void CandidateCache::trimLocked() {
    while (!entries_.empty() &&
           (entries_.size() > capacity_ || bytes_ > kMaxEstimatedBytes)) {
        eraseLocked(std::prev(entries_.end()));
    }
}

void CandidateCache::loadLocked() {
    if (file_.empty()) {
        FCITX_WARN() << "Candidate cache persistence enabled, but no cache "
                        "directory is available";
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) {
        if (ec) {
            FCITX_WARN() << "Could not inspect persistent candidate cache: "
                         << ec.message();
        }
        return;
    }
    const auto fileSize = std::filesystem::file_size(file_, ec);
    if (ec || fileSize > kMaxCacheFileBytes) {
        FCITX_WARN() << "Persistent candidate cache is unreadable or too large";
        return;
    }

    std::ifstream input(file_, std::ios::binary);
    if (!input) {
        FCITX_WARN() << "Could not open persistent candidate cache";
        return;
    }
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());

    try {
        const json document = json::parse(contents);
        if (!document.is_object() || !document.contains("version") ||
            document["version"] != 1 || !document.contains("entries") ||
            !document["entries"].is_array()) {
            FCITX_WARN() << "Persistent candidate cache has an unsupported format";
            return;
        }

        struct LoadedEntry {
            Key key;
            std::vector<GoogleCandidate> candidates;
        };
        std::vector<LoadedEntry> loaded;
        for (const auto &item : document["entries"]) {
            if (!item.is_object() || !item.contains("text") ||
                !item["text"].is_string() || !item.contains("itc") ||
                !item["itc"].is_string() || !item.contains("num") ||
                !item["num"].is_number_integer() ||
                !item.contains("candidates") ||
                !item["candidates"].is_array()) {
                continue;
            }

            const auto num = item["num"].get<std::int64_t>();
            if (num <= 0 || num > std::numeric_limits<int>::max()) continue;

            LoadedEntry entry{{item["text"].get<std::string>(),
                               item["itc"].get<std::string>(),
                               static_cast<int>(num)},
                              {}};
            for (const auto &candidate : item["candidates"]) {
                if (!candidate.is_object() || !candidate.contains("text") ||
                    !candidate["text"].is_string() ||
                    !candidate.contains("matchedLength") ||
                    !candidate["matchedLength"].is_number_integer() ||
                    !candidate.contains("annotation") ||
                    !candidate["annotation"].is_string()) {
                    continue;
                }
                const auto matchedLength =
                    candidate["matchedLength"].get<std::int64_t>();
                if (matchedLength < 0 ||
                    matchedLength > std::numeric_limits<int>::max()) {
                    continue;
                }
                entry.candidates.push_back(
                    {candidate["text"].get<std::string>(),
                     static_cast<int>(matchedLength),
                     candidate["annotation"].get<std::string>()});
            }
            if (!entry.candidates.empty()) loaded.push_back(std::move(entry));
        }

        for (auto entry = loaded.rbegin(); entry != loaded.rend(); ++entry) {
            insertLocked(std::move(entry->key), std::move(entry->candidates));
        }
    } catch (const json::exception &error) {
        FCITX_WARN() << "Could not parse persistent candidate cache: "
                     << error.what();
    }
}

void CandidateCache::saveLocked() {
    if (file_.empty()) {
        FCITX_WARN() << "Candidate cache persistence enabled, but no cache "
                        "directory is available";
        return;
    }

    json document{{"version", 1}, {"entries", json::array()}};
    for (const auto &entry : entries_) {
        json candidates = json::array();
        for (const auto &candidate : entry.candidates) {
            candidates.push_back(
                {{"text", candidate.text},
                 {"matchedLength", candidate.matchedLength},
                 {"annotation", candidate.annotation}});
        }
        document["entries"].push_back(
            {{"text", entry.key.text},
             {"itc", entry.key.itc},
             {"num", entry.key.num},
             {"candidates", std::move(candidates)}});
    }

    std::string contents;
    try {
        contents = document.dump();
    } catch (const json::exception &error) {
        FCITX_WARN() << "Could not serialize persistent candidate cache: "
                     << error.what();
        return;
    }

    const auto parent = file_.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec || ::chmod(parent.c_str(), S_IRWXU) != 0) {
        FCITX_WARN() << "Could not secure candidate cache directory";
        return;
    }

    static std::atomic<std::uint64_t> tempSequence{0};
    const std::filesystem::path temp =
        file_.string() + ".tmp." + std::to_string(::getpid()) + "." +
        std::to_string(tempSequence.fetch_add(1));
    const int fd = ::open(temp.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          S_IRUSR | S_IWUSR);
    if (fd < 0) {
        FCITX_WARN() << "Could not create persistent candidate cache file: "
                     << std::strerror(errno);
        return;
    }

    bool success = writeAll(fd, contents);
    if (success && ::fsync(fd) != 0) success = false;
    const int closeResult = ::close(fd);
    if (closeResult != 0) success = false;

    if (success) {
        std::filesystem::rename(temp, file_, ec);
        success = !ec;
    }
    if (!success) {
        ::unlink(temp.c_str());
        FCITX_WARN() << "Could not save persistent candidate cache";
    }
}

void CandidateCache::clearDiskLocked() {
    if (file_.empty()) return;
    std::error_code ec;
    std::filesystem::remove(file_, ec);
    if (ec) {
        FCITX_WARN() << "Could not remove persistent candidate cache: "
                     << ec.message();
    }
}

CandidateCache &candidate_cache() {
    static CandidateCache cache;
    return cache;
}

void configure_candidate_cache(std::size_t capacity, int ttlMinutes,
                               bool persistent) {
    candidate_cache().configure(capacity, std::chrono::minutes(ttlMinutes),
                                persistent);
}
