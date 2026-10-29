#include "candidate_paging.h"
#include "config.h"

int main() {
    if (effectiveCandidateListSize(9) != 9 ||
        effectiveCandidateListSize(1) != 1 ||
        effectiveCandidateListSize(0) != 1 ||
        effectiveCandidateListSize(-5) != 1 ||
        effectiveCandidateListSize(10) != 9) {
        return 1;
    }

    if (candidatePageCount(0, 5) != 0 ||
        candidatePageCount(9, 5) != 2 ||
        candidatePageCount(10, 5) != 2 ||
        candidatePageCount(11, 5) != 3 ||
        candidatePageCount(9, 1) != 9) {
        return 2;
    }

    // A five-row page is partial after the initial nine-result response.
    // The next fetched candidate must continue that page rather than skip it.
    if (candidatePageAndCursor(9, 5) != std::pair<int, int>{1, 4}) {
        return 3;
    }

    // With four visible rows, candidate ten continues page two after an
    // initial nine-result batch, again without omitting an index.
    if (candidatePageAndCursor(9, 4) != std::pair<int, int>{2, 1} ||
        candidatePageAndCursor(18, 5) != std::pair<int, int>{3, 3}) {
        return 4;
    }

    if (nextCandidateFetchLimit(0) != 9 ||
        nextCandidateFetchLimit(9) != 18 ||
        nextCandidateFetchLimit(18) != 27) {
        return 5;
    }
}
