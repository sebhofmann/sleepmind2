#ifndef EVALUATION_H
#define EVALUATION_H

#include "board.h"

// Piece values
#define PAWN_VALUE 100
#define KNIGHT_VALUE 320
#define BISHOP_VALUE 330
#define ROOK_VALUE 500
#define QUEEN_VALUE 900
#define KING_VALUE 20000 // King value is effectively infinite in terms of material

int evaluate(const Board* board);

#endif // EVALUATION_H
