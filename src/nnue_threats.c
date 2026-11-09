#include "nnue_threats.h"
#include "move_generator.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

// Victim slot per attacker type (-1 = no feature); kings never attack and are never targets.
//   pawn -> N R;  knight, queen -> P N B R Q;  bishop, rook -> P N B R
static const int8_t TARGET_SLOT[5][6] = {
    { -1, 0, -1, 1, -1, -1 },
    {  0, 1,  2, 3,  4, -1 },
    {  0, 1,  2, 3, -1, -1 },
    {  0, 1,  2, 3, -1, -1 },
    {  0, 1,  2, 3,  4, -1 },
};
static const int SLOTS[5] = { 2, 5, 4, 4, 5 };
#define PAWN_SLOT_SIZE (6 * 14)  // ranks 2-7 x 14 attack directions

static bool tp_ready = false;
static Bitboard pawn_attacks[2][64];
static Bitboard knight_attacks[64];
// empty-board attacks of N, B, R, Q (indexed by piece type) and the number of attacks on lower squares
static Bitboard empty_attacks[5][64];
static uint16_t sq_base[5][64];
// index base inside one attacker side's block per [attacker type][victim type][victim colour], -1 = none
static int32_t slot_base[5][6][2];
// victims an attacker type can have (filled per position from the piece bitboards)
static const int VICTIM_TYPES[5][5] = {
    { KNIGHT, ROOK, -1, -1, -1 },
    { PAWN, KNIGHT, BISHOP, ROOK, QUEEN },
    { PAWN, KNIGHT, BISHOP, ROOK, -1 },
    { PAWN, KNIGHT, BISHOP, ROOK, -1 },
    { PAWN, KNIGHT, BISHOP, ROOK, QUEEN },
};

// Rays without the origin, pawnocchio's direction order: N NE E SE S SW W NW (dir + 4 = opposite)
static Bitboard rays[8][64];
static const int RAY_DIRS[8][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 } };

static Bitboard ray_attacks(int sq, const int dirs[4][2]) {
    Bitboard bb = 0;
    for (int d = 0; d < 4; d++) {
        int r = sq / 8 + dirs[d][0], f = sq % 8 + dirs[d][1];
        while (r >= 0 && r < 8 && f >= 0 && f < 8) {
            bb |= 1ULL << (r * 8 + f);
            r += dirs[d][0];
            f += dirs[d][1];
        }
    }
    return bb;
}

void nnue_tp_init(void) {
    if (tp_ready) return;
    static const int DIAG[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    static const int ORTH[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    static const int KNIGHT_D[8][2] = { { 1, 2 }, { 2, 1 }, { 2, -1 }, { 1, -2 },
                                        { -1, -2 }, { -2, -1 }, { -2, 1 }, { -1, 2 } };
    for (int sq = 0; sq < 64; sq++) {
        const int r = sq / 8, f = sq % 8;
        Bitboard n = 0;
        for (int i = 0; i < 8; i++) {
            const int rr = r + KNIGHT_D[i][0], ff = f + KNIGHT_D[i][1];
            if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) n |= 1ULL << (rr * 8 + ff);
        }
        knight_attacks[sq] = n;
        pawn_attacks[WHITE][sq] = 0;
        pawn_attacks[BLACK][sq] = 0;
        if (r < 7) {
            if (f > 0) pawn_attacks[WHITE][sq] |= 1ULL << (sq + 7);
            if (f < 7) pawn_attacks[WHITE][sq] |= 1ULL << (sq + 9);
        }
        if (r > 0) {
            if (f > 0) pawn_attacks[BLACK][sq] |= 1ULL << (sq - 9);
            if (f < 7) pawn_attacks[BLACK][sq] |= 1ULL << (sq - 7);
        }
        empty_attacks[KNIGHT][sq] = n;
        empty_attacks[BISHOP][sq] = ray_attacks(sq, DIAG);
        empty_attacks[ROOK][sq] = ray_attacks(sq, ORTH);
        empty_attacks[QUEEN][sq] = empty_attacks[BISHOP][sq] | empty_attacks[ROOK][sq];
        for (int d = 0; d < 8; d++) {
            Bitboard bb = 0;
            int rr = r + RAY_DIRS[d][0], ff = f + RAY_DIRS[d][1];
            while (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) {
                bb |= 1ULL << (rr * 8 + ff);
                rr += RAY_DIRS[d][0];
                ff += RAY_DIRS[d][1];
            }
            rays[d][sq] = bb;
        }
    }

    int next = 2 * SLOTS[PAWN] * PAWN_SLOT_SIZE;
    for (int v = 0; v < 6; v++)
        for (int c = 0; c < 2; c++)
            slot_base[PAWN][v][c] = TARGET_SLOT[PAWN][v] < 0 ? -1
                                    : (TARGET_SLOT[PAWN][v] + c * SLOTS[PAWN]) * PAWN_SLOT_SIZE;
    for (int pt = KNIGHT; pt <= QUEEN; pt++) {
        int count = 0;
        for (int sq = 0; sq < 64; sq++) {
            sq_base[pt][sq] = (uint16_t)count;
            count += __builtin_popcountll(empty_attacks[pt][sq]);
        }
        for (int v = 0; v < 6; v++)
            for (int c = 0; c < 2; c++)
                slot_base[pt][v][c] = TARGET_SLOT[pt][v] < 0 ? -1
                                      : next + (TARGET_SLOT[pt][v] + c * SLOTS[pt]) * count;
        next += 2 * SLOTS[pt] * count;
    }
    if (next != NNUE_TP_THREATS_PER_SIDE) {
        fprintf(stderr, "info string NNUE threat tables inconsistent (%d != %d)\n", next, NNUE_TP_THREATS_PER_SIDE);
        exit(1);
    }
    tp_ready = true;
}

int nnue_tp_raw_threats(const Board* board, uint32_t* out) {
    Bitboard occ = 0;
    for (int c = 0; c < 2; c++)
        for (int t = 0; t < 6; t++) occ |= board->byTypeBB[c][t];

    int n = 0;
    for (int c = 0; c < 2; c++) {
        for (int pt = PAWN; pt <= QUEEN; pt++) {
            Bitboard victims = 0;
            for (int i = 0; i < 5 && VICTIM_TYPES[pt][i] >= 0; i++)
                victims |= board->byTypeBB[WHITE][VICTIM_TYPES[pt][i]] | board->byTypeBB[BLACK][VICTIM_TYPES[pt][i]];
            if (!victims) continue;

            const uint32_t attacker = (uint32_t)(c * 6 + pt) << 20;
            Bitboard attackers = board->byTypeBB[c][pt];
            while (attackers) {
                const int from = __builtin_ctzll(attackers);
                attackers &= attackers - 1;
                Bitboard att;
                switch (pt) {
                    case PAWN:   att = pawn_attacks[c][from]; break;
                    case KNIGHT: att = knight_attacks[from]; break;
                    case BISHOP: att = getBishopAttacks(from, occ); break;
                    case ROOK:   att = getRookAttacks(from, occ); break;
                    default:     att = getQueenAttacks(from, occ); break;
                }
                att &= victims;
                const uint32_t head = attacker | (uint32_t)from << 14;
                while (att) {
                    const int to = __builtin_ctzll(att);
                    att &= att - 1;
                    out[n++] = head | (uint32_t)to << 8 | board->piece[to];
                }
            }
        }
    }
    return n;
}

int nnue_tp_threat_indices(const uint32_t* raw, int n, int perspective, int mask, uint16_t* out) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t r = raw[i];
        const int attacker = (int)(r >> 20);
        const int c = attacker >= 6;
        const int pt = attacker - 6 * c;
        const int f = (int)((r >> 14) & 63) ^ mask;
        const int t = (int)((r >> 8) & 63) ^ mask;
        const int piece = (int)(r & 255);
        const int vt = PIECE_TYPE_OF(piece);
        const int vc = PIECE_COLOR_OF(piece) ^ perspective;
        const int32_t slot = slot_base[pt][vt][vc];
        if (slot < 0) continue;  // victim type is no target of this attacker
        const int base = NNUE_TP_PAIRS + (c ^ perspective) * NNUE_TP_THREATS_PER_SIDE + slot;
        if (pt == PAWN) {
            out[count++] = (uint16_t)(base + ((f >> 3) - 1) * 14 + 2 * (f & 7) + ((t & 7) > (f & 7)) - 1);
        } else if (vt != pt || t < f) {
            // between two pieces of the same type only the attack towards the lower square counts
            out[count++] = (uint16_t)(base + sq_base[pt][f] + __builtin_popcountll(empty_attacks[pt][f] & ((1ULL << t) - 1)));
        }
    }
    return count;
}

void nnue_tp_threats(const Board* board, const int mask[2], uint16_t* out[2], int count[2]) {
    uint32_t raw[NNUE_TP_MAX_THREATS];
    const int n = nnue_tp_raw_threats(board, raw);
    for (int p = 0; p < 2; p++) count[p] = nnue_tp_threat_indices(raw, n, p, mask[p], out[p]);
}

int nnue_tp_raw_pairs(const Board* board, uint16_t* out) {
    const Bitboard black_pawns = board->byTypeBB[BLACK][PAWN];
    const Bitboard pawns = board->byTypeBB[WHITE][PAWN] | black_pawns;
    const Bitboard FILE_A = 0x0101010101010101ULL;
    int n = 0;

    Bitboard outer = pawns;
    while (outer) {
        const int a = __builtin_ctzll(outer);
        outer &= outer - 1;
        const int file = a & 7;
        Bitboard band = FILE_A << file;
        if (file > 0) band |= FILE_A << (file - 1);
        if (file < 7) band |= FILE_A << (file + 1);
        // partners on higher squares, so every unordered pair is listed once
        Bitboard partners = outer & band;
        const uint16_t head = (uint16_t)(a << 8 | (int)((black_pawns >> a) & 1) << 1);
        while (partners) {
            const int b = __builtin_ctzll(partners);
            partners &= partners - 1;
            out[n++] = (uint16_t)(head | b << 2 | (int)((black_pawns >> b) & 1));
        }
    }
    return n;
}

int nnue_tp_pair_indices(const uint16_t* raw, int n, int perspective, int mask, uint16_t* out) {
    for (int i = 0; i < n; i++) {
        const int r = raw[i];
        // pawn id = perspective colour * 48 + square - 8
        const int ia = (((r >> 1) & 1) ^ perspective) * 48 + ((r >> 8) ^ mask) - 8;
        const int ib = ((r & 1) ^ perspective) * 48 + (((r >> 2) & 63) ^ mask) - 8;
        const int lo = ia < ib ? ia : ib, hi = ia < ib ? ib : ia;
        out[i] = (uint16_t)(hi * (hi - 1) / 2 + lo);
    }
    return n;
}

void nnue_tp_pairs(const Board* board, const int mask[2], uint16_t* out[2], int count[2]) {
    uint16_t raw[NNUE_TP_MAX_PAIRS];
    const int n = nnue_tp_raw_pairs(board, raw);
    for (int p = 0; p < 2; p++) count[p] = nnue_tp_pair_indices(raw, n, p, mask[p], out[p]);
}

// =============================================================================================
// Incremental threat updates, after pawnocchio (src/nnue/threats/updates.zig, scalar backend):
// every piece change emits the threats it creates or destroys as raw tuples
//   - outgoing: the piece's own attacks on occupied squares
//   - incoming: pieces attacking its square
//   - discovered: a slider whose line through the square is opened (removal) or blocked (addition)
// The emitters work on a described board state: occupancy `occ` plus an optional square whose
// piece differs from the board array (`ovr_sq` / `ovr_piece`), so the whole move can be replayed
// on the board after the move in the same order pawnocchio's makeMove calls them.
// =============================================================================================

typedef struct {
    const Board* board;
    int ovr_sq;
    int ovr_piece;
    NNUETPDelta* out;
} TPEmitter;

static inline int emit_lookup(const TPEmitter* e, int sq) {
    return sq == e->ovr_sq ? e->ovr_piece : e->board->piece[sq];
}

// attacker / victim as enum Piece
static inline void emit_tuple(TPEmitter* e, bool is_add, int attacker, int from, int victim, int to) {
    NNUETPDelta* d = e->out;
    const uint32_t r = (uint32_t)(PIECE_TYPE_OF(attacker) + 6 * PIECE_COLOR_OF(attacker)) << 20 |
                       (uint32_t)from << 14 | (uint32_t)to << 8 | (uint32_t)victim;
    if (is_add) {
        if (d->n_add < NNUE_TP_MAX_DELTA) d->add[d->n_add++] = r;
        else d->overflow = true;
    } else {
        if (d->n_sub < NNUE_TP_MAX_DELTA) d->sub[d->n_sub++] = r;
        else d->overflow = true;
    }
}

// nearest occupied square from sq in direction dir, -1 if none
static inline int nearest(int sq, int dir, Bitboard occ) {
    const Bitboard hit = rays[dir][sq] & occ;
    if (!hit) return -1;
    // N, NE, E, NW go to higher squares
    return (dir <= 2 || dir == 7) ? __builtin_ctzll(hit) : 63 - __builtin_clzll(hit);
}

static inline Bitboard piece_attacks(int pt, int c, int sq, Bitboard occ) {
    switch (pt) {
        case PAWN:   return pawn_attacks[c][sq];
        case KNIGHT: return knight_attacks[sq];
        case BISHOP: return getBishopAttacks(sq, occ);
        case ROOK:   return getRookAttacks(sq, occ);
        case QUEEN:  return getQueenAttacks(sq, occ);
        default:     return 0;
    }
}

// outgoing threats of `piece` on sq
static void emit_outgoing(TPEmitter* e, int piece, int sq, bool is_add, Bitboard occ) {
    Bitboard att = piece_attacks(PIECE_TYPE_OF(piece), PIECE_COLOR_OF(piece), sq, occ) & occ;
    while (att) {
        const int t = __builtin_ctzll(att);
        att &= att - 1;
        const int victim = emit_lookup(e, t);
        if (PIECE_TYPE_OF(victim) != KING) emit_tuple(e, is_add, piece, sq, victim, t);
    }
}

// threats of other pieces on `piece` standing on sq
static void emit_incoming(TPEmitter* e, int piece, int sq, bool is_add, Bitboard occ) {
    Bitboard cand = getRookAttacks(sq, occ) & occ;
    while (cand) {
        const int t = __builtin_ctzll(cand);
        cand &= cand - 1;
        const int a = emit_lookup(e, t);
        const int at = PIECE_TYPE_OF(a);
        if (at == ROOK || at == QUEEN) emit_tuple(e, is_add, a, t, piece, sq);
    }
    cand = getBishopAttacks(sq, occ) & occ;
    while (cand) {
        const int t = __builtin_ctzll(cand);
        cand &= cand - 1;
        const int a = emit_lookup(e, t);
        const int at = PIECE_TYPE_OF(a);
        if (at == BISHOP || at == QUEEN) emit_tuple(e, is_add, a, t, piece, sq);
    }
    cand = knight_attacks[sq] & occ;
    while (cand) {
        const int t = __builtin_ctzll(cand);
        cand &= cand - 1;
        const int a = emit_lookup(e, t);
        if (PIECE_TYPE_OF(a) == KNIGHT) emit_tuple(e, is_add, a, t, piece, sq);
    }
    // a white pawn attacks sq from the squares a black pawn on sq would attack, and vice versa
    cand = pawn_attacks[BLACK][sq] & occ;
    while (cand) {
        const int t = __builtin_ctzll(cand);
        cand &= cand - 1;
        if (emit_lookup(e, t) == W_PAWN) emit_tuple(e, is_add, W_PAWN, t, piece, sq);
    }
    cand = pawn_attacks[WHITE][sq] & occ;
    while (cand) {
        const int t = __builtin_ctzll(cand);
        cand &= cand - 1;
        if (emit_lookup(e, t) == B_PAWN) emit_tuple(e, is_add, B_PAWN, t, piece, sq);
    }
}

static inline bool slides_along(int pt, int dir) {
    return pt == QUEEN || (pt == ROOK && (dir & 1) == 0) || (pt == BISHOP && (dir & 1) == 1);
}

// slider threats through sq that a piece on sq blocks (is_add) or releases (!is_add)
static void emit_discovered(TPEmitter* e, int sq, bool is_add, Bitboard occ) {
    for (int d = 0; d < 4; d++) {
        const int s1 = nearest(sq, d, occ);
        if (s1 < 0) continue;
        const int s2 = nearest(sq, d + 4, occ);
        if (s2 < 0) continue;
        const int p1 = emit_lookup(e, s1), p2 = emit_lookup(e, s2);
        const int t1 = PIECE_TYPE_OF(p1), t2 = PIECE_TYPE_OF(p2);
        if (slides_along(t1, d) && t2 != KING) emit_tuple(e, !is_add, p1, s1, p2, s2);
        if (slides_along(t2, d) && t1 != KING) emit_tuple(e, !is_add, p2, s2, p1, s1);
    }
}

// pawnocchio emitChangeSide: `piece` appears on / disappears from sq
static void emit_change(TPEmitter* e, int piece, int sq, bool is_add, Bitboard occ) {
    if (PIECE_TYPE_OF(piece) != KING) {
        emit_outgoing(e, piece, sq, is_add, occ);
        emit_incoming(e, piece, sq, is_add, occ);
    }
    emit_discovered(e, sq, is_add, occ);
}

// pawnocchio onMutate: the piece on sq changes, occupancy stays
static void emit_mutate(TPEmitter* e, int old_piece, int new_piece, int sq, Bitboard occ) {
    if (PIECE_TYPE_OF(old_piece) != KING) {
        emit_outgoing(e, old_piece, sq, false, occ);
    }
    if (PIECE_TYPE_OF(new_piece) != KING) {
        emit_outgoing(e, new_piece, sq, true, occ);
    }
    if (PIECE_TYPE_OF(old_piece) != KING) emit_incoming(e, old_piece, sq, false, occ);
    if (PIECE_TYPE_OF(new_piece) != KING) emit_incoming(e, new_piece, sq, true, occ);
}

static inline uint16_t raw_pair(int a, int ca, int b, int cb) {
    // same packing as nnue_tp_raw_pairs (index computation does not need a < b)
    return (uint16_t)(a << 8 | b << 2 | ca << 1 | cb);
}

static inline Bitboard pair_band(int sq) {
    const Bitboard FILE_A = 0x0101010101010101ULL;
    const int file = sq & 7;
    Bitboard band = FILE_A << file;
    if (file > 0) band |= FILE_A << (file - 1);
    if (file < 7) band |= FILE_A << (file + 1);
    return band;
}

static void pairs_with(NNUETPDelta* d, bool is_add, int sq, int colour, Bitboard others, Bitboard black_pawns) {
    Bitboard bb = others & pair_band(sq);
    while (bb) {
        const int o = __builtin_ctzll(bb);
        bb &= bb - 1;
        const uint16_t r = raw_pair(sq, colour, o, (int)((black_pawns >> o) & 1));
        if (is_add) d->pair_add[d->n_pair_add++] = r;
        else d->pair_sub[d->n_pair_sub++] = r;
    }
}

bool nnue_tp_record_move(const Board* board, int from, int to, int moving_piece, int captured_piece,
                         int capture_sq, bool castling, NNUETPDelta* d) {
    d->n_add = d->n_sub = d->n_pair_add = d->n_pair_sub = 0;
    d->overflow = false;

    Bitboard occ = 0;
    for (int c = 0; c < 2; c++)
        for (int t = 0; t < 6; t++) occ |= board->byTypeBB[c][t];

    TPEmitter e = { board, -1, NO_PIECE, d };
    const int us = PIECE_COLOR_OF(moving_piece);
    const Bitboard to_bb = 1ULL << to;

    if (castling) {
        // board order of pawnocchio: king off, rook off, king on, rook on
        const int king_to = to;
        const int rook_from = (to & 7) == 6 ? to + 1 : to - 2;
        const int rook_to = (to & 7) == 6 ? to - 1 : to + 1;
        const int rook = us == WHITE ? W_ROOK : B_ROOK;
        const Bitboard s2 = occ & ~(1ULL << king_to) & ~(1ULL << rook_to);
        e.ovr_sq = rook_from;
        e.ovr_piece = rook;
        emit_change(&e, moving_piece, from, false, s2 | (1ULL << rook_from));
        e.ovr_sq = -1;
        emit_change(&e, rook, rook_from, false, s2);
        emit_change(&e, moving_piece, king_to, true, occ & ~(1ULL << rook_to));
        emit_change(&e, rook, rook_to, true, occ);
    } else if (captured_piece != NO_PIECE && capture_sq != to) {
        // en passant: the pawn moves (captured pawn still there), then the captured pawn goes
        const Bitboard with_cap = occ | (1ULL << capture_sq);
        e.ovr_sq = capture_sq;
        e.ovr_piece = captured_piece;
        emit_change(&e, moving_piece, from, false, with_cap & ~to_bb);
        emit_change(&e, moving_piece, to, true, with_cap);
        e.ovr_sq = -1;
        emit_change(&e, captured_piece, capture_sq, false, occ);
    } else if (captured_piece != NO_PIECE) {
        // capture: the mover leaves (victim still on to), then the piece on to mutates
        e.ovr_sq = to;
        e.ovr_piece = captured_piece;
        emit_change(&e, moving_piece, from, false, occ);
        e.ovr_sq = -1;
        emit_mutate(&e, captured_piece, board->piece[to], to, occ);
    } else {
        // quiet move / promotion: leave from (to still empty), arrive on to
        emit_change(&e, moving_piece, from, false, occ & ~to_bb);
        emit_change(&e, board->piece[to], to, true, occ);
    }

    // pawn pairs (pawnocchio dirtyPawns / computePairs): pairs of removed pawns with the pawns that
    // stay, pairs of the added pawn with them, and the pair of two removed pawns
    const Bitboard black_pawns = board->byTypeBB[BLACK][PAWN];
    const Bitboard pawns = board->byTypeBB[WHITE][PAWN] | black_pawns;
    const bool from_pawn = PIECE_TYPE_OF(moving_piece) == PAWN;
    const bool cap_pawn = captured_piece != NO_PIECE && PIECE_TYPE_OF(captured_piece) == PAWN;
    const bool added_pawn = from_pawn && PIECE_TYPE_OF(board->piece[to]) == PAWN;
    const Bitboard unchanged = pawns & ~(added_pawn ? to_bb : 0);
    if (from_pawn) pairs_with(d, false, from, us, unchanged, black_pawns);
    if (cap_pawn) pairs_with(d, false, capture_sq, us ^ 1, unchanged, black_pawns);
    if (from_pawn && cap_pawn && (pair_band(from) >> capture_sq & 1))
        d->pair_sub[d->n_pair_sub++] = raw_pair(from, us, capture_sq, us ^ 1);
    if (added_pawn) pairs_with(d, true, to, us, unchanged, black_pawns);

    return !d->overflow;
}
