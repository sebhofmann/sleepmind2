#ifndef ZOBRIST_H
#define ZOBRIST_H
#include "board.h"

// --- Zobrist Keys ---
// [piece_type][color][square]
extern uint64_t zobrist_piece_keys[KING_T + 1][2][64];
extern uint64_t zobrist_castling_keys[16];
extern uint64_t zobrist_enpassant_keys[64 + 1]; // 64 squares + 1 for no en passant square (index 64)
extern uint64_t zobrist_side_to_move_key; // Key to XOR if it's black to move

uint64_t calculate_zobrist_key(const Board* board);


#endif