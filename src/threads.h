#ifndef THREADS_H
#define THREADS_H

#include "board.h"
#include "move.h"
#include "move_generator.h"
#include "nnue.h"
#include "search.h"
#include "syzygy.h"
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Search thread pool (Lazy SMP)
//
// A fixed set of persistent threads, each with its own search state (board,
// histories, NNUE stack). They all search the same root position and share
// only the transposition table. Thread 0 is the main search thread: it does
// the time management, prints the info lines and finally the bestmove; the
// others are helpers that search until the main thread is done.
//
// All functions here are called from the UCI thread only. The search runs
// asynchronously: threads_start_search() returns immediately.
// =============================================================================

#define THREADS_MAX 256

// Everything a search needs besides the position
typedef struct {
    long startTimeMs;
    long softTimeLimit;   // 0 = none
    long hardTimeLimit;   // 0 = none
    int depthLimit;       // 0 = none
    uint64_t nodeLimit;   // 0 = none
    bool infinite;        // "go infinite": bestmove only after "stop"
    bool ponder;          // "go ponder": no time limits until "ponderhit"

    // Syzygy: in-search WDL probing and the root restriction from a DTZ probe
    // (same meaning as the fields of SearchInfo)
    int tbProbeLimit;
    Move tbRootMoves[MAX_MOVES];
    int tbRootMoveCount;
    int tbRootScore;
    int tbRootMatePlies;
    Move tbRootPv[SYZYGY_MAX_PV];
    int tbRootPvLen;

    SearchParams params;
} SearchRequest;

// Creates the pool with `count` threads (clamped to 1..THREADS_MAX). Calling
// it again replaces the pool; the search histories start empty. Waits for a
// running search to finish first.
void threads_set_count(int count);
int threads_count(void);

// Stops a running search and releases all threads.
void threads_destroy(void);

// Clears the histories of all threads (new game). Waits for a running search.
void threads_clear_histories(void);

// Starts a search of `board`. `acc` is the NNUE accumulator of that position.
// Must not be called while a search is running (use threads_wait() first).
void threads_start_search(const Board* board, const NNUEAccumulator* acc,
                          const NNUENetwork* net, const SearchRequest* request);

// True from threads_start_search() until the bestmove has been printed.
bool threads_searching(void);

// True if the running search only ends on "stop" (infinite or still pondering).
bool threads_search_unbounded(void);

// Asks the running search to finish as soon as possible (does not wait).
void threads_stop(void);

// The move pondered on was played: the search continues as a normal one.
void threads_ponderhit(void);

// Blocks until no search is running (bestmove printed).
void threads_wait(void);

#endif // THREADS_H
