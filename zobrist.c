#include "zobrist.h"
#include "board.h"
#include "bitboard_utils.h"

// --- Zobrist Keys ---
// [piece_type][color][square]
uint64_t zobrist_piece_keys[KING_T + 1][2][64]; // Made global
uint64_t zobrist_castling_keys[16]; // Made global
uint64_t zobrist_enpassant_keys[64 + 1]; // Made global
uint64_t zobrist_side_to_move_key; // Made global

static unsigned int tt_rand_seed = 1804289383;

uint64_t random_uint64() {
    uint64_t r = 0;
    for (int i = 0; i < 64; i += 32) {
        tt_rand_seed = tt_rand_seed * 1103515245 + 12345;
        r |= ((uint64_t)(tt_rand_seed / 65536) % (1ULL << 16)) << (i + 16);
        tt_rand_seed = tt_rand_seed * 1103515245 + 12345;
        r |= ((uint64_t)(tt_rand_seed / 65536) % (1ULL << 16)) << i;
    }
    return r;
}

void init_zobrist_keys() {
    for (int pt = PAWN_T; pt <= KING_T; ++pt) {
        for (int color = 0; color < 2; ++color) {
            for (int sq = 0; sq < 64; ++sq) {
                zobrist_piece_keys[pt][color][sq] = random_uint64();
            }
        }
    }
    for (int i = 0; i < 16; ++i) {
        zobrist_castling_keys[i] = random_uint64();
    }
    for (int i = 0; i <= 64; ++i) { // Including "no en passant"
        zobrist_enpassant_keys[i] = random_uint64();
    }
    zobrist_side_to_move_key = random_uint64();
}

uint64_t calculate_zobrist_key(const Board* board) {
    uint64_t key = 0ULL;

    // Piece positions
    for (int sq = 0; sq < 64; ++sq) {
        if (GET_BIT(board->whitePawns, sq)) key ^= zobrist_piece_keys[PAWN_T][0][sq];
        else if (GET_BIT(board->whiteKnights, sq)) key ^= zobrist_piece_keys[KNIGHT_T][0][sq];
        else if (GET_BIT(board->whiteBishops, sq)) key ^= zobrist_piece_keys[BISHOP_T][0][sq];
        else if (GET_BIT(board->whiteRooks, sq)) key ^= zobrist_piece_keys[ROOK_T][0][sq];
        else if (GET_BIT(board->whiteQueens, sq)) key ^= zobrist_piece_keys[QUEEN_T][0][sq];
        else if (GET_BIT(board->whiteKings, sq)) key ^= zobrist_piece_keys[KING_T][0][sq];
        else if (GET_BIT(board->blackPawns, sq)) key ^= zobrist_piece_keys[PAWN_T][1][sq];
        else if (GET_BIT(board->blackKnights, sq)) key ^= zobrist_piece_keys[KNIGHT_T][1][sq];
        else if (GET_BIT(board->blackBishops, sq)) key ^= zobrist_piece_keys[BISHOP_T][1][sq];
        else if (GET_BIT(board->blackRooks, sq)) key ^= zobrist_piece_keys[ROOK_T][1][sq];
        else if (GET_BIT(board->blackQueens, sq)) key ^= zobrist_piece_keys[QUEEN_T][1][sq];
        else if (GET_BIT(board->blackKings, sq)) key ^= zobrist_piece_keys[KING_T][1][sq];
    }

    // Castling rights
    key ^= zobrist_castling_keys[board->castlingRights];

    // En passant square
    if (board->enPassantSquare != SQ_NONE && board->enPassantSquare >=0 && board->enPassantSquare < 64) {
        key ^= zobrist_enpassant_keys[board->enPassantSquare];
    } else {
        key ^= zobrist_enpassant_keys[64]; // Key for no en passant
    }


    // Side to move
    if (!board->whiteToMove) {
        key ^= zobrist_side_to_move_key;
    }

    return key;
}

