#ifndef MOVE_GENERATOR_H
#define MOVE_GENERATOR_H

#include "board.h"
#include "move.h"

// Initialize magic bitboards and other precomputed data
void initMoveGenerator();

// Called by initMoveGenerator to build the magic attack tables from the precomputed magics
// Returns true on success, false if a magic number does not map its occupancies collision-free
bool findAndInitMagicNumbers();

// Magic lookup entry: attacks[((occupancy & mask) * magic) >> shift]
typedef struct {
    Bitboard mask;
    Bitboard magic;
    Bitboard* attacks;
    int shift;
} MagicEntry;

extern MagicEntry g_rook_magics[64];
extern MagicEntry g_bishop_magics[64];

// Generate all pseudo-legal moves for the current player
void generateMoves(const Board* board, MoveList* moveList);

// Generate all legal moves for the current player (slower, but guaranteed legal)
void generateLegalMoves(Board* board, MoveList* moveList);

// Generate only pseudo-legal capture moves for the current player
void generateCaptureMoves(const Board* board, MoveList* moveList);

// Generate only pseudo-legal capture and promotion moves for the current player
void generateCaptureAndPromotionMoves(const Board* board, MoveList* moveList);

// Generate only pseudo-legal quiet moves (no captures, no promotions).
// Together with generateCaptureAndPromotionMoves this covers exactly the
// move set of generateMoves.
void generateQuietMoves(const Board* board, MoveList* moveList);

// Check whether a move is pseudo-legal in the given position, i.e. whether
// generateMoves would emit exactly this move (used to validate TT moves)
bool moveIsPseudoLegal(const Board* board, Move move);

// Functions to get attacks for sliding pieces (using magic bitboards)
static inline Bitboard getRookAttacks(Square square, Bitboard occupancy) {
    const MagicEntry* m = &g_rook_magics[square];
    return m->attacks[((occupancy & m->mask) * m->magic) >> m->shift];
}

static inline Bitboard getBishopAttacks(Square square, Bitboard occupancy) {
    const MagicEntry* m = &g_bishop_magics[square];
    return m->attacks[((occupancy & m->mask) * m->magic) >> m->shift];
}
static inline Bitboard getQueenAttacks(Square square, Bitboard occupancy) { // Combines rook and bishop
    return getRookAttacks(square, occupancy) | getBishopAttacks(square, occupancy);
}
bool isKingAttacked(const Board* board, bool isWhite);
// static inline int pop_lsb(Bitboard *bb); // Removed static inline declaration from header
// static inline int get_lsb_index(Bitboard bb); // Removed static inline declaration from header


#endif // MOVE_GENERATOR_H
