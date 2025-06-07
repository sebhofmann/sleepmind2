#include "evaluation.h"
#include "bitboard_utils.h" // For POPCOUNT

// Basic material evaluation
int evaluate(const Board* board) {
    int score = 0;

    // Material count
    score += POPCOUNT(board->whitePawns) * PAWN_VALUE;
    score += POPCOUNT(board->whiteKnights) * KNIGHT_VALUE;
    score += POPCOUNT(board->whiteBishops) * BISHOP_VALUE;
    score += POPCOUNT(board->whiteRooks) * ROOK_VALUE;
    score += POPCOUNT(board->whiteQueens) * QUEEN_VALUE;

    score -= POPCOUNT(board->blackPawns) * PAWN_VALUE;
    score -= POPCOUNT(board->blackKnights) * KNIGHT_VALUE;
    score -= POPCOUNT(board->blackBishops) * BISHOP_VALUE;
    score -= POPCOUNT(board->blackRooks) * ROOK_VALUE;
    score -= POPCOUNT(board->blackQueens) * QUEEN_VALUE;
    
    // TODO: Add piece-square tables
    // TODO: Add mobility
    // TODO: Add king safety
    // TODO: Add pawn structure evaluation

    return board->whiteToMove ? score : -score;
}
