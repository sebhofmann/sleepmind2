#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include "move.h"
#include "nnue.h"
#include <time.h> // For clock_t
#include <stdbool.h>

#define MAX_PLY 64 // Maximum search depth

// Silent mode - disables info and debug output (for training)
extern bool search_silent_mode;
void set_search_silent(bool silent);
#define MAX_KILLERS 2  // Number of killer moves per ply

typedef struct {
    clock_t startTime;
    long softTimeLimit;  // Zeit, nach der keine neue Tiefe begonnen wird
    long hardTimeLimit;  // Absolutes Zeitlimit (Abbruch der Suche)
    bool stopSearch;
    int nodesSearched;
    Move bestMoveThisIteration;
    int bestScoreThisIteration;
    
    // PV table
    Move pv_table[MAX_PLY][MAX_PLY]; // For storing the Principal Variation
    int pv_length[MAX_PLY];          // Length of PV at each ply
    
    // Killer moves (quiet moves that caused beta cutoffs)
    Move killers[MAX_PLY][MAX_KILLERS];
    
    // History heuristic (indexed by [side][from][to])
    int history[2][64][64];
    
    // Counter moves (indexed by [piece][to_square])
    Move counter_moves[12][64];
    
    // NNUE accumulator and network for incremental updates
    NNUEAccumulator* nnue_acc;
    const NNUENetwork* nnue_net;
    
    long lastIterationTime;  // Zeit der letzten Iteration für Vorhersage
    int seldepth;            // Selective depth (max depth reached)
    int depthLimit;          // Maximum search depth (0 = no limit)
} SearchInfo;

Move iterative_deepening_search(Board* board, SearchInfo* info);
int alpha_beta_search(Board* board, int depth, int alpha, int beta, bool maximizingPlayer, SearchInfo* info, int ply);
int quiescence_search(Board* board, int alpha, int beta, bool maximizingPlayer, SearchInfo* info, int ply);
void clear_search_history(SearchInfo* info);

#define MATE_SCORE 1000000 // Arbitrary large score for checkmate

#endif // SEARCH_H
