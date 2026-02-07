#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include "move.h"
#include "nnue.h"
#include <time.h> // For clock_t
#include <stdbool.h>

#define MAX_PLY 64 // Maximum search depth

// =============================================================================
// Tunable Search Parameters (UCI Options)
// =============================================================================

typedef struct {
    // Feature enable flags
    bool use_lmr;              // Enable Late Move Reductions (default: true)
    bool use_null_move;        // Enable Null Move Pruning (default: true)
    bool use_futility;         // Enable Futility Pruning (default: true)
    bool use_rfp;              // Enable Reverse Futility Pruning (default: true)
    bool use_delta_pruning;    // Enable Delta Pruning in QSearch (default: true)
    bool use_aspiration;       // Enable Aspiration Windows (default: true)

    // Late Move Reduction parameters
    int lmr_full_depth_moves;  // Number of moves before LMR kicks in (default: 4)
    int lmr_reduction_limit;   // Minimum depth for LMR (default: 3)

    // Null Move Pruning parameters
    int null_move_reduction;   // Depth reduction for null move (default: 3)
    int null_move_min_depth;   // Minimum depth for null move pruning (default: 3)

    // Futility pruning margins
    int futility_margin;       // Depth 1 margin (default: 150)
    int futility_margin_d2;    // Depth 2 margin (default: 300)
    int futility_margin_d3;    // Depth 3 margin (default: 450)

    // Reverse Futility Pruning margins
    int rfp_margin;            // Per depth margin (default: 120)
    int rfp_max_depth;         // Maximum depth for RFP (default: 6)

    // Razoring parameters
    bool use_razoring;         // Enable Razoring (default: true)
    int razor_margin;          // Base margin for razoring (default: 300)

    // Late Move Pruning parameters
    bool use_lmp;              // Enable Late Move Pruning (default: true)
    int lmp_base;              // Base moves before LMP kicks in (default: 3)

    // Delta pruning margin for quiescence
    int delta_margin;          // (default: 200)

    // Aspiration window
    int aspiration_window;     // (default: 50)
} SearchParams;

// Initialize SearchParams with default values
void search_params_init(SearchParams* params);

// Silent mode - disables info and debug output (for training)
extern bool search_silent_mode;
void set_search_silent(bool silent);
#define MAX_KILLERS 2  // Number of killer moves per ply

typedef struct {
    clock_t startTime;
    long softTimeLimit;  // Zeit, nach der keine neue Tiefe begonnen wird
    long hardTimeLimit;  // Absolutes Zeitlimit (Abbruch der Suche)
    bool stopSearch;
    uint64_t nodesSearched;
    Move bestMoveThisIteration;
    int bestScoreThisIteration;
    
    // PV table
    Move pv_table[MAX_PLY][MAX_PLY]; // For storing the Principal Variation
    int pv_length[MAX_PLY];          // Length of PV at each ply
    
    // Killer moves (quiet moves that caused beta cutoffs)
    Move killers[MAX_PLY][MAX_KILLERS];
    
    // History heuristic (indexed by [side][from][to])
    int history[2][64][64];

    // Counter moves (indexed by [prev_from][prev_to] -> best response)
    Move counter_moves[64][64];

    // Previous move at each ply (for counter move heuristic)
    Move prev_moves[MAX_PLY];
    
    // NNUE accumulator and network for incremental updates
    NNUEAccumulator* nnue_acc;
    const NNUENetwork* nnue_net;
    
    long lastIterationTime;  // Zeit der letzten Iteration für Vorhersage
    int seldepth;            // Selective depth (max depth reached)
    int depthLimit;          // Maximum search depth (0 = no limit)
    uint64_t nodeLimit;      // Maximum nodes to search (0 = no limit)
    
    // Tunable search parameters
    SearchParams params;
} SearchInfo;

Move iterative_deepening_search(Board* board, SearchInfo* info);
int alpha_beta_search(Board* board, int depth, int alpha, int beta, bool maximizingPlayer, SearchInfo* info, int ply);
int quiescence_search(Board* board, int alpha, int beta, bool maximizingPlayer, SearchInfo* info, int ply);
void clear_search_history(SearchInfo* info);

#define MATE_SCORE 1000000 // Arbitrary large score for checkmate

#endif // SEARCH_H
