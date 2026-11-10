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
// Lookup tables of nnue_tp_threat_indices:
//   pair_base[perspective][attacker colour * 6 + type][victim as enum Piece]: feature index without
//     the square part, -1 if the pair is no feature
//   pair_same[attacker][victim]: both of the same type (only attacks towards the lower square count)
//   from_to[attacker type][from][to]: square part, squares in perspective coordinates
static int32_t pair_base[2][12][13];
static uint8_t pair_same[12][13];
static uint16_t from_to[5][64][64];
// attacker type / victim type pairs that are a feature (in every perspective)
static bool tp_valid[6][6];
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
    for (int pt = PAWN; pt <= KING; pt++)
        for (int vt = PAWN; vt <= KING; vt++) tp_valid[pt][vt] = pt != KING && TARGET_SLOT[pt][vt] >= 0;
    for (int p = 0; p < 2; p++) {
        for (int a = 0; a < 12; a++) {
            const int c = a >= 6, pt = a - 6 * c;
            for (int v = 0; v <= B_KING; v++) {
                pair_base[p][a][v] = -1;
                pair_same[a][v] = 0;
                if (pt == KING || v == NO_PIECE) continue;
                const int vt = PIECE_TYPE_OF(v), vc = PIECE_COLOR_OF(v) ^ p;
                if (slot_base[pt][vt][vc] < 0) continue;
                pair_base[p][a][v] = NNUE_TP_PAIRS + (c ^ p) * NNUE_TP_THREATS_PER_SIDE + slot_base[pt][vt][vc];
                pair_same[a][v] = vt == pt;
            }
        }
    }
    for (int f = 0; f < 64; f++) {
        for (int t = 0; t < 64; t++) {
            // pawns only stand on ranks 2-7
            const int pawn = ((f >> 3) - 1) * 14 + 2 * (f & 7) + ((t & 7) > (f & 7)) - 1;
            from_to[PAWN][f][t] = (uint16_t)(pawn < 0 ? 0 : pawn);
            for (int pt = KNIGHT; pt <= QUEEN; pt++)
                from_to[pt][f][t] = (uint16_t)(sq_base[pt][f] +
                                               __builtin_popcountll(empty_attacks[pt][f] & ((1ULL << t) - 1)));
        }
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
    // Branchless: every tuple writes its index, the count only advances for real features. `out`
    // needs room for n entries.
    const int32_t (*base)[13] = pair_base[perspective];
    int count = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t r = raw[i];
        const int attacker = (int)(r >> 20);
        const int pt = attacker - 6 * (attacker >= 6);
        const int f = (int)((r >> 14) & 63) ^ mask;
        const int t = (int)((r >> 8) & 63) ^ mask;
        const int victim = (int)(r & 255);
        const int32_t b = base[attacker][victim];
        out[count] = (uint16_t)(b + from_to[pt][f][t]);
        // between two pieces of the same type only the attack towards the lower square counts
        count += (b >= 0) & (!pair_same[attacker][victim] | (t < f));
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
    // attackers by movement in the described state (both colours): rooks + queens, bishops + queens,
    // knights, and the pawns per colour. Bits outside the described occupancy are stale and must be
    // masked with it.
    Bitboard orth, diag, knights, pawns[2];
    Bitboard base_orth, base_diag, base_knights, base_pawns[2];
} TPEmitter;

static inline int emit_lookup(const TPEmitter* e, int sq) {
    return sq == e->ovr_sq ? e->ovr_piece : e->board->piece[sq];
}

static void emitter_init(TPEmitter* e, const Board* board, NNUETPDelta* out) {
    e->board = board;
    e->out = out;
    e->ovr_sq = -1;
    e->ovr_piece = NO_PIECE;
    const Bitboard queens = board->byTypeBB[WHITE][QUEEN] | board->byTypeBB[BLACK][QUEEN];
    e->orth = e->base_orth = board->byTypeBB[WHITE][ROOK] | board->byTypeBB[BLACK][ROOK] | queens;
    e->diag = e->base_diag = board->byTypeBB[WHITE][BISHOP] | board->byTypeBB[BLACK][BISHOP] | queens;
    e->knights = e->base_knights = board->byTypeBB[WHITE][KNIGHT] | board->byTypeBB[BLACK][KNIGHT];
    e->pawns[WHITE] = e->base_pawns[WHITE] = board->byTypeBB[WHITE][PAWN];
    e->pawns[BLACK] = e->base_pawns[BLACK] = board->byTypeBB[BLACK][PAWN];
}

// The described state has `piece` on sq instead of what the board array holds there (sq < 0: none)
static void emitter_override(TPEmitter* e, int sq, int piece) {
    e->ovr_sq = sq;
    e->ovr_piece = piece;
    e->orth = e->base_orth;
    e->diag = e->base_diag;
    e->knights = e->base_knights;
    e->pawns[WHITE] = e->base_pawns[WHITE];
    e->pawns[BLACK] = e->base_pawns[BLACK];
    if (sq < 0) return;
    const Bitboard bit = 1ULL << sq;
    e->orth &= ~bit;
    e->diag &= ~bit;
    e->knights &= ~bit;
    e->pawns[WHITE] &= ~bit;
    e->pawns[BLACK] &= ~bit;
    switch (PIECE_TYPE_OF(piece)) {
        case PAWN:   e->pawns[PIECE_COLOR_OF(piece)] |= bit; break;
        case KNIGHT: e->knights |= bit; break;
        case BISHOP: e->diag |= bit; break;
        case ROOK:   e->orth |= bit; break;
        case QUEEN:  e->orth |= bit; e->diag |= bit; break;
        default: break;
    }
}

// attacker / victim as enum Piece; pairs that are no feature in any perspective are dropped here
static inline void emit_tuple(TPEmitter* e, bool is_add, int attacker, int from, int victim, int to) {
    if (!tp_valid[PIECE_TYPE_OF(attacker)][PIECE_TYPE_OF(victim)]) return;
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

// Attack sets from sq on occupied squares, shared by the emitters of one square: `rook` / `bishop`
// hold the nearest piece in each of the four orthogonal / diagonal directions.
typedef struct {
    Bitboard rook, bishop;
} TPLines;

static inline TPLines lines_from(int sq, Bitboard occ) {
    const TPLines l = { getRookAttacks(sq, occ) & occ, getBishopAttacks(sq, occ) & occ };
    return l;
}

// outgoing threats of `piece` on sq
static inline void emit_outgoing(TPEmitter* e, int piece, int sq, bool is_add, Bitboard occ, TPLines l) {
    Bitboard att;
    switch (PIECE_TYPE_OF(piece)) {
        case PAWN:   att = pawn_attacks[PIECE_COLOR_OF(piece)][sq] & occ; break;
        case KNIGHT: att = knight_attacks[sq] & occ; break;
        case BISHOP: att = l.bishop; break;
        case ROOK:   att = l.rook; break;
        case QUEEN:  att = l.rook | l.bishop; break;
        default:     att = 0; break;
    }
    while (att) {
        const int t = __builtin_ctzll(att);
        att &= att - 1;
        emit_tuple(e, is_add, piece, sq, emit_lookup(e, t), t);
    }
}

// Threats of other pieces on sq: removed for `old_piece`, added for `new_piece` (NO_PIECE = none).
// A white pawn attacks sq from the squares a black pawn on sq would attack, and vice versa.
static inline void emit_incoming(TPEmitter* e, int old_piece, int new_piece, int sq, Bitboard occ, TPLines l) {
    Bitboard cand = (l.rook & e->orth) | (l.bishop & e->diag) | (knight_attacks[sq] & e->knights & occ) |
                    (pawn_attacks[BLACK][sq] & e->pawns[WHITE] & occ) |
                    (pawn_attacks[WHITE][sq] & e->pawns[BLACK] & occ);
    while (cand) {
        const int t = __builtin_ctzll(cand);
        cand &= cand - 1;
        const int a = emit_lookup(e, t);
        if (old_piece != NO_PIECE) emit_tuple(e, false, a, t, old_piece, sq);
        if (new_piece != NO_PIECE) emit_tuple(e, true, a, t, new_piece, sq);
    }
}

// slider threats through sq that a piece on sq blocks (is_add) or releases (!is_add)
static inline void emit_discovered(TPEmitter* e, int sq, bool is_add, TPLines l) {
    const Bitboard orth = l.rook & e->orth, diag = l.bishop & e->diag;
    if (!(orth | diag)) return;
    for (int d = 0; d < 4; d++) {
        const Bitboard line = (d & 1) ? l.bishop : l.rook;
        const Bitboard a = line & rays[d][sq], b = line & rays[d + 4][sq];
        // needs a piece on both sides, at least one of them sliding along this line
        if (!a || !b || !((a | b) & ((d & 1) ? diag : orth))) continue;
        const int s1 = __builtin_ctzll(a), s2 = __builtin_ctzll(b);
        const int p1 = emit_lookup(e, s1), p2 = emit_lookup(e, s2);
        if (a & ((d & 1) ? diag : orth)) emit_tuple(e, !is_add, p1, s1, p2, s2);
        if (b & ((d & 1) ? diag : orth)) emit_tuple(e, !is_add, p2, s2, p1, s1);
    }
}

// pawnocchio emitChangeSide: `piece` appears on / disappears from sq
static void emit_change(TPEmitter* e, int piece, int sq, bool is_add, Bitboard occ) {
    const TPLines l = lines_from(sq, occ);
    if (PIECE_TYPE_OF(piece) != KING) {
        emit_outgoing(e, piece, sq, is_add, occ, l);
        emit_incoming(e, is_add ? NO_PIECE : piece, is_add ? piece : NO_PIECE, sq, occ, l);
    }
    emit_discovered(e, sq, is_add, l);
}

// pawnocchio onMutate: the piece on sq changes, occupancy stays
static void emit_mutate(TPEmitter* e, int old_piece, int new_piece, int sq, Bitboard occ) {
    const TPLines l = lines_from(sq, occ);
    const bool had = PIECE_TYPE_OF(old_piece) != KING, has = PIECE_TYPE_OF(new_piece) != KING;
    if (had) emit_outgoing(e, old_piece, sq, false, occ, l);
    if (has) emit_outgoing(e, new_piece, sq, true, occ, l);
    if (had || has) emit_incoming(e, had ? old_piece : NO_PIECE, has ? new_piece : NO_PIECE, sq, occ, l);
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

    TPEmitter e;
    emitter_init(&e, board, d);
    const int us = PIECE_COLOR_OF(moving_piece);
    const Bitboard to_bb = 1ULL << to;

    if (castling) {
        // board order of pawnocchio: king off, rook off, king on, rook on
        const int king_to = to;
        const int rook_from = (to & 7) == 6 ? to + 1 : to - 2;
        const int rook_to = (to & 7) == 6 ? to - 1 : to + 1;
        const int rook = us == WHITE ? W_ROOK : B_ROOK;
        const Bitboard s2 = occ & ~(1ULL << king_to) & ~(1ULL << rook_to);
        emitter_override(&e, rook_from, rook);
        emit_change(&e, moving_piece, from, false, s2 | (1ULL << rook_from));
        emitter_override(&e, -1, NO_PIECE);
        emit_change(&e, rook, rook_from, false, s2);
        emit_change(&e, moving_piece, king_to, true, occ & ~(1ULL << rook_to));
        emit_change(&e, rook, rook_to, true, occ);
    } else if (captured_piece != NO_PIECE && capture_sq != to) {
        // en passant: the pawn moves (captured pawn still there), then the captured pawn goes
        const Bitboard with_cap = occ | (1ULL << capture_sq);
        emitter_override(&e, capture_sq, captured_piece);
        emit_change(&e, moving_piece, from, false, with_cap & ~to_bb);
        emit_change(&e, moving_piece, to, true, with_cap);
        emitter_override(&e, -1, NO_PIECE);
        emit_change(&e, captured_piece, capture_sq, false, occ);
    } else if (captured_piece != NO_PIECE) {
        // capture: the mover leaves (victim still on to), then the piece on to mutates
        emitter_override(&e, to, captured_piece);
        emit_change(&e, moving_piece, from, false, occ);
        emitter_override(&e, -1, NO_PIECE);
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
