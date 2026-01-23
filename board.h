#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>  // For NULL

typedef uint64_t Bitboard;

// Castling rights constants
#define NO_CASTLING 0
#define WHITE_KINGSIDE_CASTLE 1  // K
#define WHITE_QUEENSIDE_CASTLE 2 // Q
#define BLACK_KINGSIDE_CASTLE 4  // k
#define BLACK_QUEENSIDE_CASTLE 8 // q

// =============================================================================
// Stockfish-style piece representation
// =============================================================================

// Colors
enum Color { WHITE = 0, BLACK = 1, COLOR_NB = 2 };

// Piece types (0-based for array indexing)
enum PieceType { 
    PAWN = 0, KNIGHT = 1, BISHOP = 2, ROOK = 3, QUEEN = 4, KING = 5, 
    PIECE_TYPE_NB = 6, NO_PIECE_TYPE_IDX = 6 
};

// Piece constants for pieceOnSquare array (0 = empty, 1-6 = white, 7-12 = black)
enum Piece {
    NO_PIECE = 0,
    W_PAWN = 1, W_KNIGHT = 2, W_BISHOP = 3, W_ROOK = 4, W_QUEEN = 5, W_KING = 6,
    B_PAWN = 7, B_KNIGHT = 8, B_BISHOP = 9, B_ROOK = 10, B_QUEEN = 11, B_KING = 12,
    PIECE_NB = 13
};

// Branchless piece type extraction: (p-1) - 6 if black
// Uses arithmetic: (p-1) - 6*((p-1)/6) but since we know (p-1)/6 is 0 for white, 1 for black
// We can use: (p-1) - 6*((p)>>3) since p>=8 iff black (7 is edge case but 7>>3=0, fixed below)
// Actually: white 1-6, black 7-12. (p>6) is 0 or 1. So: (p-1) - 6*(p>6)
#define PIECE_TYPE_OF(p) ((int)(p) - 1 - 6 * ((int)(p) > 6))
#define PIECE_COLOR_OF(p) ((int)((p) > 6))                  // 0=white, 1=black (branchless)
#define MAKE_PIECE_NEW(type, color) ((type) + 1 + ((color) * 6))
#define PIECE_IS_VALID(p) ((p) >= W_PAWN && (p) <= B_KING)

// Legacy macros for compatibility
#define PIECE_TYPE(p) ((p) > 6 ? (p) - 6 : (p))           // Extract piece type (1-6)
#define PIECE_COLOR(p) ((p) > 6 ? 1 : ((p) > 0 ? 0 : -1)) // 0=white, 1=black, -1=no piece
#define MAKE_PIECE(type, isWhite) ((isWhite) ? (type) : ((type) + 6))
#define PIECE_IS_WHITE(p) ((p) >= W_PAWN && (p) <= W_KING)
#define PIECE_IS_BLACK(p) ((p) >= B_PAWN && (p) <= B_KING)

// Square Constants
enum Squares {
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
    SQ_NONE // Represents no square or an invalid square
};

// =============================================================================
// Board structure - Stockfish-style with indexed bitboards
// =============================================================================
typedef struct {
    // PRIMARY: Square-indexed piece array for O(1) lookup
    uint8_t piece[64];
    
    // SECONDARY: Bitboards for move generation (indexed by [color][pieceType])
    Bitboard byTypeBB[2][6];   // [WHITE/BLACK][PAWN..KING]
    Bitboard byColorBB[2];     // All pieces of each color
    
    bool whiteToMove;
    uint8_t castlingRights;
    int halfMoveClock;
    int fullMoveNumber;
    int enPassantSquare;
    uint64_t zobristKey; 
    uint64_t history[1000]; 
    int historyIndex; 
} Board;

// For compatibility with old code
#define pieceOnSquare piece

// Legacy bitboard compatibility macros (reference the new indexed structure)
#define whitePawns   byTypeBB[WHITE][PAWN]
#define whiteKnights byTypeBB[WHITE][KNIGHT]
#define whiteBishops byTypeBB[WHITE][BISHOP]
#define whiteRooks   byTypeBB[WHITE][ROOK]
#define whiteQueens  byTypeBB[WHITE][QUEEN]
#define whiteKings   byTypeBB[WHITE][KING]
#define blackPawns   byTypeBB[BLACK][PAWN]
#define blackKnights byTypeBB[BLACK][KNIGHT]
#define blackBishops byTypeBB[BLACK][BISHOP]
#define blackRooks   byTypeBB[BLACK][ROOK]
#define blackQueens  byTypeBB[BLACK][QUEEN]
#define blackKings   byTypeBB[BLACK][KING]

// Define squares (0-63)
typedef int Square;

// Define piece types (for move.h compatibility)
typedef enum {
    NO_PIECE_TYPE, PAWN_T, KNIGHT_T, BISHOP_T, ROOK_T, QUEEN_T, KING_T
} PieceTypeToken;

// =============================================================================
// BRANCHLESS piece manipulation - the core hotpath functions
// =============================================================================

// Get piece at square (O(1) lookup) - THE primary lookup
static inline uint8_t get_piece(const Board* board, Square sq) {
    return board->piece[sq];
}

// Put a piece on a square - BRANCHLESS (updates piece array + byTypeBB)
static inline void put_piece(Board* board, uint8_t p, Square sq) {
    int color = PIECE_COLOR_OF(p);
    int type = PIECE_TYPE_OF(p);
    board->piece[sq] = p;
    board->byTypeBB[color][type] |= (1ULL << sq);
}

// Remove a piece from a square - BRANCHLESS
static inline void remove_piece_fast(Board* board, Square sq) {
    uint8_t p = board->piece[sq];
    if (p == NO_PIECE) return;
    int color = PIECE_COLOR_OF(p);
    int type = PIECE_TYPE_OF(p);
    board->byTypeBB[color][type] &= ~(1ULL << sq);
    board->piece[sq] = NO_PIECE;
}

// Move a piece - BRANCHLESS, optimal for hotpath
static inline void move_piece_fast(Board* board, Square from, Square to) {
    uint8_t p = board->piece[from];
    int color = PIECE_COLOR_OF(p);
    int type = PIECE_TYPE_OF(p);
    Bitboard fromTo = (1ULL << from) | (1ULL << to);
    
    board->byTypeBB[color][type] ^= fromTo;
    board->piece[from] = NO_PIECE;
    board->piece[to] = p;
}

// Pop a piece (remove and return it) - for captures
static inline uint8_t pop_piece(Board* board, Square sq) {
    uint8_t p = board->piece[sq];
    if (p == NO_PIECE) return NO_PIECE;
    int color = PIECE_COLOR_OF(p);
    int type = PIECE_TYPE_OF(p);
    board->byTypeBB[color][type] &= ~(1ULL << sq);
    board->piece[sq] = NO_PIECE;
    return p;
}

// =============================================================================
// Legacy compatibility helpers (use new branchless versions internally)
// =============================================================================

static inline Bitboard* get_piece_bb(Board* board, int pieceType, bool isWhite) {
    // pieceType is 1-6 (PAWN_T..KING_T), convert to 0-5
    return &board->byTypeBB[isWhite ? WHITE : BLACK][pieceType - 1];
}

static inline void remove_piece(Board* board, int pieceType, bool isWhite, Square sq) {
    int color = isWhite ? WHITE : BLACK;
    int type = pieceType - 1;  // Convert from PAWN_T(1) to PAWN(0)
    board->byTypeBB[color][type] &= ~(1ULL << sq);
    board->piece[sq] = NO_PIECE;
}

static inline void clear_piece_array(Board* board) {
    for (int i = 0; i < 64; i++) {
        board->piece[i] = NO_PIECE;
    }
    for (int c = 0; c < 2; c++) {
        board->byColorBB[c] = 0;
        for (int t = 0; t < 6; t++) {
            board->byTypeBB[c][t] = 0;
        }
    }
}

// Sync byColorBB from byTypeBB (call after setting up byTypeBB)
static inline void sync_color_bitboards(Board* board) {
    board->byColorBB[WHITE] = board->byTypeBB[WHITE][PAWN] | board->byTypeBB[WHITE][KNIGHT] |
                               board->byTypeBB[WHITE][BISHOP] | board->byTypeBB[WHITE][ROOK] |
                               board->byTypeBB[WHITE][QUEEN] | board->byTypeBB[WHITE][KING];
    board->byColorBB[BLACK] = board->byTypeBB[BLACK][PAWN] | board->byTypeBB[BLACK][KNIGHT] |
                               board->byTypeBB[BLACK][BISHOP] | board->byTypeBB[BLACK][ROOK] |
                               board->byTypeBB[BLACK][QUEEN] | board->byTypeBB[BLACK][KING];
}

// Sync piece array from bitboards
static inline void sync_piece_array_from_bitboards(Board* board) {
    for (int sq = 0; sq < 64; sq++) {
        board->piece[sq] = NO_PIECE;
        Bitboard bit = 1ULL << sq;
        for (int c = 0; c < 2; c++) {
            for (int t = 0; t < 6; t++) {
                if (board->byTypeBB[c][t] & bit) {
                    board->piece[sq] = MAKE_PIECE_NEW(t, c);
                    goto next_sq;
                }
            }
        }
        next_sq:;
    }
}

#endif // BOARD_H

