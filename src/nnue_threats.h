#ifndef NNUE_THREATS_H
#define NNUE_THREATS_H

// Threat and pawn-pair input features of the threats net (NNUE_ARCH=threats).
// Must produce exactly the indices of training/threat_inputs.rs (same feature space as
// bullet's examples/advanced / pawnocchio).
//
// A perspective sees squares as sq ^ mask, mask = (56 for black) ^ (7 if its king is on files e-h),
// and colours relative to itself. Index layout:
//   [0, 4560)       pawn pairs: unordered pairs of pawns (any colours) at most one file apart
//   [4560, 64368)   threats: own attackers first, then the opponent's (29904 each)

#include "board.h"
#include <stdint.h>

#define NNUE_TP_PAIRS            4560
#define NNUE_TP_THREATS_PER_SIDE 29904
#define NNUE_TP_INPUTS           (NNUE_TP_PAIRS + 2 * NNUE_TP_THREATS_PER_SIDE)
// 16 pawns -> 120 pairs; each of at most 30 non-king pieces attacks at most 8 occupied squares
#define NNUE_TP_MAX_PAIRS        120
#define NNUE_TP_MAX_THREATS      256

// Builds the index tables (idempotent, called by the net loader)
void nnue_tp_init(void);

// Square mask of a perspective (0 = white, 1 = black) for the current king position
static inline int nnue_tp_square_mask(const Board* board, int perspective) {
    const int king = __builtin_ctzll(board->byTypeBB[perspective][KING]);
    return (perspective ? 56 : 0) ^ ((king & 7) >= 4 ? 7 : 0);
}

// Raw threats in board coordinates, independent of the perspective, packed as
//   (attacker colour * 6 + attacker type) << 20 | from << 14 | to << 8 | victim piece (enum Piece)
// Only attacker/victim types that can form a feature are listed. The list comes out sorted
// ascending, so two positions can be diffed with a merge. Returns the count (<= NNUE_TP_MAX_THREATS).
int nnue_tp_raw_threats(const Board* board, uint32_t* out);

// Feature indices of raw threats for one perspective; threats that are no feature in this
// perspective (same-type attack towards a higher square) are skipped. Returns the count.
int nnue_tp_threat_indices(const uint32_t* raw, int n, int perspective, int mask, uint16_t* out);

// Threat/pawn-pair changes of one move as raw tuples (packing as above), see nnue_tp_record_move
#define NNUE_TP_MAX_DELTA        160
#define NNUE_TP_MAX_PAIR_DELTA   32   // removed: <= 2 x 15 + 1, added: <= 15
typedef struct {
    uint32_t add[NNUE_TP_MAX_DELTA];
    uint32_t sub[NNUE_TP_MAX_DELTA];
    uint16_t pair_add[NNUE_TP_MAX_PAIR_DELTA];
    uint16_t pair_sub[NNUE_TP_MAX_PAIR_DELTA];
    int16_t n_add, n_sub;
    int8_t n_pair_add, n_pair_sub;
    bool overflow;
} NNUETPDelta;

// Emits the threat and pawn-pair changes of a move (after pawnocchio's threat updates), called on
// the board AFTER the move. moving_piece / captured_piece as enum Piece (NO_PIECE = no capture),
// capture_sq differs from `to` only for en passant, castling: `to` is the king's target square.
// Returns false if the buffers overflowed (the caller then rebuilds from scratch).
bool nnue_tp_record_move(const Board* board, int from, int to, int moving_piece, int captured_piece,
                         int capture_sq, bool castling, NNUETPDelta* delta);

// Active threat features of both perspectives (masks from nnue_tp_square_mask)
void nnue_tp_threats(const Board* board, const int mask[2], uint16_t* out[2], int count[2]);

// Raw pawn pairs in board coordinates, packed as a << 8 | b << 2 | colour(a) << 1 | colour(b)
// with a < b, sorted ascending like the raw threats. Returns the count (<= NNUE_TP_MAX_PAIRS).
int nnue_tp_raw_pairs(const Board* board, uint16_t* out);

// Feature indices of raw pawn pairs for one perspective. Returns the count (= n).
int nnue_tp_pair_indices(const uint16_t* raw, int n, int perspective, int mask, uint16_t* out);

// Active pawn-pair features of both perspectives
void nnue_tp_pairs(const Board* board, const int mask[2], uint16_t* out[2], int count[2]);

#endif // NNUE_THREATS_H
