#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include "move.h"
#include <time.h> // For clock_t

typedef struct {
    clock_t startTime;
    long timeLimit; // in milliseconds
    bool stopSearch;
    int nodesSearched;
    Move bestMoveThisIteration;
    int bestScoreThisIteration;
    // TranspositionTable* tt; // Will be added later
} SearchInfo;

Move iterative_deepening_search(Board* board, SearchInfo* info);
int alpha_beta_search(Board* board, int depth, int alpha, int beta, bool maximizingPlayer, SearchInfo* info, int ply);
int quiescence_search(Board* board, int alpha, int beta, bool maximizingPlayer, SearchInfo* info, int ply);

#endif // SEARCH_H
