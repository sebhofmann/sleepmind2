#include "board.h"          // Expected to define Board struct and U64
#include "bitboard_utils.h" // For POPCOUNT
#include "evaluation.h"     // For function signature

#include <stdbool.h>
#include <math.h>   // For fabs, fmin, fmax for game phase, abs for integer comparison
#include <stdlib.h> // For abs with integers if math.h one is for doubles

// Fallback U64 definition if not in board.h or bitboard_utils.h
#ifndef U64
typedef unsigned long long U64;
#endif

// Helper for LSB index
#if defined(__GNUC__) || defined(__clang__)
#define get_lsb_index(bb) ( (bb) == 0 ? -1 : __builtin_ctzll(bb) )
#else
// Basic fallback for get_lsb_index
static int get_lsb_index_fallback(U64 bb) {
    if (bb == 0) return -1;
    int count = 0;
    while (!((bb >> count) & 1)) {
        count++;
        if (count == 64) return -1; 
    }
    return count;
}
#define get_lsb_index(bb) get_lsb_index_fallback(bb)
#endif

// Piece types for indexing PIECE_VALUES
typedef enum {
    C_PIECE_NONE, C_PIECE_KING, C_PIECE_QUEEN, C_PIECE_PAWN, 
    C_PIECE_BISHOP, C_PIECE_KNIGHT, C_PIECE_ROOK
} CPieceType;

// Material values from Java template
static const int PIECE_VALUES[] = {
    0,    // NONE
    20000,// KING (for game phase calculation)
    900,  // QUEEN
    100,  // PAWN
    330,  // BISHOP
    320,  // KNIGHT
    510   // ROOK
};

// Game phase thresholds
static const int ENDGAME_MATERIAL_THRESHOLD = 2600;
static const int OPENING_MATERIAL_THRESHOLD = 7000;

// Evaluation weights
static const int CENTER_CONTROL_WEIGHT_OPENING = 20;
static const int CENTER_CONTROL_WEIGHT_ENDGAME = 5;
static const int SPACE_WEIGHT_OPENING = 8;
static const int SPACE_WEIGHT_ENDGAME = 3;

// Piece-Square Tables (PSTs) - White's perspective
// A1=0, H1=7, A8=56, H8=63
static const int PAWN_PST_OPENING[] = {
    0,   0,   0,   0,   0,   0,   0,   0,  5,  10,  10,  -5,  -5,  10,  10,   5,
   10,  10,  15,  20,  20,  15,  10,  10,  5,  10,  20,  25,  25,  20,  10,   5,
    5,   5,  10,  25,  25,  10,   5,   5, 10,  10,  20,  30,  30,  20,  10,  10,
   50,  50,  50,  50,  50,  50,  50,  50,  0,   0,   0,   0,   0,   0,   0,   0
};
static const int PAWN_PST_ENDGAME[] = {
    0,   0,   0,   0,   0,   0,   0,   0, 10,  10,  10,  10,  10,  10,  10,  10,
   10,  10,  10,  10,  10,  10,  10,  10, 20,  20,  20,  20,  20,  20,  20,  20,
   30,  30,  30,  30,  30,  30,  30,  30, 50,  50,  50,  50,  50,  50,  50,  50,
   80,  80,  80,  80,  80,  80,  80,  80,  0,   0,   0,   0,   0,   0,   0,   0
};
static const int KNIGHT_PST_OPENING[] = {
  -40, -15, -30, -30, -30, -30, -15, -40, -40, -20,   0,   5,   5,   0, -20, -40,
  -30,   5,  10,  15,  15,  10,   5, -30, -30,   0,  20,  20,  20,  20,   0, -30,
  -30,   5,  20,  20,  20,  20,   5, -30, -30,   0,  10,  15,  15,  10,   0, -30,
  -40, -20,   0,   0,   0,   0, -20, -40, -50, -40, -30, -30, -30, -30, -40, -50
};
static const int KNIGHT_PST_ENDGAME[] = {
  -50, -40, -20, -20, -20, -20, -40, -50, -40, -20,   0,   5,   5,   0, -20, -40,
  -30,   5,  10,  15,  15,  10,   5, -30, -30,   0,  15,  20,  20,  15,   0, -30,
  -30,   5,  15,  20,  20,  15,   5, -30, -30,   0,  10,  15,  15,  10,   0, -30,
  -40, -20,   0,   0,   0,   0, -20, -40, -50, -40, -30, -30, -30, -30, -40, -50
};
static const int BISHOP_PST_OPENING[] = {
  -20, -10, -10, -10, -10, -10, -10, -20, -10,  10,   0,   0,   0,   0,  10, -10,
  -10,  10,  10,  10,  10,  10,  10, -10, -10,   0,  10,  10,  10,  10,   0, -10,
  -10,   5,   5,  10,  10,   5,   5, -10, -10,   0,   5,  10,  10,   5,   0, -10,
  -10,   0,   0,   0,   0,   0,   0, -10, -20, -10, -10, -10, -10, -10, -10, -20
};
static const int BISHOP_PST_ENDGAME[] = {
  -20, -10, -10, -10, -10, -10, -10, -20, -10,   5,   0,   0,   0,   0,   5, -10,
  -10,  10,  10,  10,  10,  10,  10, -10, -10,   0,  10,  10,  10,  10,   0, -10,
  -10,   5,   5,  10,  10,   5,   5, -10, -10,   0,   5,  10,  10,   5,   0, -10,
  -10,   0,   0,   0,   0,   0,   0, -10, -20, -10, -10, -10, -10, -10, -10, -20
};
static const int ROOK_PST_OPENING[] = {
    0,   0,   0,   5,   5,   0,   0,   0,  -5,   0,   0,   0,   0,   0,   0,  -5,
   -5,   0,   0,   0,   0,   0,   0,  -5,  -5,   0,   0,   0,   0,   0,   0,  -5,
   -5,   0,   0,   0,   0,   0,   0,  -5,  -5,   0,   0,   0,   0,   0,   0,  -5,
   10,  15,  15,  15,  15,  15,  15,  10,   0,   0,   0,   0,   0,   0,   0,   0
};
static const int ROOK_PST_ENDGAME[] = {
    0,   0,   0,   5,   5,   0,   0,   0,  -5,   0,   0,   0,   0,   0,   0,  -5,
   -5,   0,   0,   0,   0,   0,   0,  -5,  -5,   0,   0,   0,   0,   0,   0,  -5,
   -5,   0,   0,   0,   0,   0,   0,  -5,  -5,   0,   0,   0,   0,   0,   0,  -5,
    5,  10,  10,  10,  10,  10,  10,   5,   0,   0,   0,   0,   0,   0,   0,   0
};
static const int QUEEN_PST_OPENING[] = {
  -20, -10, -10,  -5,  -5, -10, -10, -20, -10,   0,   5,   0,   0,   0,   0, -10,
  -10,   5,   5,   5,   5,   5,   0, -10,   0,   0,   5,   5,   5,   5,   0,  -5,
   -5,   0,   5,   5,   5,   5,   0,  -5, -10,   0,   5,   5,   5,   5,   0, -10,
  -10,   0,   0,   0,   0,   0,   0, -10, -20, -10, -10,  -5,  -5, -10, -10, -20
};
static const int QUEEN_PST_ENDGAME[] = { // Same as opening in Java
  -20, -10, -10,  -5,  -5, -10, -10, -20, -10,   0,   5,   0,   0,   0,   0, -10,
  -10,   5,   5,   5,   5,   5,   0, -10,   0,   0,   5,   5,   5,   5,   0,  -5,
   -5,   0,   5,   5,   5,   5,   0,  -5, -10,   0,   5,   5,   5,   5,   0, -10,
  -10,   0,   0,   0,   0,   0,   0, -10, -20, -10, -10,  -5,  -5, -10, -10, -20
};
static const int KING_PST_OPENING[] = {
   20,  30,  10,   0,   0,  10,  30,  20,  20,  20,   0,   0,   0,   0,  20,  20,
  -10, -20, -20, -20, -20, -20, -20, -10, -20, -30, -30, -40, -40, -30, -30, -20,
  -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30,
  -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30
};
static const int KING_PST_ENDGAME[] = {
  -50, -30, -30, -30, -30, -30, -30, -50, -30, -30,   0,   0,   0,   0, -30, -30,
  -30, -10,  20,  30,  30,  20, -10, -30, -30, -10,  30,  40,  40,  30, -10, -30,
  -30, -10,  30,  40,  40,  30, -10, -30, -30, -10,  20,  30,  30,  20, -10, -30,
  -30, -20, -10,   0,   0, -10, -20, -30, -50, -40, -30, -20, -20, -30, -40, -50
};

// Bitboard constants
static const U64 CENTER_SQUARES  = 0x0000001818000000ULL; // d4, e4, d5, e5
static const U64 EXTENDED_CENTER = 0x00003C3C3C3C0000ULL; // c3-f6 rectangle

static const U64 FILE_MASKS[] = {
    0x0101010101010101ULL, 0x0202020202020202ULL, 0x0404040404040404ULL, 0x0808080808080808ULL,
    0x1010101010101010ULL, 0x2020202020202020ULL, 0x4040404040404040ULL, 0x8080808080808080ULL
};
static const U64 RANK_MASKS[] = {
    0x00000000000000FFULL, 0x000000000000FF00ULL, 0x0000000000FF0000ULL, 0x00000000FF000000ULL,
    0x000000FF00000000ULL, 0x0000FF0000000000ULL, 0x00FF000000000000ULL, 0xFF00000000000000ULL
};

// Forward declarations
static U64 get_knight_attacks_c(int square);
static U64 get_pawn_attacks_c(U64 pawns, bool is_white_color);


static int get_total_material(const Board* board) {
    int material = 0;
    material += (POPCOUNT(board->whitePawns) + POPCOUNT(board->blackPawns)) * PIECE_VALUES[C_PIECE_PAWN];
    material += (POPCOUNT(board->whiteKnights) + POPCOUNT(board->blackKnights)) * PIECE_VALUES[C_PIECE_KNIGHT];
    material += (POPCOUNT(board->whiteBishops) + POPCOUNT(board->blackBishops)) * PIECE_VALUES[C_PIECE_BISHOP];
    material += (POPCOUNT(board->whiteRooks) + POPCOUNT(board->blackRooks)) * PIECE_VALUES[C_PIECE_ROOK];
    material += (POPCOUNT(board->whiteQueens) + POPCOUNT(board->blackQueens)) * PIECE_VALUES[C_PIECE_QUEEN];
    return material;
}

static double calculate_game_phase(int total_material_count) {
    if (total_material_count >= OPENING_MATERIAL_THRESHOLD) return 0.0; // Opening
    if (total_material_count <= ENDGAME_MATERIAL_THRESHOLD) return 1.0; // Endgame
    double x = (double)(total_material_count - ENDGAME_MATERIAL_THRESHOLD) / 
               (OPENING_MATERIAL_THRESHOLD - ENDGAME_MATERIAL_THRESHOLD);
    return 1.0 - x; // Interpolate: 0 for opening, 1 for endgame
}

static int evaluate_material_c(const Board* board) {
    int score = 0;
    score += POPCOUNT(board->whitePawns) * PIECE_VALUES[C_PIECE_PAWN];
    score += POPCOUNT(board->whiteKnights) * PIECE_VALUES[C_PIECE_KNIGHT];
    score += POPCOUNT(board->whiteBishops) * PIECE_VALUES[C_PIECE_BISHOP];
    score += POPCOUNT(board->whiteRooks) * PIECE_VALUES[C_PIECE_ROOK];
    score += POPCOUNT(board->whiteQueens) * PIECE_VALUES[C_PIECE_QUEEN];
    score -= POPCOUNT(board->blackPawns) * PIECE_VALUES[C_PIECE_PAWN];
    score -= POPCOUNT(board->blackKnights) * PIECE_VALUES[C_PIECE_KNIGHT];
    score -= POPCOUNT(board->blackBishops) * PIECE_VALUES[C_PIECE_BISHOP];
    score -= POPCOUNT(board->blackRooks) * PIECE_VALUES[C_PIECE_ROOK];
    score -= POPCOUNT(board->blackQueens) * PIECE_VALUES[C_PIECE_QUEEN];
    if (POPCOUNT(board->whiteBishops) >= 2) score += 30;
    if (POPCOUNT(board->blackBishops) >= 2) score -= 30;
    return score;
}

static int evaluate_pst_for_piece(U64 piece_bb, const int open_table[], const int end_table[], 
                                  double game_phase, bool is_black) {
    int score = 0;
    U64 pieces = piece_bb;
    while (pieces != 0) {
        int sq = get_lsb_index(pieces);
        if (sq == -1) break;
        int adj_sq = is_black ? (sq ^ 56) : sq; // Flip for black (A1=0..H8=63 vs A8=0..H1=63)
        int open_val = open_table[adj_sq];
        int end_val = end_table[adj_sq];
        score += (int)(open_val * (1.0 - game_phase) + end_val * game_phase);
        pieces &= (pieces - 1); // Clear LSB
    }
    return score;
}

static int evaluate_piece_square_tables_c(const Board* board, double game_phase) {
    int score = 0;
    // White pieces
    score += evaluate_pst_for_piece(board->whitePawns, PAWN_PST_OPENING, PAWN_PST_ENDGAME, game_phase, false);
    score += evaluate_pst_for_piece(board->whiteKnights, KNIGHT_PST_OPENING, KNIGHT_PST_ENDGAME, game_phase, false);
    score += evaluate_pst_for_piece(board->whiteBishops, BISHOP_PST_OPENING, BISHOP_PST_ENDGAME, game_phase, false);
    score += evaluate_pst_for_piece(board->whiteRooks, ROOK_PST_OPENING, ROOK_PST_ENDGAME, game_phase, false);
    score += evaluate_pst_for_piece(board->whiteQueens, QUEEN_PST_OPENING, QUEEN_PST_ENDGAME, game_phase, false);
    score += evaluate_pst_for_piece(board->whiteKings, KING_PST_OPENING, KING_PST_ENDGAME, game_phase, false);
    // Black pieces
    score -= evaluate_pst_for_piece(board->blackPawns, PAWN_PST_OPENING, PAWN_PST_ENDGAME, game_phase, true);
    score -= evaluate_pst_for_piece(board->blackKnights, KNIGHT_PST_OPENING, KNIGHT_PST_ENDGAME, game_phase, true);
    score -= evaluate_pst_for_piece(board->blackBishops, BISHOP_PST_OPENING, BISHOP_PST_ENDGAME, game_phase, true);
    score -= evaluate_pst_for_piece(board->blackRooks, ROOK_PST_OPENING, ROOK_PST_ENDGAME, game_phase, true);
    score -= evaluate_pst_for_piece(board->blackQueens, QUEEN_PST_OPENING, QUEEN_PST_ENDGAME, game_phase, true);
    score -= evaluate_pst_for_piece(board->blackKings, KING_PST_OPENING, KING_PST_ENDGAME, game_phase, true);
    return score;
}

static int evaluate_pawn_chains_for_color(U64 pawns, bool is_white_color) {
    int score = 0;
    U64 pawns_copy = pawns;
    while (pawns_copy != 0) {
        int square = get_lsb_index(pawns_copy);
        if (square == -1) break;
        U64 pawn_bit = 1ULL << square;
        if ((square % 8) > 0 && (pawns & (pawn_bit >> 1))) score += 3; // Phalanx left
        if ((square % 8) < 7 && (pawns & (pawn_bit << 1))) score += 3; // Phalanx right

        if (is_white_color) {
            if (square >= 9 && (square % 8) != 0 && (pawns & (pawn_bit >> 9))) score += 4; // Support SW
            if (square >= 7 && (square % 8) != 7 && (pawns & (pawn_bit >> 7))) score += 4; // Support SE
        } else {
            if (square <= 54 && (square % 8) != 7 && (pawns & (pawn_bit << 7))) score += 5; // Support NW (Java had 5)
            if (square <= 56 && (square % 8) != 0 && (pawns & (pawn_bit << 9))) score += 5; // Support NE (Java had 5)
        }
        pawns_copy &= (pawns_copy - 1);
    }
    return score;
}

static int evaluate_pawn_structure_for_color(const Board* board, bool is_white_color, double game_phase) {
    U64 pawns = is_white_color ? board->whitePawns : board->blackPawns;
    U64 opponent_pawns = is_white_color ? board->blackPawns : board->whitePawns;
    int score = 0;

    for (int file = 0; file < 8; file++) {
        U64 file_pawns = pawns & FILE_MASKS[file];
        if (file_pawns != 0) {
            int pawn_count_on_file = POPCOUNT(file_pawns);
            if (pawn_count_on_file > 1) score -= 5 * (pawn_count_on_file - 1); // Doubled

            bool has_support = false;
            if (file > 0 && (pawns & FILE_MASKS[file - 1]) != 0) has_support = true;
            if (file < 7 && (pawns & FILE_MASKS[file + 1]) != 0) has_support = true;
            if (!has_support) score -= 10; // Isolated

            // Passed pawn (simplified from Java - check only own file and adjacent for blockers in front)
            U64 current_pawn_iter = file_pawns;
            while(current_pawn_iter != 0){
                int pawn_square = get_lsb_index(current_pawn_iter);
                if(pawn_square == -1) break;
                int rank = pawn_square / 8;
                bool is_passed = true;
                U64 front_span_mask = 0ULL;
                // Create a mask for squares in front of the pawn on its file and adjacent files
                for(int r_scan = rank + (is_white_color ? 1 : -1); 
                    is_white_color ? (r_scan < 8) : (r_scan >=0); 
                    r_scan += (is_white_color ? 1 : -1) ){
                    front_span_mask |= FILE_MASKS[file] & RANK_MASKS[r_scan];
                    if(file > 0) front_span_mask |= FILE_MASKS[file-1] & RANK_MASKS[r_scan];
                    if(file < 7) front_span_mask |= FILE_MASKS[file+1] & RANK_MASKS[r_scan];
                }

                if((opponent_pawns & front_span_mask) != 0){
                    is_passed = false;
                }

                if (is_passed) {
                    int effective_rank = is_white_color ? rank : (7 - rank);
                    score += (20 + effective_rank * 10);
                }
                current_pawn_iter &= (current_pawn_iter-1); // next pawn on this file if any (for doubled passed)
            }
        }
    }
    score += evaluate_pawn_chains_for_color(pawns, is_white_color);
    return score;
}

static int evaluate_pawn_structure_c(const Board* board, double game_phase) {
    int score = 0;
    score += evaluate_pawn_structure_for_color(board, true, game_phase);
    score -= evaluate_pawn_structure_for_color(board, false, game_phase);
    return score;
}

static U64 get_knight_attacks_c(int square) {
    U64 attacks = 0ULL;
    const int knight_moves[] = {17, 15, 10, 6, -17, -15, -10, -6}; // NNE, NNW, ENE, ESE, SSW, SSE, WNW, WSW (relative)
    int x = square % 8;
    int y = square / 8;
    for (int i = 0; i < 8; ++i) {
        int target_sq = square + knight_moves[i];
        if (target_sq < 0 || target_sq > 63) continue;
        int tx = target_sq % 8;
        int ty = target_sq / 8;
        int dx = abs(tx - x);
        int dy = abs(ty - y);
        if ((dx == 1 && dy == 2) || (dx == 2 && dy == 1)) {
            attacks |= (1ULL << target_sq);
        }
    }
    return attacks;
}


static int evaluate_center_control_c(const Board* board, double game_phase) {
    int cc_weight = (int)(CENTER_CONTROL_WEIGHT_OPENING * (1.0 - game_phase) + CENTER_CONTROL_WEIGHT_ENDGAME * game_phase);
    int score = 0;
    U64 white_major_minor = board->whitePawns | board->whiteKnights | board->whiteBishops | board->whiteRooks | board->whiteQueens;
    U64 black_major_minor = board->blackPawns | board->blackKnights | board->blackBishops | board->blackRooks | board->blackQueens;
    score += POPCOUNT(white_major_minor & CENTER_SQUARES) * cc_weight;
    score -= POPCOUNT(black_major_minor & CENTER_SQUARES) * cc_weight;
    score += POPCOUNT(white_major_minor & EXTENDED_CENTER) * cc_weight / 2;
    score -= POPCOUNT(black_major_minor & EXTENDED_CENTER) * cc_weight / 2;
    return score;
}

static int evaluate_space_c(const Board* board, double game_phase) {
    int sp_weight = (int)(SPACE_WEIGHT_OPENING * (1.0 - game_phase) + SPACE_WEIGHT_ENDGAME * game_phase);
    int score = 0;
    // Pawns on 5th, 6th, 7th ranks for white (indices 4,5,6)
    score += POPCOUNT(board->whitePawns & (RANK_MASKS[4] | RANK_MASKS[5] | RANK_MASKS[6])) * sp_weight;
    // Pawns on 4th, 3rd, 2nd ranks for black (indices 3,2,1)
    score -= POPCOUNT(board->blackPawns & (RANK_MASKS[3] | RANK_MASKS[2] | RANK_MASKS[1])) * sp_weight;
    return score;
}

// Main evaluation function
int evaluate(const Board* board) {
    int score = 0;

    // Game phase
    int total_mat = get_total_material(board);
    double game_phase = calculate_game_phase(total_mat);

    // Evaluations
    score += evaluate_material_c(board);
    score += evaluate_piece_square_tables_c(board, game_phase);
    score += evaluate_pawn_structure_c(board, game_phase);
    score += evaluate_center_control_c(board, game_phase);
    //score += evaluate_space_c(board, game_phase);
    
    return score;
}
