#ifndef EVALUATION_H
#define EVALUATION_H

#include "board.h"
#include "nnue.h"

// Piece values (for fallback/classical evaluation)
#define PAWN_VALUE 100
#define KNIGHT_VALUE 320
#define BISHOP_VALUE 330
#define ROOK_VALUE 500
#define QUEEN_VALUE 900
#define KING_VALUE 20000 // King value is effectively infinite in terms of material

// NNUE evaluation (main evaluation function)
int evaluate(const Board* board);

// NNUE evaluation with accumulator (for search with incremental updates)
int evaluate_nnue(const Board* board, NNUEAccumulator* acc);

// Classical/HCE evaluation (fallback)
int evaluate_classical(const Board* board);

// Initialize evaluation (loads NNUE if available)
void eval_init(const char* nnue_path);

#endif // EVALUATION_H
