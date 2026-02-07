#ifndef ZOBRIST_H
#define ZOBRIST_H
#include "board.h"

// --- Zobrist Keys ---
// Flat array layout: [pieceType-1][color][square]
// Index = (pieceType-1) * 128 + colorIdx * 64 + square
// pieceType: PAWN_T=1, KNIGHT_T=2, BISHOP_T=3, ROOK_T=4, QUEEN_T=5, KING_T=6
// colorIdx: 0=white, 1=black
// Total: 6 piece types * 2 colors * 64 squares = 768 entries
extern uint64_t zobrist_piece_keys_flat[768];
extern uint64_t zobrist_castling_keys[16];
extern uint64_t zobrist_enpassant_keys[64];
extern uint64_t zobrist_side_to_move_key;

// Optimized index: only 2 shifts
// (pieceType-1) * 128 + colorIdx * 64 + square
#define ZOBRIST_PIECE_INDEX(pieceType, colorIdx, square) \
    ((((pieceType) - 1) << 7) + ((colorIdx) << 6) + (square))

#define ZOBRIST_PIECE_KEY(pieceType, colorIdx, square) \
    zobrist_piece_keys_flat[ZOBRIST_PIECE_INDEX(pieceType, colorIdx, square)]

uint64_t calculate_zobrist_key(const Board* board);


#endif