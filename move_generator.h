#ifndef MOVE_GENERATOR_H
#define MOVE_GENERATOR_H

#include "board.h"
#include "move.h"

// Initialize magic bitboards and other precomputed data
void initMoveGenerator();

// Called by initMoveGenerator to find magic numbers
// Returns true on success, false on failure (e.g., too many attempts)
bool findAndInitMagicNumbers();

// Generate all pseudo-legal moves for the current player
void generateMoves(const Board* board, MoveList* moveList);

// Generate only pseudo-legal capture moves for the current player
void generateCaptureMoves(const Board* board, MoveList* moveList);

// Generate only pseudo-legal capture and promotion moves for the current player
void generateCaptureAndPromotionMoves(const Board* board, MoveList* moveList);

// Functions to get attacks for sliding pieces (using magic bitboards)
Bitboard getRookAttacks(Square square, Bitboard occupancy);
Bitboard getBishopAttacks(Square square, Bitboard occupancy);
Bitboard getQueenAttacks(Square square, Bitboard occupancy); // Combines rook and bishop
bool isKingAttacked(const Board* board, bool isWhite);
static inline int pop_lsb(Bitboard *bb);
static inline int get_lsb_index(Bitboard bb);


#endif // MOVE_GENERATOR_H
