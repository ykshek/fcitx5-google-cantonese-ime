#pragma once

#include <utility>

inline constexpr int kCandidateFetchBatchSize = 9;

inline int candidatePageCount(int candidateCount, int visibleCount) {
    if (candidateCount <= 0 || visibleCount <= 0) return 0;
    return (candidateCount + visibleCount - 1) / visibleCount;
}

inline std::pair<int, int> candidatePageAndCursor(int candidateIndex,
                                                  int visibleCount) {
    if (candidateIndex < 0 || visibleCount <= 0) return {0, 0};
    return {candidateIndex / visibleCount, candidateIndex % visibleCount};
}

inline int nextCandidateFetchLimit(int requestedLimit) {
    return requestedLimit + kCandidateFetchBatchSize;
}

inline bool shouldFetchMoreToFillPage(int candidateCount, int requestedLimit,
                                      int page, int visibleCount,
                                      bool fetchInFlight, bool candidatesFresh) {
    if (!candidatesFresh || fetchInFlight || candidateCount <= 0 ||
        candidateCount != requestedLimit || page < 0 || visibleCount <= 0) {
        return false;
    }

    const int pageStart = page * visibleCount;
    return pageStart < candidateCount &&
           pageStart + visibleCount > candidateCount;
}
