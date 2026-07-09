#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif
#include "search.h"
#include "evaluation.h"
#include "move_generator.h"
#include "board_modifiers.h"
#include "tt.h"
#include "board_io.h"
#include "board.h"
#include "move.h"
#include "bitboard_utils.h"
#include "zobrist.h"
#include "syzygy.h"
#include <stdio.h>

// UCI cp value reported for a proven TB win/loss without a known mate distance.
#define TB_DISPLAY_CP 20000

// Define this to enable Zobrist hash verification after undo
//#define DEBUG_ZOBRIST_VERIFY

// Define this to enable search statistics (TT + pruning) output after each search
// Can be enabled via: make STATS=1
//#define SEARCH_STATS

#ifdef SEARCH_STATS
static uint64_t tt_probes = 0;
static uint64_t tt_hits = 0;
static uint64_t tt_cutoffs = 0;
static uint64_t beta_cutoffs = 0;
static uint64_t beta_cutoffs_first = 0;

static struct {
    uint64_t null_move;
    uint64_t reverse_futility;
    uint64_t razoring;
    uint64_t futility;
    uint64_t lmp;
    uint64_t lmr;
    uint64_t delta;
    uint64_t see_pruning;
} pruning_stats;

#define TT_STAT_INC(var) ((var)++)
#define TT_STATS_RESET() do { tt_probes = 0; tt_hits = 0; tt_cutoffs = 0; \
                              beta_cutoffs = 0; beta_cutoffs_first = 0; } while(0)
#define PRUNING_STAT_INC(field) (pruning_stats.field++)
#define PRUNING_STATS_RESET() memset(&pruning_stats, 0, sizeof(pruning_stats))
#else
#define TT_STAT_INC(var) ((void)0)
#define TT_STATS_RESET() ((void)0)
#define PRUNING_STAT_INC(field) ((void)0)
#define PRUNING_STATS_RESET() ((void)0)
#endif

#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <math.h>

// =============================================================================
// Silent Mode (for training - disables info/debug output)
// =============================================================================
bool search_silent_mode = false;



// =============================================================================
// LMR Reduction Table (precomputed for speed)
// =============================================================================
static int lmr_table[MAX_PLY][64];
static bool lmr_table_initialized = false;

static void init_lmr_table(void) {
    if (lmr_table_initialized) return;
    for (int depth = 0; depth < MAX_PLY; depth++) {
        for (int moves = 0; moves < 64; moves++) {
            if (depth == 0 || moves == 0) {
                lmr_table[depth][moves] = 0;
            } else {
                // Stockfish-style formula: ln(depth) * ln(moves) / 2
                lmr_table[depth][moves] = (int)(log((double)depth) * log((double)moves) / 2.0);
            }
        }
    }
    lmr_table_initialized = true;
}

void set_search_silent(bool silent) {
    search_silent_mode = silent;
}

long search_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

// =============================================================================
// Initialize SearchParams with default values
// =============================================================================

void search_params_init(SearchParams* params) {
    // Initialize LMR table on first call
    init_lmr_table();

    // Feature enable flags (tuned via tournament testing)
    params->use_lmr = true;
    params->use_null_move = true;
    params->use_futility = true;
    params->use_rfp = true;
    params->use_delta_pruning = false;  // Disabled - tested to be better without
    params->use_aspiration = true;
    params->use_check_extension = true; // Extend by 1 ply when in check
    params->use_qs_see_pruning = true;  // SPRT-confirmed +30 Elo (skip SEE<0 captures in qsearch)
    params->use_bad_capture_last = true; // SPRT-confirmed +11 Elo (losing captures ordered after quiets)
    params->use_lmp = true;

    // Late Move Pruning: skip quiets after base + depth^2 searched moves
    params->lmp_base = 6;
    params->lmp_max_depth = 8;

    // Late Move Reduction parameters (tuned via tournament testing)
    params->lmr_full_depth_moves = 3;   // More aggressive LMR
    params->lmr_reduction_limit = 1;    // Start LMR earlier

    // Null Move Pruning: reduction is adaptive in negamax (3 + depth/3 +
    // eval margin term; LTC-SPRT +15.7 Elo vs static R=4 with verification).
    params->null_move_min_depth = 4;

    // Futility pruning margins (SPSA-tuned)
    params->futility_margin = 243;       // Depth 1
    params->futility_margin_d2 = 287;   // Depth 2
    params->futility_margin_d3 = 440;   // Depth 3

    // Reverse Futility Pruning (SPSA-tuned)
    params->rfp_margin = 93;
    params->rfp_max_depth = 9;

    // Razoring (drop into qsearch if position looks hopeless)
    params->use_razoring = true;
    params->razor_margin = 299;         // Base margin (scaled by depth)

    // Delta pruning margin for quiescence
    params->delta_margin = 200;         // Tighter with reliable eval

    // Aspiration window (SPSA-tuned)
    params->aspiration_window = 114;

    // History update scale (SPSA-tuned: bonus steeper, malus flatter than
    // the Stockfish magnitudes these started from)
    params->hist_bonus_mult = 441;
    params->hist_bonus_sub = 260;
    params->hist_bonus_max = 5361;
    params->hist_malus_mult = 966;
    params->hist_malus_sub = 401;
    params->hist_malus_max = 1433;
    params->fmh_weight = 130;           // 166/96: 2-ply history weighted above 1-ply

    // LMR thresholds on the combined quiet ordering score - calibrated to
    // the history magnitudes above, tune them together
    params->lmr_stat_low2 = -32216;
    params->lmr_stat_low1 = -2893;
    params->lmr_stat_high1 = 23973;
    params->lmr_stat_high2 = 14621;
}

// =============================================================================
// Piece values for move ordering
// =============================================================================

static int get_piece_value(PieceTypeToken piece_type) {
    piece_type &= 0x7;
    switch (piece_type) {
        case PAWN_T:   return 100;
        case KNIGHT_T: return 320;
        case BISHOP_T: return 330;
        case ROOK_T:   return 500;
        case QUEEN_T:  return 900;
        case KING_T:   return 20000;
        default:       return 0;
    }
}

static int get_promotion_value(int promo) {
    switch (promo) {
        case PROMOTION_N: return 320;
        case PROMOTION_B: return 330;
        case PROMOTION_R: return 500;
        case PROMOTION_Q: return 900;
        default:          return 0;
    }
}

// SEE piece values (simple array for fast lookup)
static const int SEE_VALUES[7] = {
    0,      // NONE
    100,    // PAWN
    320,    // KNIGHT
    330,    // BISHOP
    500,    // ROOK
    900,    // QUEEN
    20000   // KING
};

// External attack tables from move_generator.c
extern Bitboard getRookAttacks(int square, Bitboard occupancy);
extern Bitboard getBishopAttacks(int square, Bitboard occupancy);

// Precomputed attack tables (declared in move_generator.c, we need access)
// We'll compute attackers dynamically

// Get all attackers to a square
static Bitboard get_all_attackers(const Board* board, int square, Bitboard occupied) {
    Bitboard attackers = 0ULL;
    
    // Pawn attacks (reverse lookup - who can attack this square with a pawn?)
    // White pawns attack diagonally upward, so square must be attacked from below
    Bitboard white_pawn_attackers = 0ULL;
    if (square >= 9 && (square % 8) > 0) white_pawn_attackers |= (1ULL << (square - 9));
    if (square >= 7 && (square % 8) < 7) white_pawn_attackers |= (1ULL << (square - 7));
    attackers |= white_pawn_attackers & board->byTypeBB[WHITE][PAWN];
    
    // Black pawns attack diagonally downward
    Bitboard black_pawn_attackers = 0ULL;
    if (square <= 54 && (square % 8) < 7) black_pawn_attackers |= (1ULL << (square + 9));
    if (square <= 56 && (square % 8) > 0) black_pawn_attackers |= (1ULL << (square + 7));
    attackers |= black_pawn_attackers & board->byTypeBB[BLACK][PAWN];
    
    // Knight attacks (symmetric)
    static const int knight_offsets[8] = {-17, -15, -10, -6, 6, 10, 15, 17};
    Bitboard knights = board->byTypeBB[WHITE][KNIGHT] | board->byTypeBB[BLACK][KNIGHT];
    for (int i = 0; i < 8; i++) {
        int from = square + knight_offsets[i];
        if (from >= 0 && from < 64) {
            int from_file = from % 8;
            int to_file = square % 8;
            if (abs(from_file - to_file) <= 2) {
                attackers |= (1ULL << from) & knights;
            }
        }
    }
    
    // King attacks (symmetric)
    static const int king_offsets[8] = {-9, -8, -7, -1, 1, 7, 8, 9};
    Bitboard kings = board->byTypeBB[WHITE][KING] | board->byTypeBB[BLACK][KING];
    for (int i = 0; i < 8; i++) {
        int from = square + king_offsets[i];
        if (from >= 0 && from < 64) {
            int from_file = from % 8;
            int to_file = square % 8;
            if (abs(from_file - to_file) <= 1) {
                attackers |= (1ULL << from) & kings;
            }
        }
    }
    
    // Sliding pieces - use indexed bitboards
    Bitboard rooks_queens = board->byTypeBB[WHITE][ROOK] | board->byTypeBB[BLACK][ROOK] |
                            board->byTypeBB[WHITE][QUEEN] | board->byTypeBB[BLACK][QUEEN];
    Bitboard bishops_queens = board->byTypeBB[WHITE][BISHOP] | board->byTypeBB[BLACK][BISHOP] |
                              board->byTypeBB[WHITE][QUEEN] | board->byTypeBB[BLACK][QUEEN];
    
    Bitboard rook_attacks = getRookAttacks(square, occupied);
    attackers |= rook_attacks & rooks_queens;
    
    Bitboard bishop_attacks = getBishopAttacks(square, occupied);
    attackers |= bishop_attacks & bishops_queens;
    
    return attackers;
}

// Get the least valuable attacker from a set of attackers
static int get_smallest_attacker(const Board* board, Bitboard attackers, bool white, int* piece_square) {
    Bitboard our_pieces;
    int c = white ? WHITE : BLACK;
    
    // Check in order: Pawns, Knights, Bishops, Rooks, Queens, King
    our_pieces = attackers & board->byTypeBB[c][PAWN];
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[1]; // PAWN
    }
    
    our_pieces = attackers & board->byTypeBB[c][KNIGHT];
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[2]; // KNIGHT
    }
    
    our_pieces = attackers & board->byTypeBB[c][BISHOP];
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[3]; // BISHOP
    }
    
    our_pieces = attackers & board->byTypeBB[c][ROOK];
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[4]; // ROOK
    }
    
    our_pieces = attackers & board->byTypeBB[c][QUEEN];
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[5]; // QUEEN
    }
    
    our_pieces = attackers & board->byTypeBB[c][KING];
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[6]; // KING
    }
    
    return 0; // No attacker
}

// Static Exchange Evaluation
// Returns the expected material gain/loss from a capture sequence
static int see(const Board* board, Move move) {
    int from = MOVE_FROM(move);
    int to = MOVE_TO(move);
    
    // Get initial attacker value
    bool attacker_white = board->whiteToMove;
    bool attacker_side = attacker_white;
    PieceTypeToken attacker_type = getPieceTypeAtSquare(board, from, &attacker_white);
    int attacker_value = get_piece_value(attacker_type);

    // For promotions, the piece on the target square after the move is the
    // promoted piece, not the pawn - and the move itself already gains
    // (promo - pawn) in material, even with an empty target square
    int piece_on_target_value = attacker_value;
    int promo_gain = 0;
    if (MOVE_IS_PROMOTION(move)) {
        piece_on_target_value = get_promotion_value(MOVE_PROMOTION(move));
        promo_gain = piece_on_target_value - SEE_VALUES[1];
    }
    
    // Get victim value
    bool victim_white = !board->whiteToMove;
    PieceTypeToken victim_type = getPieceTypeAtSquare(board, to, &victim_white);
    int victim_value = get_piece_value(victim_type);
    
    // Handle en passant
    if (MOVE_IS_EN_PASSANT(move)) {
        victim_value = SEE_VALUES[1]; // Pawn
    }
    
    // If capturing nothing (and not promoting), there is no exchange to evaluate
    if (victim_value == 0 && !MOVE_IS_EN_PASSANT(move) && !MOVE_IS_PROMOTION(move)) {
        return 0;
    }

    // Gain array to track material balance at each step
    int gain[32];
    int depth = 0;

    // Initial capture (plus material gained by promoting)
    gain[depth] = victim_value + promo_gain;
    
    // Simulate the capture
    Bitboard occupied = board->whitePawns | board->whiteKnights | board->whiteBishops |
                        board->whiteRooks | board->whiteQueens | board->whiteKings |
                        board->blackPawns | board->blackKnights | board->blackBishops |
                        board->blackRooks | board->blackQueens | board->blackKings;
    
    // Remove the initial attacker from occupancy
    occupied &= ~(1ULL << from);
    
    // Get all attackers to the target square
    Bitboard attackers = get_all_attackers(board, to, occupied);
    
    // Remove the initial attacker
    attackers &= ~(1ULL << from);
    
    // Current piece on the target square (the one that just captured)
    // For promotions, the piece on the target is the promoted piece, not the pawn
    int current_piece_value = piece_on_target_value;
    
    // Alternate sides
    bool side_to_move = !attacker_side;
    
    while (attackers) {
        // Find smallest attacker for the side to move. If that side has no
        // attacker left (even though opposite-colour attackers may linger in the
        // set), the exchange is over. Must check BEFORE incrementing depth, else
        // gain[depth] is left uninitialised and the negamax below reads garbage.
        int piece_square;
        int next_attacker_value = get_smallest_attacker(board, attackers, side_to_move, &piece_square);
        if (next_attacker_value == 0) break;

        depth++;
        if (depth >= 32) break;

        // Calculate gain: capture the current piece, but we might lose our attacker
        gain[depth] = current_piece_value - gain[depth - 1];

        // No early-exit pruning: a sound cutoff must not change the result. The
        // previous condition did (it stopped before a profitable recapture, e.g.
        // Arasan SEE case "Bxc6" gave -130 instead of -230). Full swap instead.

        // Update occupied (remove the attacker)
        occupied &= ~(1ULL << piece_square);
        attackers &= ~(1ULL << piece_square);
        
        // Update attackers (x-ray through the removed piece)
        attackers |= get_all_attackers(board, to, occupied) & occupied;
        
        // Update current piece value
        current_piece_value = next_attacker_value;
        
        // Switch sides
        side_to_move = !side_to_move;
    }
    
    // Negamax the gain array
    while (depth > 0) {
        gain[depth - 1] = -(-gain[depth - 1] > gain[depth] ? -gain[depth - 1] : gain[depth]);
        depth--;
    }
    
    return gain[0];
}

// Quick SEE check: is the capture likely good (>= threshold)?
__attribute__((unused))
static bool see_ge(const Board* board, Move move, int threshold) {
    return see(board, move) >= threshold;
}

// Debug wrapper to expose SEE for testing
int see_debug(const Board* board, Move move) {
    return see(board, move);
}

// =============================================================================
// Continuation history (Stockfish-style, replaces killers + countermoves)
// Indexed by [prev_piece][prev_to][piece][to]; piece index = board->piece - 1
// (0-5 white, 6-11 black). File-scope static because the tables are too large
// for the stack-allocated SearchInfo used in training. Cleared on new game,
// persists (un-decayed) across searches within a game, like Stockfish.
// =============================================================================
#define CMH_MAX 16384
static int16_t cmh_table[12][64][12][64]; // 1 ply back (countermove history)
static int16_t fmh_table[12][64][12][64]; // 2 plies back (follow-up history)

// Piece index for continuation tables, -1 if square is empty
static inline int cmh_piece_index(const Board* board, int sq) {
    int p = board->piece[sq];
    return p - 1; // NO_PIECE(0) -> -1
}

// Continuation history score for move m, `delta` plies back (0 if unavailable)
static inline int cont_score(int16_t (*tbl)[64][12][64], const Board* board,
                             SearchInfo* info, int ply, int delta, Move m) {
    if (ply < delta) return 0;
    Move prev = info->prev_moves[ply - delta];
    if (prev == 0) return 0;
    int pp = info->prev_pieces[ply - delta];
    if (pp < 0) return 0;
    int cp = cmh_piece_index(board, MOVE_FROM(m));
    if (cp < 0) return 0;
    return tbl[pp][MOVE_TO(prev)][cp][MOVE_TO(m)];
}

// Gravity update of a continuation history entry (bonus may be negative)
static void update_cont(int16_t (*tbl)[64][12][64], const Board* board,
                        SearchInfo* info, int ply, int delta, Move m, int bonus) {
    if (ply < delta) return;
    Move prev = info->prev_moves[ply - delta];
    if (prev == 0) return;
    int pp = info->prev_pieces[ply - delta];
    if (pp < 0) return;
    int cp = cmh_piece_index(board, MOVE_FROM(m));
    if (cp < 0) return;
    int16_t* e = &tbl[pp][MOVE_TO(prev)][cp][MOVE_TO(m)];
    int v = *e;
    *e = (int16_t)(v + bonus - v * abs(bonus) / CMH_MAX);
}

// Apply bonus/malus to both continuation tables (fmh down-weighted like SF)
static void update_cont_histories(const Board* board, SearchInfo* info, int ply,
                                  Move m, int bonus) {
    update_cont(cmh_table, board, info, ply, 1, m, bonus);
    update_cont(fmh_table, board, info, ply, 2, m, bonus * info->params.fmh_weight / 96);
}

// =============================================================================
// Staged move picker
//
// Yields moves lazily instead of scoring/sorting the full move list up front:
//   1. TT move (validated pseudo-legal, no generation needed)
//   2. generate captures/promotions, yield the good ones (SEE >= 0, promos)
//   3. generate quiet moves, yield by combined history score
//   4. losing captures last (or before the quiets if use_bad_capture_last
//      is disabled)
// Qsearch skips the quiet stage; in-check qsearch uses a single evasion
// stage over all moves.
// =============================================================================

typedef struct {
    Move move;
    int score;
} ScoredMove;

#define TT_MOVE_SCORE 10000000

typedef enum { MP_NORMAL, MP_QSEARCH, MP_EVASION } MovePickerMode;

enum {
    MP_STAGE_TT,
    MP_STAGE_GEN_CAPTURES,
    MP_STAGE_GOOD_CAPTURES,
    MP_STAGE_GEN_QUIETS,
    MP_STAGE_QUIETS,
    MP_STAGE_BAD_CAPTURES,
    MP_STAGE_GEN_EVASIONS,
    MP_STAGE_EVASIONS,
    MP_STAGE_DONE
};

typedef struct {
    Board* board;
    SearchInfo* info;
    int ply;
    Move tt_move;          // validated pseudo-legal TT move, 0 if none
    MovePickerMode mode;
    int stage;
    // list layout: [0, good_count) good captures/promotions,
    // [good_count, cap_count) losing captures, [cap_count, total_count)
    // quiets. In evasion mode all moves live in [0, total_count).
    ScoredMove list[MAX_MOVES];
    int good_count;
    int cap_count;
    int total_count;
    int idx;               // next pick position within the active region
} MovePicker;

static void movepicker_init(MovePicker* mp, Board* board, SearchInfo* info,
                            int ply, Move tt_move, MovePickerMode mode) {
    mp->board = board;
    mp->info = info;
    mp->ply = ply;
    mp->mode = mode;
    mp->stage = MP_STAGE_TT;
    mp->tt_move = 0;
    mp->idx = 0;
    if (tt_move != 0 && moveIsPseudoLegal(board, tt_move)) {
        // Qsearch (not in check) only searches captures/promotions
        if (mode != MP_QSEARCH || MOVE_IS_CAPTURE(tt_move) || MOVE_IS_PROMOTION(tt_move)) {
            mp->tt_move = tt_move;
        }
    }
}

// Selection pick: swap the best-scored entry of [idx, end) to idx, return it
static Move mp_pick(MovePicker* mp, int end, int* score_out) {
    int best = mp->idx;
    for (int i = mp->idx + 1; i < end; i++) {
        if (mp->list[i].score > mp->list[best].score) best = i;
    }
    ScoredMove picked = mp->list[best];
    mp->list[best] = mp->list[mp->idx];
    mp->list[mp->idx] = picked;
    mp->idx++;
    if (score_out) *score_out = picked.score;
    return picked.move;
}

// Score a capture or (non-capture) promotion; *is_good selects the partition
static int mp_capture_score(MovePicker* mp, Move m, bool* is_good) {
    Board* board = mp->board;

    if (MOVE_IS_CAPTURE(m)) {
        int see_value = see(board, m);

        // MVV-LVA as tiebreaker
        bool isWhite = board->whiteToMove;
        bool isBlack = !isWhite;
        PieceTypeToken victim = getPieceTypeAtSquare(board, MOVE_TO(m), &isBlack);
        PieceTypeToken attacker = getPieceTypeAtSquare(board, MOVE_FROM(m), &isWhite);
        int mvv_lva = get_piece_value(victim) * 10 - get_piece_value(attacker);

        *is_good = see_value >= 0;
        if (mp->mode == MP_NORMAL) {
            return *is_good ? 8000000 + see_value * 100 + mvv_lva
                            : -1000000 + see_value * 100 + mvv_lva;
        }
        return *is_good ? 1000000 + see_value * 100 + mvv_lva
                        : see_value * 100 + mvv_lva;
    }

    // Non-capture promotion (kept in the good partition; underpromotions
    // score below every good capture and therefore come after them)
    *is_good = true;
    int promo = MOVE_PROMOTION(m);
    if (mp->mode == MP_NORMAL) {
        return promo == PROMOTION_Q ? 9000000 : 7000000 + get_promotion_value(promo);
    }
    return 500000 + get_promotion_value(promo);
}

static void mp_fill_captures(MovePicker* mp) {
    MoveList caps;
    generateCaptureAndPromotionMoves(mp->board, &caps);

    int n = 0;
    int good = 0;
    for (int i = 0; i < caps.count; i++) {
        Move m = caps.moves[i];
        if (m == mp->tt_move) continue;
        bool is_good;
        int score = mp_capture_score(mp, m, &is_good);
        mp->list[n].move = m;
        mp->list[n].score = score;
        if (is_good) {
            ScoredMove tmp = mp->list[n];
            mp->list[n] = mp->list[good];
            mp->list[good] = tmp;
            good++;
        }
        n++;
    }
    mp->good_count = good;
    mp->cap_count = n;
    mp->total_count = n;
}

static void mp_fill_quiets(MovePicker* mp) {
    MoveList quiets;
    generateQuietMoves(mp->board, &quiets);

    Board* board = mp->board;
    SearchInfo* info = mp->info;
    int side = board->whiteToMove ? 0 : 1;
    int n = mp->cap_count;
    for (int i = 0; i < quiets.count && n < MAX_MOVES; i++) {
        Move m = quiets.moves[i];
        if (m == mp->tt_move) continue;
        // Combined butterfly + continuation history (Stockfish-style:
        // continuation history subsumes killers and countermoves)
        mp->list[n].move = m;
        mp->list[n].score = info->history[side][MOVE_FROM(m)][MOVE_TO(m)]
                          + cont_score(cmh_table, board, info, mp->ply, 1, m)
                          + cont_score(fmh_table, board, info, mp->ply, 2, m);
        n++;
    }
    mp->total_count = n;
}

static void mp_fill_evasions(MovePicker* mp) {
    MoveList all;
    generateMoves(mp->board, &all);

    int n = 0;
    for (int i = 0; i < all.count; i++) {
        Move m = all.moves[i];
        if (m == mp->tt_move) continue;
        int score = 0;
        if (MOVE_IS_CAPTURE(m) || MOVE_IS_PROMOTION(m)) {
            bool is_good;
            score = mp_capture_score(mp, m, &is_good);
        }
        mp->list[n].move = m;
        mp->list[n].score = score;
        n++;
    }
    mp->total_count = n;
}

// Yield the next move, or 0 when exhausted. score_out (optional) receives the
// ordering score - for quiets that is the combined history score used by LMR.
static Move movepicker_next(MovePicker* mp, int* score_out) {
    for (;;) {
        switch (mp->stage) {
        case MP_STAGE_TT:
            mp->stage = (mp->mode == MP_EVASION) ? MP_STAGE_GEN_EVASIONS
                                                 : MP_STAGE_GEN_CAPTURES;
            if (mp->tt_move != 0) {
                if (score_out) *score_out = TT_MOVE_SCORE;
                return mp->tt_move;
            }
            break;

        case MP_STAGE_GEN_CAPTURES:
            mp_fill_captures(mp);
            mp->idx = 0;
            mp->stage = MP_STAGE_GOOD_CAPTURES;
            break;

        case MP_STAGE_GOOD_CAPTURES:
            if (mp->idx < mp->good_count) {
                return mp_pick(mp, mp->good_count, score_out);
            }
            if (mp->mode == MP_QSEARCH || !mp->info->params.use_bad_capture_last) {
                mp->idx = mp->good_count;
                mp->stage = MP_STAGE_BAD_CAPTURES;
            } else {
                mp->stage = MP_STAGE_GEN_QUIETS;
            }
            break;

        case MP_STAGE_BAD_CAPTURES:
            if (mp->idx < mp->cap_count) {
                return mp_pick(mp, mp->cap_count, score_out);
            }
            if (mp->mode == MP_NORMAL && !mp->info->params.use_bad_capture_last) {
                mp->stage = MP_STAGE_GEN_QUIETS;
            } else {
                mp->stage = MP_STAGE_DONE;
            }
            break;

        case MP_STAGE_GEN_QUIETS:
            mp_fill_quiets(mp);
            mp->idx = mp->cap_count;
            mp->stage = MP_STAGE_QUIETS;
            break;

        case MP_STAGE_QUIETS:
            if (mp->idx < mp->total_count) {
                return mp_pick(mp, mp->total_count, score_out);
            }
            if (mp->info->params.use_bad_capture_last) {
                mp->idx = mp->good_count;
                mp->stage = MP_STAGE_BAD_CAPTURES;
            } else {
                mp->stage = MP_STAGE_DONE;
            }
            break;

        case MP_STAGE_GEN_EVASIONS:
            mp_fill_evasions(mp);
            mp->idx = 0;
            mp->stage = MP_STAGE_EVASIONS;
            break;

        case MP_STAGE_EVASIONS:
            if (mp->idx < mp->total_count) {
                return mp_pick(mp, mp->total_count, score_out);
            }
            mp->stage = MP_STAGE_DONE;
            break;

        default:
            return 0;
        }
    }
}

// =============================================================================
// Helper functions
// =============================================================================

// History bonus/malus sizes: linear in depth with a cap (Stockfish-style,
// scaled from SF's 7183 cap to our 16384). Malus is steeper than the bonus
// so refuted moves are unlearned quickly. The LMR history thresholds below
// are calibrated to THESE magnitudes - change them together.
static inline int history_bonus(const SearchInfo* info, int depth) {
    const SearchParams* p = &info->params;
    int b = p->hist_bonus_mult * depth - p->hist_bonus_sub;
    return b > p->hist_bonus_max ? p->hist_bonus_max : b;
}

static inline int history_malus(const SearchInfo* info, int depth) {
    const SearchParams* p = &info->params;
    int m = p->hist_malus_mult * depth - p->hist_malus_sub;
    return m > p->hist_malus_max ? p->hist_malus_max : m;
}

// Update history heuristic with gravity (bonus for good moves, malus for bad)
static void update_history(SearchInfo* info, Board* board, Move m, int depth) {
    int side = board->whiteToMove ? 0 : 1;
    int from = MOVE_FROM(m);
    int to = MOVE_TO(m);

    int bonus = history_bonus(info, depth);

    // Gravity formula to prevent runaway values
    int current = info->history[side][from][to];
    int max_history = 16384;
    info->history[side][from][to] += bonus - (current * abs(bonus) / max_history);
}

// Penalize quiet moves that didn't cause a cutoff
static void update_history_malus(SearchInfo* info, Board* board, Move m, int depth) {
    int side = board->whiteToMove ? 0 : 1;
    int from = MOVE_FROM(m);
    int to = MOVE_TO(m);

    int malus = history_malus(info, depth);
    int current = info->history[side][from][to];
    int max_history = 16384;
    // Gravity with negative bonus: h += -malus - h*|malus|/max.
    // Converges to -max_history instead of diverging below it, and still
    // penalizes moves whose history is at the positive cap.
    info->history[side][from][to] += -malus - (current * abs(malus) / max_history);
}

// Clear all search heuristics (new game / startup)
void clear_search_history(SearchInfo* info) {
    memset(info->history, 0, sizeof(info->history));
    memset(info->prev_moves, 0, sizeof(info->prev_moves));
    memset(info->prev_pieces, 0, sizeof(info->prev_pieces));
    memset(cmh_table, 0, sizeof(cmh_table));
    memset(fmh_table, 0, sizeof(fmh_table));
}

// Clear only ply-indexed state before each search; histories persist
// across moves within a game. Butterfly history is halved (aging) so stale
// entries decay; continuation history is NOT decayed (entries are context-
// specific, Stockfish keeps them un-decayed too).
void clear_volatile_history(SearchInfo* info) {
    memset(info->prev_moves, 0, sizeof(info->prev_moves));
    for (int s = 0; s < 2; s++)
        for (int f = 0; f < 64; f++)
            for (int t = 0; t < 64; t++)
                info->history[s][f][t] /= 2;
}

// Forward declaration
int evaluate(const Board* board, NNUEAccumulator* nnue_acc, const NNUENetwork* nnue_net);

static NNUEAccumulator* search_prepare_nnue_child(SearchInfo* info, int ply) {
    if (info->nnue_acc == NULL || info->nnue_net == NULL || ply + 1 >= MAX_PLY + 2) {
        return info->nnue_acc;
    }

    NNUEAccumulator* child = &info->nnue_stack[ply + 1];
    nnue_prepare_child_accumulator(child, info->nnue_acc);
    return child;
}

// Check time limit (hard limit - sofortiger Abbruch)
static bool check_time(SearchInfo* info) {
    if (info->hardTimeLimit > 0) {
        long elapsed = search_current_time_ms() - info->startTimeMs;
        if (elapsed >= info->hardTimeLimit) {
            info->stopSearch = true;
            return true;
        }
    }
    return false;
}

// Note: Node limit is now a SOFT limit, checked only between iterations
// in iterative_deepening_search() to allow each iteration to complete fully.

// Hilfsfunktion: Verstrichene Zeit in ms
static long get_elapsed_time(SearchInfo* info) {
    return search_current_time_ms() - info->startTimeMs;
}

// Check if position is likely a draw
static bool is_draw(Board* board, int ply) {
    // Draw by 50-move rule
    if (board->halfMoveClock >= 100) {
        return true;
    }
    
    // Draw by repetition
    if (ply > 0) {
        int rep_count = 0;
        for (int i = 0; i < board->historyIndex; i++) {
            if (board->history[i] == board->zobristKey) {
                rep_count++;
                if (rep_count >= 2) {
                    return true;
                }
            }
        }
    }
    
    return false;
}

// Simple check if we can do null move (not in check, have pieces besides pawns)
static bool can_do_null_move(Board* board) {
    if (board->whiteToMove) {
        // Check if white has non-pawn material
        return (board->whiteKnights | board->whiteBishops | 
                board->whiteRooks | board->whiteQueens) != 0;
    } else {
        return (board->blackKnights | board->blackBishops | 
                board->blackRooks | board->blackQueens) != 0;
    }
}

// =============================================================================
// Quiescence Search
// =============================================================================

// Depth used for QS entries in TT (negative to distinguish from main search)
#define QS_TT_DEPTH 0

static int quiescence(Board* board, int alpha, int beta, SearchInfo* info, int ply) {
    // This node was already counted by the caller (the negamax leaf/razoring
    // node, or the parent quiescence before it recursed), so do NOT count it
    // again here - otherwise every qsearch-root node is counted twice.

    // Update selective depth
    if (ply > info->seldepth) {
        info->seldepth = ply;
    }
    
    // Check time limit periodically (node limit is soft - checked between iterations)
    if ((info->nodesSearched & 2047) == 0 && check_time(info)) {
        return 0;
    }
    if (info->stopSearch) return 0;
    
    // Max ply check
    if (ply >= MAX_PLY) {
        int eval = evaluate(board, info->nnue_acc, info->nnue_net);
        return board->whiteToMove ? eval : -eval;
    }
    
    int original_alpha = alpha;
    bool is_pv = (beta - alpha) > 1;

    // ==========================================================================
    // TT Probe in Quiescence Search
    // ==========================================================================
    Move tt_move = 0;
    TT_STAT_INC(tt_probes);
    TTData tte = tt_probe(board->zobristKey);
    bool tt_pv = is_pv || (tte.found && tte.is_pv);
    if (tte.found) {
        TT_STAT_INC(tt_hits);
        tt_move = tte.move;

        // Use TT cutoff if depth is sufficient (QS entries have depth 0)
        if (tte.depth >= QS_TT_DEPTH) {
            int tt_score = tte.score;
            uint8_t tt_flag = tte.bound;
            
            // Adjust ply-dependent (mate and TB) scores
            if (tt_score >= TB_SCORE_MIN) {
                tt_score -= ply;
            } else if (tt_score <= -TB_SCORE_MIN) {
                tt_score += ply;
            }
            
            if (tt_flag == TT_EXACT) {
                TT_STAT_INC(tt_cutoffs);
                return tt_score;
            }
            if (tt_flag == TT_LOWERBOUND && tt_score >= beta) {
                TT_STAT_INC(tt_cutoffs);
                return tt_score;
            }
            if (tt_flag == TT_UPPERBOUND && tt_score <= alpha) {
                TT_STAT_INC(tt_cutoffs);
                return tt_score;
            }
        }
    }
    
    // Check if we're in check
    bool in_check = isKingAttacked(board, board->whiteToMove);
    
    // If in check, we must search all moves (not just captures)
    if (in_check) {
        // Staged picker in evasion mode: TT move first, then all moves
        // (captures ordered by SEE, quiets after)
        MovePicker mp;
        movepicker_init(&mp, board, info, ply, tt_move, MP_EVASION);

        Move best_move = 0;
        int moves_searched = 0;

        Move m;
        while ((m = movepicker_next(&mp, NULL)) != 0) {
            #ifdef DEBUG_ZOBRIST_VERIFY
            uint64_t saved_zobrist = board->zobristKey;
            #endif
            
            NNUEAccumulator* parent_acc = info->nnue_acc;
            NNUEAccumulator* child_acc = search_prepare_nnue_child(info, ply);
            MoveUndoInfo undo;
            applyMove(board, m, &undo, child_acc, info->nnue_net);
            
            // Skip illegal moves (king left in check) - move generator now returns pseudo-legal moves
            if (isKingAttacked(board, !board->whiteToMove)) {
                undoMove(board, m, &undo, child_acc, info->nnue_net);
                continue;
            }
            info->nnue_acc = child_acc;
            
            #ifdef DEBUG_NNUE_EVAL
            eval_set_last_move(m, MOVE_FROM(m), MOVE_TO(m));
            #endif
            info->nodesSearched++;  // count this qsearch child exactly once
            int score = -quiescence(board, -beta, -alpha, info, ply + 1);
            info->nnue_acc = parent_acc;
            undoMove(board, m, &undo, child_acc, info->nnue_net);

            moves_searched++;
            
            #ifdef DEBUG_ZOBRIST_VERIFY
            if (board->zobristKey != saved_zobrist) {
                char move_str[6];
                moveToString(m, move_str);
                printf("info string ZOBRIST MISMATCH (qsearch check) move=%s ply=%d\n", move_str, ply);
                printf("info string   before=0x%llx after=0x%llx\n",
                       (unsigned long long)saved_zobrist, (unsigned long long)board->zobristKey);
                fflush(stdout);
            }
            #endif
            
            if (info->stopSearch) return 0;
            
            if (score >= beta) {
                // TT Store for beta cutoff
                tt_store(board->zobristKey, QS_TT_DEPTH, beta, TT_LOWERBOUND, m, tt_pv, TT_EVAL_NONE);
                return beta;
            }
            if (score > alpha) {
                alpha = score;
                best_move = m;
            }
        }
        
        // No legal moves found: checkmate (we're in check and can't escape)
        if (moves_searched == 0) {
            return -MATE_SCORE + ply;
        }
        
        // TT Store at end
        if (!info->stopSearch) {
            uint8_t tt_flag = (alpha <= original_alpha) ? TT_UPPERBOUND : TT_EXACT;
            tt_store(board->zobristKey, QS_TT_DEPTH, alpha, tt_flag, best_move, tt_pv, TT_EVAL_NONE);
        }

        return alpha;
    }
    
    // Not in check: stand pat, reusing the TT static eval when available
    int stand_pat;
    if (tte.found && tte.eval != TT_EVAL_NONE) {
        stand_pat = tte.eval;
    } else {
        stand_pat = evaluate(board, info->nnue_acc, info->nnue_net);
        if (!board->whiteToMove) {
            stand_pat = -stand_pat;
        }
    }
    
    if (stand_pat >= beta) {
        return beta;
    }
    if (stand_pat > alpha) {
        alpha = stand_pat;
    }
    
    // Delta pruning: if we're so far behind that even capturing a queen won't help
    if (info->params.use_delta_pruning && stand_pat + 900 + info->params.delta_margin < alpha) {
        PRUNING_STAT_INC(delta);
        return alpha;
    }
    
    // Staged picker in qsearch mode: TT move first (only if it is a capture
    // or promotion), then generated captures/promotions, losing captures last
    MovePicker mp;
    movepicker_init(&mp, board, info, ply, tt_move, MP_QSEARCH);
    tt_move = mp.tt_move;  // validated - used for the SEE-pruning exemption below

    Move best_move = 0;

    Move m;
    while ((m = movepicker_next(&mp, NULL)) != 0) {
        // Delta pruning: skip captures that can't possibly raise alpha
        if (info->params.use_delta_pruning && !MOVE_IS_PROMOTION(m)) {
            int gain;
            if (MOVE_IS_EN_PASSANT(m)) {
                gain = 100; // Pawn value
            } else {
                bool isBlack = !board->whiteToMove;
                PieceTypeToken victim = getPieceTypeAtSquare(board, MOVE_TO(m), &isBlack);
                gain = get_piece_value(victim);
            }

            if (stand_pat + gain + info->params.delta_margin < alpha) {
                PRUNING_STAT_INC(delta);
                continue;
            }
        }

        // SEE pruning: skip captures that lose material (not in check here by branch).
        // Keep the TT move and promotions, which may be tactically necessary.
        if (info->params.use_qs_see_pruning && !MOVE_IS_PROMOTION(m) && m != tt_move) {
            if (see(board, m) < 0) {
                PRUNING_STAT_INC(see_pruning);
                continue;
            }
        }

        #ifdef DEBUG_ZOBRIST_VERIFY
        uint64_t saved_zobrist = board->zobristKey;
        #endif

        NNUEAccumulator* parent_acc = info->nnue_acc;
        NNUEAccumulator* child_acc = search_prepare_nnue_child(info, ply);
        MoveUndoInfo undo;
        applyMove(board, m, &undo, child_acc, info->nnue_net);

        // Skip illegal moves (king left in check) - needed because generateCaptureAndPromotionMoves is pseudo-legal
        if (isKingAttacked(board, !board->whiteToMove)) {
            undoMove(board, m, &undo, child_acc, info->nnue_net);
            continue;
        }
        info->nnue_acc = child_acc;
        
        #ifdef DEBUG_NNUE_EVAL
        eval_set_last_move(m, MOVE_FROM(m), MOVE_TO(m));
        #endif
        info->nodesSearched++;  // count this qsearch child exactly once
        int score = -quiescence(board, -beta, -alpha, info, ply + 1);
        info->nnue_acc = parent_acc;
        undoMove(board, m, &undo, child_acc, info->nnue_net);

        #ifdef DEBUG_ZOBRIST_VERIFY
        if (board->zobristKey != saved_zobrist) {
            char move_str[6];
            moveToString(m, move_str);
            printf("info string ZOBRIST MISMATCH (qsearch) move=%s ply=%d\n", move_str, ply);
            printf("info string   before=0x%llx after=0x%llx\n",
                   (unsigned long long)saved_zobrist, (unsigned long long)board->zobristKey);
            fflush(stdout);
        }
        #endif
        
        if (info->stopSearch) return 0;
        
        if (score >= beta) {
            // TT Store for beta cutoff
            tt_store(board->zobristKey, QS_TT_DEPTH, beta, TT_LOWERBOUND, m, tt_pv, stand_pat);
            return beta;
        }
        if (score > alpha) {
            alpha = score;
            best_move = m;
        }
    }
    
    // TT Store at end of QS
    if (!info->stopSearch) {
        uint8_t tt_flag = (alpha <= original_alpha) ? TT_UPPERBOUND : TT_EXACT;
        tt_store(board->zobristKey, QS_TT_DEPTH, alpha, tt_flag, best_move, tt_pv, stand_pat);
    }
    
    return alpha;
}

// =============================================================================
// Negamax with Alpha-Beta Pruning
// =============================================================================

static int negamax(Board* board, int depth, int alpha, int beta, SearchInfo* info, 
                   int ply, bool do_null, bool is_null_move_search) {
    info->nodesSearched++;
    info->pv_length[ply] = 0;
    
    bool is_pv = (beta - alpha) > 1;  // Are we in a PV node?
    int original_alpha = alpha;
    
    // Check time limit periodically (node limit is soft - checked between iterations)
    if (ply > 0 && (info->nodesSearched & 2047) == 0 && check_time(info)) {
        return 0;
    }
    if (info->stopSearch) return 0;
    
    // Draw detection
    if (ply > 0 && is_draw(board, ply)) {
        return 0;
    }
    
    // Max ply check
    if (ply >= MAX_PLY) {
        int eval = evaluate(board, info->nnue_acc, info->nnue_net);
        return board->whiteToMove ? eval : -eval;  // Convert from White perspective to side-to-move
    }
    
    // Check if in check (needed for various extensions/reductions)
    bool in_check = isKingAttacked(board, board->whiteToMove);
    
    // Check extension
    if (in_check && info->params.use_check_extension) {
        depth++;
    }
    
    // TT Probe
    Move tt_move = 0;
    TT_STAT_INC(tt_probes);
    TTData tte = tt_probe(board->zobristKey);
    bool tt_pv = is_pv || (tte.found && tte.is_pv);
    if (tte.found) {
        TT_STAT_INC(tt_hits);
        tt_move = tte.move;

        // Only use TT cutoff in non-PV nodes
        if (!is_pv && tte.depth >= depth && ply > 0) {
            int tt_score = tte.score;
            uint8_t tt_flag = tte.bound;
            
            // Adjust ply-dependent (mate and TB) scores
            if (tt_score >= TB_SCORE_MIN) {
                tt_score -= ply;
            } else if (tt_score <= -TB_SCORE_MIN) {
                tt_score += ply;
            }
            
            if (tt_flag == TT_EXACT) {
                TT_STAT_INC(tt_cutoffs);
                return tt_score;
            }
            if (tt_flag == TT_LOWERBOUND && tt_score >= beta) {
                TT_STAT_INC(tt_cutoffs);
                return tt_score;
            }
            if (tt_flag == TT_UPPERBOUND && tt_score <= alpha) {
                TT_STAT_INC(tt_cutoffs);
                return tt_score;
            }
        }
    }
    
    // ==========================================================================
    // Syzygy tablebase WDL probe (interior, non-root nodes)
    //
    // Only when castling is impossible and the 50-move counter is zero (Fathom's
    // WDL probe requires rule50 == 0). Gives an exact win/draw/loss verdict that
    // overrides the speculative pruning below.
    // ==========================================================================
    if (ply > 0 && info->tbProbeLimit > 0 &&
        board->castlingRights == NO_CASTLING && board->halfMoveClock == 0) {
        Bitboard tb_occ =
            board->byTypeBB[WHITE][PAWN]   | board->byTypeBB[BLACK][PAWN]   |
            board->byTypeBB[WHITE][KNIGHT] | board->byTypeBB[BLACK][KNIGHT] |
            board->byTypeBB[WHITE][BISHOP] | board->byTypeBB[BLACK][BISHOP] |
            board->byTypeBB[WHITE][ROOK]   | board->byTypeBB[BLACK][ROOK]   |
            board->byTypeBB[WHITE][QUEEN]  | board->byTypeBB[BLACK][QUEEN]  |
            board->byTypeBB[WHITE][KING]   | board->byTypeBB[BLACK][KING];
        int tb_pieces = POPCOUNT(tb_occ);
        int wdl;
        if (tb_pieces <= info->tbProbeLimit && syzygy_available(tb_pieces) &&
            syzygy_probe_wdl(board, &wdl)) {
            info->tbHits++;
            int tb_score = (wdl > 0) ?  (TB_WIN_SCORE - ply)
                         : (wdl < 0) ? -(TB_WIN_SCORE - ply)
                         :              0;
            // Store exact, applying the same ply-offset used on store below.
            int store_score = tb_score;
            if (store_score >= TB_SCORE_MIN)       store_score += ply;
            else if (store_score <= -TB_SCORE_MIN) store_score -= ply;
            tt_store(board->zobristKey, depth, store_score, TT_EXACT, 0, tt_pv, TT_EVAL_NONE);
            return tb_score;
        }
    }

    // Quiescence search at leaf nodes
    if (depth <= 0) {
        return quiescence(board, alpha, beta, info, ply);
    }
    
    bool can_null = info->params.use_null_move && do_null && !in_check && !is_pv &&
        depth >= info->params.null_move_min_depth && can_do_null_move(board);
    bool can_rfp = info->params.use_rfp && !is_pv && !in_check &&
        depth <= info->params.rfp_max_depth && abs(beta) < TB_SCORE_MIN;
    bool can_razor = info->params.use_razoring && !is_pv && !in_check &&
        depth <= 3 && abs(alpha) < TB_SCORE_MIN;
    bool can_futility = info->params.use_futility && !is_pv && !in_check &&
        depth <= 3 && abs(alpha) < TB_SCORE_MIN && abs(beta) < TB_SCORE_MIN;
    bool can_lmp = info->params.use_lmp && !is_pv && !in_check &&
        depth <= info->params.lmp_max_depth && abs(alpha) < TB_SCORE_MIN;
    int lmp_threshold = info->params.lmp_base + depth * depth;

    // Static eval only where pruning needs it, reusing the TT copy when
    // available; stays TT_EVAL_NONE otherwise (in check / PV nodes)
    int static_eval = TT_EVAL_NONE;
    if (can_null || can_rfp || can_razor || can_futility) {
        if (tte.found && tte.eval != TT_EVAL_NONE) {
            static_eval = tte.eval;
        } else {
            static_eval = evaluate(board, info->nnue_acc, info->nnue_net);
            if (!board->whiteToMove) {
                static_eval = -static_eval;
            }
        }
    }

    // ==========================================================================
    // Null Move Pruning
    // Adaptive reduction (deeper searches and larger eval margins allow larger
    // reductions); only tried when static eval already fails high, so no
    // verification search is needed.
    // ==========================================================================
    if (can_null && static_eval >= beta) {

        // Make null move - update zobrist key for consistent TT usage
        board->whiteToMove = !board->whiteToMove;
        board->zobristKey ^= zobrist_side_to_move_key;
        
        int old_ep = board->enPassantSquare;
        if (old_ep != SQ_NONE) {
            board->zobristKey ^= zobrist_enpassant_keys[old_ep];
        }
        board->enPassantSquare = SQ_NONE;

        // No previous move after a null move - prevents stale counter-move
        // and continuation history lookups in the child node
        info->prev_moves[ply] = 0;

        // Adaptive reduction: base 3, grows with depth and with the margin by
        // which the static eval exceeds beta (capped so shallow real search
        // remains for zugzwang-ish positions).
        int eval_term = (static_eval - beta) / 200;
        if (eval_term > 3) eval_term = 3;
        int null_reduction = 3 + depth / 3 + eval_term;

        // Search with reduced depth - mark as null move search to skip TT store for this node
        int null_score = -negamax(board, depth - 1 - null_reduction, -beta, -beta + 1,
                                  info, ply + 1, false, true);
        
        // Unmake null move - restore zobrist key
        board->enPassantSquare = old_ep;
        if (old_ep != SQ_NONE) {
            board->zobristKey ^= zobrist_enpassant_keys[old_ep];
        }
        board->whiteToMove = !board->whiteToMove;
        board->zobristKey ^= zobrist_side_to_move_key;
        
        if (info->stopSearch) return 0;
        
        if (null_score >= beta) {
            PRUNING_STAT_INC(null_move);
            // Never return unproven mate/TB scores from a null-window search
            return null_score >= TB_SCORE_MIN ? beta : null_score;
        }
    }


    // ==========================================================================
    // Reverse Futility Pruning (Static Null Move Pruning)
    // If static eval is way above beta, we can assume a beta cutoff
    // ==========================================================================
    if (can_rfp) {
        int rfp_margin = info->params.rfp_margin * depth;
        if (static_eval - rfp_margin >= beta) {
            PRUNING_STAT_INC(reverse_futility);
            return static_eval - rfp_margin;
        }
    }

    // ==========================================================================
    // Razoring: If static eval is far below alpha, drop into qsearch
    // Very effective at low depths to cut hopeless positions early
    // ==========================================================================
    if (can_razor) {
        int razor_margin = info->params.razor_margin + info->params.razor_margin * (depth - 1);
        if (static_eval + razor_margin < alpha) {
            int razor_score = quiescence(board, alpha - 1, alpha, info, ply);
            if (razor_score < alpha) {
                PRUNING_STAT_INC(razoring);
                return razor_score;
            }
        }
    }

    // ==========================================================================
    // Futility Pruning (now enabled with NNUE)
    // ==========================================================================
    bool futility_pruning = false;
    int futility_margin = 0;
    if (can_futility) {
        if (depth == 1) {
            futility_margin = info->params.futility_margin;
        } else if (depth == 2) {
            futility_margin = info->params.futility_margin_d2;
        } else {
            futility_margin = info->params.futility_margin_d3;
        }
        if (static_eval + futility_margin <= alpha) {
            futility_pruning = true;
        }
    }
    
    // Staged move picker: TT move first (validated pseudo-legal, no move
    // generation needed), then captures/promotions, then quiet moves.
    // Legality is still checked after applyMove.
    MovePicker mp;
    movepicker_init(&mp, board, info, ply, tt_move, MP_NORMAL);

    Move best_move = 0;
    int moves_searched = 0;

    // Quiet moves yielded so far (for the history malus on a beta cutoff)
    Move quiets_tried[MAX_MOVES];
    int quiets_tried_count = 0;

    Move m;
    int move_score;
    while ((m = movepicker_next(&mp, &move_score)) != 0) {
        // Syzygy root-move restriction: at the root, only search TB-optimal moves.
        if (ply == 0 && info->tbRootMoveCount > 0) {
            bool tb_allowed = false;
            for (int k = 0; k < info->tbRootMoveCount; k++) {
                if (info->tbRootMoves[k] == m) { tb_allowed = true; break; }
            }
            if (!tb_allowed) continue;
        }

        bool is_capture = MOVE_IS_CAPTURE(m);
        bool is_promotion = MOVE_IS_PROMOTION(m);
        bool is_tactical = is_capture || is_promotion;

        if (!is_tactical && quiets_tried_count < MAX_MOVES) {
            quiets_tried[quiets_tried_count++] = m;
        }

        // =======================================================================
        // Futility Pruning: skip quiet moves that can't improve alpha
        // =======================================================================
        if (futility_pruning && moves_searched > 0 && !is_tactical) {
            PRUNING_STAT_INC(futility);
            continue;
        }

        // =======================================================================
        // Late Move Pruning: quiets ordered this late at shallow depth almost
        // never raise alpha - skip them entirely (movecount-based pruning)
        // =======================================================================
        if (can_lmp && !is_tactical && moves_searched >= lmp_threshold) {
            PRUNING_STAT_INC(lmp);
            continue;
        }

        // Prefetch TT entry for likely next position
        // (This is a simple optimization that can help cache performance)
        
        // DEBUG: Save Zobrist key before move for verification
        #ifdef DEBUG_ZOBRIST_VERIFY
        uint64_t saved_zobrist = board->zobristKey;
        #endif
        
        // DEBUG: Save accumulator state before move
        #ifdef DEBUG_NNUE_INCREMENTAL
        NNUEAccumulator saved_acc;
        if (info->nnue_acc) {
            memcpy(&saved_acc, info->nnue_acc, sizeof(NNUEAccumulator));
        }
        #endif
        
        NNUEAccumulator* parent_acc = info->nnue_acc;
        NNUEAccumulator* child_acc = search_prepare_nnue_child(info, ply);
        MoveUndoInfo undo;
        applyMove(board, m, &undo, child_acc, info->nnue_net);
        
        // Skip illegal moves (king left in check) - move generator now returns pseudo-legal moves
        if (isKingAttacked(board, !board->whiteToMove)) {
            undoMove(board, m, &undo, child_acc, info->nnue_net);
            continue;
        }
        info->nnue_acc = child_acc;
        
        // Track last move for debug
        #ifdef DEBUG_NNUE_EVAL
        eval_set_last_move(m, MOVE_FROM(m), MOVE_TO(m));
        #endif
        
        // Prefetch next position's TT entry
        tt_prefetch(board->zobristKey);

        // Track this move for continuation history. Piece recorded
        // post-applyMove: the mover (or promoted piece) on `to`.
        info->prev_moves[ply] = m;
        info->prev_pieces[ply] = cmh_piece_index(board, MOVE_TO(m));

        int score;

        // =======================================================================
        // Principal Variation Search (PVS) with Late Move Reductions (LMR)
        // =======================================================================
        if (moves_searched == 0) {
            // First move: full window search
            score = -negamax(board, depth - 1, -beta, -alpha, info, ply + 1, true, false);
        } else {
            // Late Move Reductions - logarithmic formula like Stockfish
            int reduction = 0;

            if (info->params.use_lmr && !in_check && !is_tactical &&
                depth >= info->params.lmr_reduction_limit &&
                moves_searched >= info->params.lmr_full_depth_moves) {

                // Lookup from precomputed table (Stockfish-style formula)
                int lmr_depth = depth < MAX_PLY ? depth : MAX_PLY - 1;
                int lmr_moves = moves_searched < 64 ? moves_searched : 63;
                reduction = lmr_table[lmr_depth][lmr_moves];

                // Increase reduction for non-PV nodes
                if (!is_pv) {
                    reduction++;
                }

                // Adjust based on the combined history score (butterfly +
                // continuation history = this quiet's ordering score, computed
                // before applyMove). Thresholds match the SF-scale update
                // magnitudes in history_bonus()/history_malus().
                int stat = move_score;
                if (stat < info->params.lmr_stat_low2) {
                    reduction += 2;
                } else if (stat < info->params.lmr_stat_low1) {
                    reduction++;
                } else if (stat > info->params.lmr_stat_high2) {
                    reduction -= 2;
                } else if (stat > info->params.lmr_stat_high1) {
                    reduction--;
                }

                // Clamp reduction: at least 1, don't reduce below depth 1
                if (reduction < 1) reduction = 1;
                if (depth - 1 - reduction < 1) {
                    reduction = depth - 2;
                }
                if (reduction < 0) reduction = 0;
            }
            
            // Null window search with possible reduction
            if (reduction > 0) {
                PRUNING_STAT_INC(lmr);
            }
            score = -negamax(board, depth - 1 - reduction, -alpha - 1, -alpha, 
                            info, ply + 1, true, false);
            
            // Re-search if LMR failed high
            if (reduction > 0 && score > alpha) {
                score = -negamax(board, depth - 1, -alpha - 1, -alpha, info, ply + 1, true, false);
            }
            
            // Re-search with full window if null window failed high
            if (score > alpha && score < beta) {
                score = -negamax(board, depth - 1, -beta, -alpha, info, ply + 1, true, false);
            }
        }
        
        info->nnue_acc = parent_acc;
        undoMove(board, m, &undo, child_acc, info->nnue_net);
        
        // DEBUG: Verify Zobrist hash is correctly restored after undo
        #ifdef DEBUG_ZOBRIST_VERIFY
        if (board->zobristKey != saved_zobrist) {
            char move_str[6];
            moveToString(m, move_str);
            printf("info string ZOBRIST MISMATCH after undo move=%s ply=%d\n", move_str, ply);
            printf("info string   before=0x%llx after=0x%llx diff=0x%llx\n", 
                   (unsigned long long)saved_zobrist, 
                   (unsigned long long)board->zobristKey,
                   (unsigned long long)(saved_zobrist ^ board->zobristKey));
            
            // Additional info about the move
            int from_sq = MOVE_FROM(m);
            int to_sq = MOVE_TO(m);
            printf("info string   from=%d to=%d cap=%d ep=%d castle=%d promo=%d\n",
                   from_sq, to_sq, MOVE_IS_CAPTURE(m), MOVE_IS_EN_PASSANT(m), 
                   MOVE_IS_CASTLING(m), MOVE_IS_PROMOTION(m));
            
            // Recalculate and compare
            uint64_t recalc = calculate_zobrist_key(board);
            printf("info string   recalculated=0x%llx matches_saved=%d matches_current=%d\n",
                   (unsigned long long)recalc, 
                   (recalc == saved_zobrist), 
                   (recalc == board->zobristKey));
            fflush(stdout);
        }
        #endif
        
        // DEBUG: Compare accumulator state after undo
        #ifdef DEBUG_NNUE_INCREMENTAL
        if (info->nnue_acc && saved_acc.computed && info->nnue_acc->computed) {
            bool mismatch = false;
            int first_mismatch_white = -1, first_mismatch_black = -1;
            int16_t diff_white = 0, diff_black = 0;
            for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
                if (info->nnue_acc->white[i] != saved_acc.white[i]) {
                    if (first_mismatch_white < 0) {
                        first_mismatch_white = i;
                        diff_white = info->nnue_acc->white[i] - saved_acc.white[i];
                    }
                    mismatch = true;
                }
                if (info->nnue_acc->black[i] != saved_acc.black[i]) {
                    if (first_mismatch_black < 0) {
                        first_mismatch_black = i;
                        diff_black = info->nnue_acc->black[i] - saved_acc.black[i];
                    }
                    mismatch = true;
                }
            }
            if (mismatch) {
                char move_str[6];
                moveToString(m, move_str);
                int from_sq = MOVE_FROM(m);
                int to_sq = MOVE_TO(m);
                bool is_capture = MOVE_IS_CAPTURE(m);
                bool is_ep = MOVE_IS_EN_PASSANT(m);
                bool is_castle = MOVE_IS_CASTLING(m);
                bool is_promo = MOVE_IS_PROMOTION(m);
                printf("info string NNUE MISMATCH move=%s ply=%d from=%d to=%d cap=%d ep=%d castle=%d promo=%d\n", 
                       move_str, ply, from_sq, to_sq, is_capture, is_ep, is_castle, is_promo);
                printf("info string   white_idx=%d diff=%d | black_idx=%d diff=%d\n",
                       first_mismatch_white, diff_white, first_mismatch_black, diff_black);
                fflush(stdout);
            }
        }
        #endif
        
        moves_searched++;
        
        if (info->stopSearch) return 0;
        
        if (score > alpha) {
            alpha = score;
            best_move = m;
            
            // Update PV
            info->pv_table[ply][0] = m;
            for (int j = 0; j < info->pv_length[ply + 1]; j++) {
                info->pv_table[ply][j + 1] = info->pv_table[ply + 1][j];
            }
            info->pv_length[ply] = info->pv_length[ply + 1] + 1;
            
            if (ply == 0) {
                info->bestMoveThisIteration = m;
                info->bestScoreThisIteration = score;  // Track score for training mode
            }
        }
        
        if (alpha >= beta) {
#ifdef SEARCH_STATS
            beta_cutoffs++;
            if (moves_searched == 1) beta_cutoffs_first++;
#endif
            // Beta cutoff - update histories for quiet moves
            if (!is_capture) {
                update_history(info, board, m, depth);
                update_cont_histories(board, info, ply, m, history_bonus(info, depth));

                // Apply malus to all previously yielded quiet moves
                for (int j = 0; j < quiets_tried_count; j++) {
                    Move prev = quiets_tried[j];
                    if (prev == m) continue;
                    update_history_malus(info, board, prev, depth);
                    update_cont_histories(board, info, ply, prev, -history_malus(info, depth));
                }
            }
            break;
        }
    }
    
    // No legal moves found: checkmate or stalemate
    // (moves_searched == 0 means all pseudo-legal moves were illegal)
    if (moves_searched == 0) {
        if (in_check) {
            return -MATE_SCORE + ply;
        }
        return 0;  // Stalemate
    }
    
    // TT Store - skip for null move search positions (they can never be reached legally)
    if (!info->stopSearch && !is_null_move_search) {
        uint8_t tt_flag;
        if (alpha <= original_alpha) {
            tt_flag = TT_UPPERBOUND;
        } else if (alpha >= beta) {
            tt_flag = TT_LOWERBOUND;
        } else {
            tt_flag = TT_EXACT;
        }
        
        // Adjust mate scores for storage
        int store_score = alpha;
        if (store_score >= TB_SCORE_MIN) {
            store_score += ply;
        } else if (store_score <= -TB_SCORE_MIN) {
            store_score -= ply;
        }
        
        tt_store(board->zobristKey, depth, store_score, tt_flag, best_move, tt_pv, static_eval);
    }
    
    return alpha;
}

// =============================================================================
// Iterative Deepening with Aspiration Windows
// =============================================================================

Move iterative_deepening_search(Board* board, SearchInfo* info) {
    if (!search_silent_mode) {
        printf("DEBUG: Starting search. White to move: %s\n", board->whiteToMove ? "true" : "false");
        printf("info string Time limits: soft=%ld ms, hard=%ld ms\n", info->softTimeLimit, info->hardTimeLimit);
        fflush(stdout);
    }

    Move best_move = 0;
    int best_score = 0;
    NNUEAccumulator* external_acc = info->nnue_acc;
    if (external_acc != NULL && info->nnue_net != NULL) {
        memcpy(&info->nnue_stack[0], external_acc, sizeof(NNUEAccumulator));
        info->nnue_stack[0].previous = NULL;
        info->nnue_acc = &info->nnue_stack[0];
    }
    
    info->nodesSearched = 0;
    info->lastIterationTime = 0;
    info->seldepth = 0;
    info->tbHits = 0;

    // Reset search statistics
    TT_STATS_RESET();
    PRUNING_STATS_RESET();
    
    // Initialize TT for new search
    tt_new_search();
    
    // Track longest meaningful iteration time
    long max_meaningful_iteration_time = 0;
    
    for (int i = 0; i < MAX_PLY; i++) {
        info->pv_length[i] = 0;
    }
    
    // Aspiration window variables
    int alpha = -INT_MAX + 1;
    int beta = INT_MAX - 1;
    int prev_score = 0;
    
    int max_depth = (info->depthLimit > 0) ? info->depthLimit : MAX_PLY;
    for (int depth = 1; depth <= max_depth; depth++) {
        long iteration_start = get_elapsed_time(info);
        int nodes_before = info->nodesSearched;
        
        info->bestMoveThisIteration = 0;
        info->seldepth = 0;
        
        int score;
        
        // Use aspiration windows after depth 4
        if (info->params.use_aspiration && depth >= 5) {
            alpha = prev_score - info->params.aspiration_window;
            beta = prev_score + info->params.aspiration_window;
            
            while (true) {
                score = negamax(board, depth, alpha, beta, info, 0, true, false);
                
                if (info->stopSearch) break;
                
                // Failed low - widen alpha
                if (score <= alpha) {
                    alpha = -INT_MAX + 1;
                }
                // Failed high - widen beta  
                else if (score >= beta) {
                    beta = INT_MAX - 1;
                }
                // Within window
                else {
                    break;
                }
            }
        } else {
            score = negamax(board, depth, alpha, beta, info, 0, true, false);
        }
        
        long iteration_end = get_elapsed_time(info);
        info->lastIterationTime = iteration_end - iteration_start;
        int nodes_this_iteration = info->nodesSearched - nodes_before;
        
        // Update max meaningful iteration time
        if (info->lastIterationTime >= 10 && nodes_this_iteration >= 1000) {
            max_meaningful_iteration_time = info->lastIterationTime;
        }
        
        if (info->stopSearch) {
            if (!search_silent_mode) {
                printf("info string Search stopped at depth %d (hard limit reached)\n", depth);
                fflush(stdout);
            }
            // DO NOT update best_move or best_score from incomplete iteration!
            // The bestMoveThisIteration might be from a partial search and unreliable.
            // Keep the best_move and bestScoreThisIteration from the last completed iteration.
            break;
        }
        
        // Only update bestScoreThisIteration after a COMPLETED iteration
        info->bestScoreThisIteration = score;
        
        // Update best move from completed iteration
        if (info->bestMoveThisIteration != 0) {
            best_move = info->bestMoveThisIteration;
            best_score = score;
            prev_score = score;
        }
        
        // UCI output
        long time_ms = get_elapsed_time(info);
        uint64_t nps = time_ms > 0 ? (info->nodesSearched * 1000ULL / time_ms) : 0;
        int hashfull = tt_hashfull();
        
        // Format the score for UCI (side-to-move perspective; the internal
        // negamax score already is). "mate N" is only reported for a known
        // mate distance: a real search mate, or a root TB hit whose PV was
        // walked out to mate. TB scores without a mate distance are shown as
        // a fixed large cp value so GUIs never see a false mate claim.
        char score_str[32];
        if (info->tbRootMoveCount > 0) {
            // Root TB hit: report the DTZ-optimal verdict, not the search score.
            int sign = (info->tbRootScore > 0) - (info->tbRootScore < 0);
            if (info->tbRootMatePlies >= 0 && sign != 0) {
                int mate_moves = (info->tbRootMatePlies + 1) / 2;
                snprintf(score_str, sizeof(score_str), "mate %d", sign * mate_moves);
            } else {
                snprintf(score_str, sizeof(score_str), "cp %d", sign * TB_DISPLAY_CP);
            }
        } else if (score > TB_WIN_SCORE) {
            int plies = MATE_SCORE - score;
            snprintf(score_str, sizeof(score_str), "mate %d", (plies + 1) / 2);
        } else if (score < -TB_WIN_SCORE) {
            int plies = MATE_SCORE + score;
            snprintf(score_str, sizeof(score_str), "mate %d", -((plies + 1) / 2));
        } else if (score >= TB_SCORE_MIN) {
            // TB win propagated from the subtree: proven win, unknown distance.
            snprintf(score_str, sizeof(score_str), "cp %d", TB_DISPLAY_CP);
        } else if (score <= -TB_SCORE_MIN) {
            snprintf(score_str, sizeof(score_str), "cp %d", -TB_DISPLAY_CP);
        } else {
            snprintf(score_str, sizeof(score_str), "cp %d", score);
        }

        if (!search_silent_mode) {
            printf("info depth %d seldepth %d score %s nodes %llu nps %llu time %ld hashfull %d tbhits %llu pv",
                   depth, info->seldepth, score_str, (unsigned long long)info->nodesSearched, (unsigned long long)nps, time_ms, hashfull, (unsigned long long)info->tbHits);
            
            if (info->tbRootMoveCount > 0 && info->tbRootPvLen > 0) {
                // Tablebase root hit: report the DTZ-optimal line.
                for (int i = 0; i < info->tbRootPvLen; i++) {
                    char move_str[10];
                    moveToString(info->tbRootPv[i], move_str);
                    printf(" %s", move_str);
                }
            } else {
                for (int i = 0; i < info->pv_length[0]; i++) {
                    if (info->pv_table[0][i] == 0) break;
                    char move_str[10];
                    moveToString(info->pv_table[0][i], move_str);
                    printf(" %s", move_str);
                }
            }
            printf("\n");
            fflush(stdout);
        }
        
        // Stop if mate found. Strictly above TB_WIN_SCORE means a real mate
        // score; TB win scores (<= TB_WIN_SCORE) must NOT stop the search,
        // since they carry no mate distance and deeper iterations are needed
        // to actually convert the win.
        if (abs(score) > TB_WIN_SCORE) {
            if (!search_silent_mode) {
                printf("info string Mate found, stopping search\n");
                fflush(stdout);
            }
            break;
        }
        
        // Soft node limit - check between iterations (allows current iteration to complete)
        if (info->nodeLimit > 0 && info->nodesSearched >= info->nodeLimit) {
            if (!search_silent_mode) {
                printf("info string Soft node limit reached after depth %d (%llu nodes)\n", 
                       depth, (unsigned long long)info->nodesSearched);
                fflush(stdout);
            }
            break;
        }
        
        // Time management
        if (info->softTimeLimit > 0) {
            long remaining = info->softTimeLimit - time_ms;
            
            long time_for_estimate = max_meaningful_iteration_time > 0 ? 
                                     max_meaningful_iteration_time : info->lastIterationTime;
            long estimated_next = time_for_estimate * 3;
            
            if (time_ms >= info->softTimeLimit) {
                printf("info string Soft time limit reached after depth %d\n", depth);
                fflush(stdout);
                break;
            }
            
            bool enough_time_for_next = (estimated_next <= remaining);
            bool still_early = (time_ms < (info->softTimeLimit * 60) / 100);
            
            if (!enough_time_for_next && !still_early) {
                if (!search_silent_mode) {
                    printf("info string Stopping before depth %d (estimated: %ld ms, remaining: %ld ms)\n",
                           depth + 1, estimated_next, remaining);
                    fflush(stdout);
                }
                break;
            }
        }
    }
    
    // Fallback: If we somehow have no best move (e.g., search stopped at depth 1 before finding anything),
    // use the bestMoveThisIteration if available, otherwise return 0 (shouldn't happen in legal position)
    if (best_move == 0 && info->bestMoveThisIteration != 0) {
        best_move = info->bestMoveThisIteration;
        if (!search_silent_mode) {
            printf("info string Using fallback move from incomplete iteration\n");
            fflush(stdout);
        }
    }
    
#ifdef DEBUG_HISTORY_RANGE
    {
        int hmin = 0, hmax = 0; long long below_cap = 0, nonzero = 0;
        for (int s = 0; s < 2; s++)
            for (int f = 0; f < 64; f++)
                for (int t = 0; t < 64; t++) {
                    int h = info->history[s][f][t];
                    if (h < hmin) hmin = h;
                    if (h > hmax) hmax = h;
                    if (h < -16384) below_cap++;
                    if (h != 0) nonzero++;
                }
        long long lmr_neg = 0, lmr_pos = 0;
        for (int s = 0; s < 2; s++)
            for (int f = 0; f < 64; f++)
                for (int t = 0; t < 64; t++) {
                    int h = info->history[s][f][t];
                    if (h < -2000) lmr_neg++;
                    if (h > 8000) lmr_pos++;
                }
        printf("info string HISTORY range=[%d,%d] below_cap=%lld nonzero=%lld lmr_neg=%lld lmr_pos=%lld\n",
               hmin, hmax, below_cap, nonzero, lmr_neg, lmr_pos);
        fflush(stdout);
    }
#endif
    (void)best_score;
    if (!search_silent_mode) {
        printf("DEBUG: Best move: %u, Total time: %ld ms\n", best_move, get_elapsed_time(info));
#ifdef SEARCH_STATS
        // Print TT statistics
        double hit_rate = tt_probes > 0 ? (100.0 * tt_hits / tt_probes) : 0;
        double cutoff_rate = tt_hits > 0 ? (100.0 * tt_cutoffs / tt_hits) : 0;
        printf("info string TT stats: probes=%llu hits=%llu (%.1f%%) cutoffs=%llu (%.1f%% of hits)\n",
               (unsigned long long)tt_probes, (unsigned long long)tt_hits, hit_rate,
               (unsigned long long)tt_cutoffs, cutoff_rate);
        // Move ordering quality: how often the first searched move caused the beta cutoff
        double fh_first = beta_cutoffs > 0 ? (100.0 * beta_cutoffs_first / beta_cutoffs) : 0;
        printf("info string Ordering: beta_cutoffs=%llu first_move=%llu (%.2f%%)\n",
               (unsigned long long)beta_cutoffs, (unsigned long long)beta_cutoffs_first, fh_first);
        // Print pruning statistics
        uint64_t total_prunings = pruning_stats.null_move + pruning_stats.reverse_futility +
                                  pruning_stats.razoring + pruning_stats.futility +
                                  pruning_stats.lmp + pruning_stats.lmr +
                                  pruning_stats.delta + pruning_stats.see_pruning;
        printf("info string Pruning stats (total=%llu):\n", (unsigned long long)total_prunings);
        printf("info string   Null Move:    %llu\n", (unsigned long long)pruning_stats.null_move);
        printf("info string   Rev Futility: %llu\n", (unsigned long long)pruning_stats.reverse_futility);
        printf("info string   Razoring:     %llu\n", (unsigned long long)pruning_stats.razoring);
        printf("info string   Futility:     %llu\n", (unsigned long long)pruning_stats.futility);
        printf("info string   LMP:          %llu\n", (unsigned long long)pruning_stats.lmp);
        printf("info string   LMR:          %llu\n", (unsigned long long)pruning_stats.lmr);
        printf("info string   Delta:        %llu\n", (unsigned long long)pruning_stats.delta);
        printf("info string   SEE Pruning:  %llu\n", (unsigned long long)pruning_stats.see_pruning);
#endif
        fflush(stdout);
    }
    if (external_acc != NULL && info->nnue_acc != NULL) {
        memcpy(external_acc, &info->nnue_stack[0], sizeof(NNUEAccumulator));
        external_acc->previous = NULL;
        info->nnue_acc = external_acc;
    }

    return best_move;
}

// Compatibility functions
int alpha_beta_search(Board* board, int depth, int alpha, int beta, bool maximizingPlayer, 
                      SearchInfo* info, int ply) {
    (void)maximizingPlayer;
    return negamax(board, depth, alpha, beta, info, ply, true, false);
}

int quiescence_search(Board* board, int alpha, int beta, bool maximizingPlayer,
                      SearchInfo* info, int ply) {
    (void)maximizingPlayer;
    info->nodesSearched++;  // quiescence() expects the caller to count its root
    return quiescence(board, alpha, beta, info, ply);
}
