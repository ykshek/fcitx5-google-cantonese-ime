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
