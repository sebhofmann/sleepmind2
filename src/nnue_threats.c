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
        const int base = NNUE_TP_PAIRS + (c ^ perspective) * NNUE_TP_THREATS_PER_SIDE + slot_base[pt][vt][vc];
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
