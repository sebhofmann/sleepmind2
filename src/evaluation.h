#ifndef EVALUATION_H
#define EVALUATION_H

#include "board.h"
#include "nnue.h"
#include "move.h"

// Piece values (for fallback/classical evaluation)
#define PAWN_VALUE 100
#define KNIGHT_VALUE 320
#define BISHOP_VALUE 330
#define ROOK_VALUE 500
#define QUEEN_VALUE 900
#define KING_VALUE 20000 // King value is effectively infinite in terms of material

// Classical/HCE evaluation (fallback)
int evaluate_classical(const Board* board);

// NNUE evaluation with accumulator and network (for search with incremental updates)
int nnue_evaluate(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net);

// Main evaluation function - use NNUE if available with provided accumulator and network
// If nnue_acc or net is NULL, falls back to classical evaluation
int evaluate(const Board* board, NNUEAccumulator* nnue_acc, const NNUENetwork* nnue_net);

// Initialize evaluation (loads NNUE into provided network if available)
void eval_init(const char* nnue_path, NNUENetwork* net);

// Debug: Set last move for mismatch tracking
void eval_set_last_move(Move m, int from, int to);

#endif // EVALUATION_H
