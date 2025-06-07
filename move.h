#ifndef MOVE_H
#define MOVE_H

#include <stdint.h>
#include <stdio.h> // For sprintf in moveToString
#include "board.h" // For Square type and PieceTypeToken enum

// Move structure
// Bits:
// 0-5: from_square (6 bits)
// 6-11: to_square (6 bits)
// 12-14: promotion_piece (3 bits: N, B, R, Q) - 0 if no promotion
// 15: capture_flag (1 bit)
// 16: double_pawn_push_flag (1 bit)
// 17: en_passant_flag (1 bit)
// 18: castling_flag (1 bit)
typedef uint32_t Move;

// Helper macros for Move
#define MOVE_FROM(m) (m & 0x3F)
#define MOVE_TO(m) ((m >> 6) & 0x3F)
#define MOVE_PROMOTION(m) ((m >> 12) & 0x7) // 001 N, 010 B, 011 R, 100 Q
#define MOVE_IS_CAPTURE(m) ((m >> 15) & 0x1)
#define MOVE_IS_DOUBLE_PAWN_PUSH(m) ((m >> 16) & 0x1)
#define MOVE_IS_EN_PASSANT(m) ((m >> 17) & 0x1)
#define MOVE_IS_CASTLING(m) ((m >> 18) & 0x1)
#define MOVE_IS_PROMOTION(m) (MOVE_PROMOTION(m) != 0) // Added this line

#define CREATE_MOVE(from, to, promotion, capture, double_push, en_passant, castling) \
    ((Move)(from) | ((Move)(to) << 6) | ((Move)(promotion) << 12) | \
     ((Move)(capture) << 15) | ((Move)(double_push) << 16) | \
     ((Move)(en_passant) << 17) | ((Move)(castling) << 18))

// Promotion piece types for the Move structure
#define PROMOTION_N 1
#define PROMOTION_B 2
#define PROMOTION_R 3
#define PROMOTION_Q 4

// MoveList structure
#define MAX_MOVES 256 // A reasonable upper bound for moves from one position
typedef struct {
    Move moves[MAX_MOVES];
    int count;
} MoveList;

// Function to add a move to the list
static inline void addMove(MoveList* list, Move move) {
    if (list->count < MAX_MOVES) {
        list->moves[list->count++] = move;
    }
    // Consider error handling if MAX_MOVES is exceeded
}

// Function to print a move (for debugging)
// e.g., "e2e4", "e7e8q" (promotion to queen)
void printMove(Move move);
char* moveToString(Move move, char* strBuffer); // User provides buffer
void squareToString(Square sq, char* strBuffer); // Declaration for squareToString

#endif // MOVE_H
