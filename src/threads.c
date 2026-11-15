#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "threads.h"
#include "board_modifiers.h"
#include "tt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#endif

// The recursive search needs far more stack than the 1 MB (or less) some
// platforms give a new thread by default.
#define SEARCH_THREAD_STACK_SIZE (8u * 1024u * 1024u)

// Search state of one thread. Allocated and released by the thread itself.
typedef struct {
    Board board;              // private copy of the root position
    NNUEAccumulator rootAcc;  // accumulator of the root position
    SearchInfo info;
    ContHistory cont;
} ThreadState;

typedef struct {
    pthread_t handle;
    int index;           // 0 = main search thread
    ThreadState* state;  // set by the thread before it first goes idle
    // Guarded by pool.mutex:
    bool searching;      // false = idle, waiting for work
    bool exit;           // leave the thread function
} SearchThread;

// One mutex and one condition variable for the whole pool. Every change of a
// `searching` / `exit` flag, of the stop flag and of the ponder flag is made
// with the mutex held and followed by a broadcast, so a waiter can never miss
// it. Search threads only take the mutex when they start or finish a search,
// never inside the search itself.
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    SearchThread* threads[THREADS_MAX];
    SearchInfo* infos[THREADS_MAX];  // SearchShared.threads
    int count;
    bool infinite;                   // current search: "go infinite" (guarded by mutex)
    SearchShared shared;
} pool = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static void* state_alloc(void) {
    size_t size = (sizeof(ThreadState) + 63) / 64 * 64;
#ifdef _WIN32
    void* mem = _aligned_malloc(size, 64);
#else
    void* mem = aligned_alloc(64, size);
#endif
    if (mem != NULL) memset(mem, 0, size);
    return mem;
}

static void state_free(void* mem) {
#ifdef _WIN32
    _aligned_free(mem);
#else
    free(mem);
#endif
}

// -----------------------------------------------------------------------------
// Helpers for the final move
// -----------------------------------------------------------------------------

// True if `move` is a legal move in `board`.
static bool move_is_legal(const Board* board, Move move) {
    if (move == 0) return false;
    Board copy = *board;
    MoveList moves;
    moves.count = 0;
    generateMoves(&copy, &moves);
    for (int i = 0; i < moves.count; i++) {
        if (moves.moves[i] != move) continue;
        MoveUndoInfo undo;
        applyMove(&copy, move, &undo, NULL, NULL);
        return !isKingAttacked(&copy, !copy.whiteToMove);
    }
    return false;
}

// First legal move of `board`, 0 if there is none.
static Move first_legal_move(const Board* board) {
    Board copy = *board;
    MoveList moves;
    moves.count = 0;
    generateMoves(&copy, &moves);
    for (int i = 0; i < moves.count; i++) {
        MoveUndoInfo undo;
        applyMove(&copy, moves.moves[i], &undo, NULL, NULL);
        bool legal = !isKingAttacked(&copy, !copy.whiteToMove);
        undoMove(&copy, moves.moves[i], &undo, NULL, NULL);
        if (legal) return moves.moves[i];
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Thread functions
// -----------------------------------------------------------------------------

static void set_helpers_searching(void) {
    pthread_mutex_lock(&pool.mutex);
    for (int i = 1; i < pool.count; i++) {
        pool.threads[i]->searching = true;
    }
    pthread_cond_broadcast(&pool.cond);
    pthread_mutex_unlock(&pool.mutex);
}

static bool any_helper_searching(void) {
    for (int i = 1; i < pool.count; i++) {
        if (pool.threads[i]->searching) return true;
    }
    return false;
}

// The search of the main thread: runs the helpers, searches itself, and
// reports the result.
static void main_thread_search(SearchThread* th) {
    ThreadState* st = th->state;
    SearchInfo* info = &st->info;
    Move best_move = 0;

    MoveList moves;
    moves.count = 0;
    generateMoves(&st->board, &moves);
    printf("info string DEBUG: UCI: Generated %d moves before calling search.\n", moves.count);
    fflush(stdout);

    if (moves.count > 0) {
        printf("info string DEBUG: UCI: Calling iterative_deepening_search...\n");
        fflush(stdout);
        set_helpers_searching();
        best_move = search_iterate(&st->board, info);
    } else {
        printf("info string DEBUG: UCI: No moves generated, not calling search.\n");
        fflush(stdout);
    }

    // UCI: an infinite or pondering search must not report its move before
    // "stop" (or "ponderhit"), even if it has nothing left to search. Then
    // stop the helpers and wait until every one of them is idle again.
    pthread_mutex_lock(&pool.mutex);
    while ((pool.infinite || atomic_load(&pool.shared.ponder)) &&
           !atomic_load(&pool.shared.stop)) {
        pthread_cond_wait(&pool.cond, &pool.mutex);
    }
    atomic_store(&pool.shared.stop, true);
    while (any_helper_searching()) {
        pthread_cond_wait(&pool.cond, &pool.mutex);
    }
    pthread_mutex_unlock(&pool.mutex);

    Move ponder_move = 0;
    if (moves.count > 0) {
        if (info->tbRootMoveCount > 0 && info->tbRootPvLen > 0) {
            // On a tablebase root hit, play the DTZ-optimal move so that the
            // played move, the reported score and the displayed PV all agree.
            best_move = info->tbRootPv[0];
            if (info->tbRootPvLen > 1) ponder_move = info->tbRootPv[1];
        } else {
            ponder_move = info->ponderMove;
        }
        printf("info string DEBUG: UCI: iterative_deepening_search returned. Best move: %u\n", best_move);
        printf("Best score: %d\n", info->bestScoreThisIteration);

        // A search stopped before it completed anything has no move yet.
        if (best_move == 0) {
            best_move = first_legal_move(&st->board);
            ponder_move = 0;
        }
    }

    if (best_move != 0) {
        char move_str[6];
        moveToString(best_move, move_str);

        // Only announce a ponder move that is legal after the best move (a PV
        // can be cut short or patched from the transposition table).
        bool ponder_ok = false;
        if (ponder_move != 0) {
            Board after = st->board;
            MoveUndoInfo undo;
            applyMove(&after, best_move, &undo, NULL, NULL);
            ponder_ok = move_is_legal(&after, ponder_move);
        }
        if (ponder_ok) {
            char ponder_str[6];
            moveToString(ponder_move, ponder_str);
            printf("bestmove %s ponder %s\n", move_str, ponder_str);
        } else {
            printf("bestmove %s\n", move_str);
        }
    } else {
        printf("bestmove 0000\n"); // no legal move (mate / stalemate at the root)
    }
    fflush(stdout);
}

static void* thread_main(void* arg) {
    SearchThread* th = (SearchThread*)arg;

    ThreadState* st = state_alloc();
    if (st == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for search thread %d\n", th->index);
        exit(1);
    }
    st->info.cont = &st->cont;
    clear_search_history(&st->info);

    pthread_mutex_lock(&pool.mutex);
    th->state = st;
    for (;;) {
        th->searching = false;
        pthread_cond_broadcast(&pool.cond);
        while (!th->searching && !th->exit) {
            pthread_cond_wait(&pool.cond, &pool.mutex);
        }
        if (th->exit) break;
        pthread_mutex_unlock(&pool.mutex);

        if (th->index == 0) {
            main_thread_search(th);
        } else {
            search_iterate(&st->board, &st->info);
        }

        pthread_mutex_lock(&pool.mutex);
    }
    th->searching = false;
    pthread_cond_broadcast(&pool.cond);
    pthread_mutex_unlock(&pool.mutex);

    state_free(st);
    return NULL;
}

// -----------------------------------------------------------------------------
// Interface for the UCI thread
// -----------------------------------------------------------------------------

void threads_wait(void) {
    pthread_mutex_lock(&pool.mutex);
    while (pool.count > 0 && pool.threads[0]->searching) {
        pthread_cond_wait(&pool.cond, &pool.mutex);
    }
    pthread_mutex_unlock(&pool.mutex);
}

bool threads_searching(void) {
    pthread_mutex_lock(&pool.mutex);
    bool searching = pool.count > 0 && pool.threads[0]->searching;
    pthread_mutex_unlock(&pool.mutex);
    return searching;
}

bool threads_search_unbounded(void) {
    pthread_mutex_lock(&pool.mutex);
    bool unbounded = pool.count > 0 && pool.threads[0]->searching &&
                     (pool.infinite || atomic_load(&pool.shared.ponder));
    pthread_mutex_unlock(&pool.mutex);
    return unbounded;
}

void threads_stop(void) {
    pthread_mutex_lock(&pool.mutex);
    atomic_store(&pool.shared.stop, true);
    pthread_cond_broadcast(&pool.cond);
    pthread_mutex_unlock(&pool.mutex);
}

void threads_ponderhit(void) {
    pthread_mutex_lock(&pool.mutex);
    atomic_store(&pool.shared.ponder, false);
    pthread_cond_broadcast(&pool.cond);
    pthread_mutex_unlock(&pool.mutex);
}

int threads_count(void) {
    return pool.count;
}

void threads_destroy(void) {
    if (pool.count == 0) return;
    threads_stop();
    threads_wait();

    pthread_mutex_lock(&pool.mutex);
    for (int i = 0; i < pool.count; i++) {
        pool.threads[i]->exit = true;
    }
    pthread_cond_broadcast(&pool.cond);
    pthread_mutex_unlock(&pool.mutex);

    for (int i = 0; i < pool.count; i++) {
        pthread_join(pool.threads[i]->handle, NULL);
        free(pool.threads[i]);
        pool.threads[i] = NULL;
        pool.infos[i] = NULL;
    }
    pool.count = 0;
}

void threads_set_count(int count) {
    if (count < 1) count = 1;
    if (count > THREADS_MAX) count = THREADS_MAX;
    threads_wait();
    if (count == pool.count) return;
    threads_destroy();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, SEARCH_THREAD_STACK_SIZE);

    for (int i = 0; i < count; i++) {
        SearchThread* th = calloc(1, sizeof(SearchThread));
        if (th == NULL) {
            fprintf(stderr, "Error: Failed to allocate search thread %d\n", i);
            exit(1);
        }
        th->index = i;
        th->searching = true;  // until the thread reports idle for the first time
        // The new thread reads the pool entry under the mutex only, but keep
        // the publication ordered anyway.
        pthread_mutex_lock(&pool.mutex);
        pool.threads[i] = th;
        pool.count = i + 1;
        pthread_mutex_unlock(&pool.mutex);
        if (pthread_create(&th->handle, &attr, thread_main, th) != 0) {
            fprintf(stderr, "Error: Failed to create search thread %d\n", i);
            exit(1);
        }
    }
    pthread_attr_destroy(&attr);

    // Wait until every thread has allocated its state and gone idle.
    pthread_mutex_lock(&pool.mutex);
    for (int i = 0; i < pool.count; i++) {
        while (pool.threads[i]->searching) {
            pthread_cond_wait(&pool.cond, &pool.mutex);
        }
        pool.infos[i] = &pool.threads[i]->state->info;
    }
    pool.shared.threadCount = pool.count;
    pool.shared.threads = pool.infos;
    pthread_mutex_unlock(&pool.mutex);
}

void threads_clear_histories(void) {
    threads_wait();
    // All threads are idle and do not touch their state.
    for (int i = 0; i < pool.count; i++) {
        clear_search_history(&pool.threads[i]->state->info);
    }
}

void threads_start_search(const Board* board, const NNUEAccumulator* acc,
                          const NNUENetwork* net, const SearchRequest* request) {
    threads_wait();
    if (pool.count == 0) return;

    pthread_mutex_lock(&pool.mutex);

    atomic_store(&pool.shared.stop, false);
    atomic_store(&pool.shared.ponder, request->ponder);
    pool.infinite = request->infinite;

    // All threads are idle (the helpers went idle before the main thread did)
    // and only look at their state again once `searching` is set.
    for (int i = 0; i < pool.count; i++) {
        ThreadState* st = pool.threads[i]->state;
        SearchInfo* info = &st->info;
        bool is_main = (i == 0);

        st->board = *board;
        memcpy(&st->rootAcc, acc, sizeof(NNUEAccumulator));
        st->rootAcc.previous = NULL;

        info->startTimeMs = request->startTimeMs;
        // Limits are enforced by the main thread; helpers run until stopped.
        info->softTimeLimit = is_main ? request->softTimeLimit : 0;
        info->hardTimeLimit = is_main ? request->hardTimeLimit : 0;
        info->depthLimit = is_main ? request->depthLimit : 0;
        info->nodeLimit = is_main ? request->nodeLimit : 0;
        info->stopSearch = false;
        info->lastIterationTime = 0;
        info->nnue_acc = &st->rootAcc;
        info->nnue_net = net;
        info->nodesSearched = 0;
        info->bestMoveThisIteration = 0;
        info->bestScoreThisIteration = 0;
        info->seldepth = 0;
        info->params = request->params;
        clear_volatile_history(info);  // ply-indexed state only; history persists

        info->tbProbeLimit = request->tbProbeLimit;
        info->tbHits = 0;
        info->tbRootMoveCount = request->tbRootMoveCount;
        memcpy(info->tbRootMoves, request->tbRootMoves, sizeof(info->tbRootMoves));
        info->tbRootScore = request->tbRootScore;
        info->tbRootMatePlies = request->tbRootMatePlies;
        info->tbRootPvLen = request->tbRootPvLen;
        memcpy(info->tbRootPv, request->tbRootPv, sizeof(info->tbRootPv));

        info->shared = &pool.shared;
        info->isHelper = !is_main;
        atomic_store(&info->publishedNodes, 0);
        atomic_store(&info->publishedTbHits, 0);
    }

    tt_new_search();  // one new TT generation per search, not per thread

    pool.threads[0]->searching = true;
    pthread_cond_broadcast(&pool.cond);
    pthread_mutex_unlock(&pool.mutex);
}
