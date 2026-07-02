#include "syzygy.h"
#include "tbprobe.h"
#include "move_generator.h"
#include "board_modifiers.h"
#include "bitboard_utils.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

// Tablebases are loaded iff this is true (and TB_LARGEST > 0).
static bool tb_loaded = false;

// -----------------------------------------------------------------------------
// Board -> Fathom bitboard conversion
//
// NOTE: byColorBB is NOT maintained incrementally during search, so white/black
// (and the occupancy) are rebuilt from byTypeBB here, never read from byColorBB.
// -----------------------------------------------------------------------------
static void board_to_fathom(const Board* b,
    uint64_t* white, uint64_t* black,
    uint64_t* kings, uint64_t* queens, uint64_t* rooks,
    uint64_t* bishops, uint64_t* knights, uint64_t* pawns,
    unsigned* castling, unsigned* ep) {
    *kings   = b->byTypeBB[WHITE][KING]   | b->byTypeBB[BLACK][KING];
    *queens  = b->byTypeBB[WHITE][QUEEN]  | b->byTypeBB[BLACK][QUEEN];
    *rooks   = b->byTypeBB[WHITE][ROOK]   | b->byTypeBB[BLACK][ROOK];
    *bishops = b->byTypeBB[WHITE][BISHOP] | b->byTypeBB[BLACK][BISHOP];
    *knights = b->byTypeBB[WHITE][KNIGHT] | b->byTypeBB[BLACK][KNIGHT];
    *pawns   = b->byTypeBB[WHITE][PAWN]   | b->byTypeBB[BLACK][PAWN];

    *white = b->byTypeBB[WHITE][PAWN]   | b->byTypeBB[WHITE][KNIGHT] |
             b->byTypeBB[WHITE][BISHOP] | b->byTypeBB[WHITE][ROOK]   |
             b->byTypeBB[WHITE][QUEEN]  | b->byTypeBB[WHITE][KING];
    *black = b->byTypeBB[BLACK][PAWN]   | b->byTypeBB[BLACK][KNIGHT] |
             b->byTypeBB[BLACK][BISHOP] | b->byTypeBB[BLACK][ROOK]   |
             b->byTypeBB[BLACK][QUEEN]  | b->byTypeBB[BLACK][KING];

    // We only ever probe when castling rights are gone; pass 0 unconditionally.
    *castling = 0;
    *ep = (b->enPassantSquare == SQ_NONE || b->enPassantSquare < 0)
              ? 0u : (unsigned)b->enPassantSquare;
}

// Map a Fathom TB_PROMOTES_* value to the engine's PROMOTION_* encoding.
static int fathom_promo_to_engine(unsigned tb_promo) {
    switch (tb_promo) {
        case TB_PROMOTES_QUEEN:  return PROMOTION_Q;
        case TB_PROMOTES_ROOK:   return PROMOTION_R;
        case TB_PROMOTES_BISHOP: return PROMOTION_B;
        case TB_PROMOTES_KNIGHT: return PROMOTION_N;
        default:                 return 0; // TB_PROMOTES_NONE
    }
}

void syzygy_init(const char* path) {
    syzygy_free();

    if (path == NULL || path[0] == '\0' || strcmp(path, "<empty>") == 0) {
        tb_loaded = false;
        return;
    }

    bool ok = tb_init(path);
    tb_loaded = ok && TB_LARGEST > 0;

    if (tb_loaded) {
        printf("info string Syzygy: loaded, max %u pieces from %s\n",
               TB_LARGEST, path);
    } else {
        printf("info string Syzygy: no tablebases found at %s\n", path);
    }
    fflush(stdout);
}

void syzygy_free(void) {
    if (tb_loaded) {
        tb_free();
        tb_loaded = false;
    }
}

int syzygy_max_pieces(void) {
    return tb_loaded ? (int)TB_LARGEST : 0;
}

bool syzygy_available(int piece_count) {
    return tb_loaded && piece_count >= 0 && piece_count <= (int)TB_LARGEST;
}

bool syzygy_probe_wdl(const Board* board, int* wdl) {
    if (!tb_loaded) return false;

    uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    unsigned castling, ep;
    board_to_fathom(board, &white, &black, &kings, &queens, &rooks,
                    &bishops, &knights, &pawns, &castling, &ep);

    // Fathom requires exactly one king per side; refuse anything else so a
    // malformed position can never reach a lsb(0) assertion inside the prober.
    if (POPCOUNT(kings) != 2) return false;

    unsigned res = tb_probe_wdl(white, black, kings, queens, rooks, bishops,
                                knights, pawns,
                                (unsigned)board->halfMoveClock, castling, ep,
                                board->whiteToMove);
    if (res == TB_RESULT_FAILED) return false;

    switch (res) {
        case TB_WIN:  *wdl =  1; break;
        case TB_LOSS: *wdl = -1; break;
        default:      *wdl =  0; break; // draw / cursed win / blessed loss
    }
    return true;
}

int syzygy_probe_play(const Board* b, Move* move, int* wdl, int* dtz) {
    if (!tb_loaded) return -2;

    uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    unsigned castling, ep;
    board_to_fathom(b, &white, &black, &kings, &queens, &rooks,
                    &bishops, &knights, &pawns, &castling, &ep);

    if (POPCOUNT(kings) != 2) return -2;

    unsigned res = tb_probe_root(white, black, kings, queens, rooks, bishops,
                                 knights, pawns,
                                 (unsigned)b->halfMoveClock, castling, ep,
                                 b->whiteToMove, NULL);
    if (res == TB_RESULT_FAILED)    return -2;
    if (res == TB_RESULT_CHECKMATE) return 0;
    if (res == TB_RESULT_STALEMATE) return -1;

    unsigned w = TB_GET_WDL(res);
    *wdl = (w == TB_WIN) ? 1 : (w == TB_LOSS) ? -1 : 0;
    *dtz = (int)TB_GET_DTZ(res);

    int from  = TB_GET_FROM(res);
    int to    = TB_GET_TO(res);
    int promo = fathom_promo_to_engine(TB_GET_PROMOTES(res));

    MoveList legal;
    generateMoves(b, &legal);
    for (int j = 0; j < legal.count; j++) {
        Move m = legal.moves[j];
        if (MOVE_FROM(m) == from && MOVE_TO(m) == to &&
            (int)MOVE_PROMOTION(m) == promo) {
            *move = m;
            return 1;
        }
    }
    return -2;
}

// Walk the DTZ-optimal line from the root out to mate (or until a draw / cap),
// filling out->pv / out->pvLen and out->matePlies. Operates on a scratch copy
// of the board; NNUE is not needed, so applyMove is called with NULL networks.
static void syzygy_walk_pv(const Board* root, SyzygyRootResult* out) {
    Board b = *root;
    out->pvLen = 0;
    out->matePlies = -1;

    for (int ply = 0; ply < SYZYGY_MAX_PV; ply++) {
        Move mv = 0;
        int wdl = 0, dtz = 0;
        int status = syzygy_probe_play(&b, &mv, &wdl, &dtz);
        if (status == 0) {            // side to move is checkmated -> mate reached
            out->matePlies = out->pvLen;
            return;
        }
        if (status != 1) return;      // stalemate / draw-terminal / failure

        out->pv[out->pvLen++] = mv;

        MoveUndoInfo undo;
        applyMove(&b, mv, &undo, NULL, NULL);

        if (wdl == 0) return;         // drawn line: stop, no forced mate
    }
}

bool syzygy_probe_root(const Board* board, SyzygyRootResult* out) {
    if (!tb_loaded) return false;

    uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    unsigned castling, ep;
    board_to_fathom(board, &white, &black, &kings, &queens, &rooks,
                    &bishops, &knights, &pawns, &castling, &ep);

    if (POPCOUNT(kings) != 2) return false;

    // tb_probe_root applies DTZ-optimal filtering itself: for a win it selects
    // the move(s) with the SMALLEST DTZ (fastest conversion), for a loss the
    // largest DTZ (longest defence). The per-move results array lets us collect
    // all moves that achieve that optimal DTZ. Restricting the search to those
    // guarantees the DTZ strictly decreases each move, so the win is actually
    // converted instead of shuffled (tb_probe_root_dtz ranks all "safe" wins
    // equally, which is why a non-progressing move like Rh4 could be picked).
    unsigned results[TB_MAX_MOVES];
    unsigned res = tb_probe_root(white, black, kings, queens, rooks, bishops,
                                 knights, pawns,
                                 (unsigned)board->halfMoveClock, castling, ep,
                                 board->whiteToMove, results);
    if (res == TB_RESULT_FAILED) return false;

    unsigned best_wdl = TB_GET_WDL(res);
    unsigned best_dtz = TB_GET_DTZ(res);
    out->wdl = (best_wdl == TB_WIN) ? 1 : (best_wdl == TB_LOSS) ? -1 : 0;

    // Match each optimal Fathom move against the engine's pseudo-legal move list
    // so the returned moves carry the correct capture/ep/castle flag bits.
    MoveList legal;
    generateMoves(board, &legal);

    out->count = 0;
    for (int i = 0; results[i] != TB_RESULT_FAILED && out->count < MAX_MOVES; i++) {
        unsigned r = results[i];
        // Keep only moves preserving the best WDL AND achieving the optimal DTZ.
        if (TB_GET_WDL(r) != best_wdl || TB_GET_DTZ(r) != best_dtz) continue;

        int from  = TB_GET_FROM(r);
        int to    = TB_GET_TO(r);
        int promo = fathom_promo_to_engine(TB_GET_PROMOTES(r));

        for (int j = 0; j < legal.count; j++) {
            Move m = legal.moves[j];
            if (MOVE_FROM(m) == from && MOVE_TO(m) == to &&
                (int)MOVE_PROMOTION(m) == promo) {
                out->moves[out->count++] = m;
                break;
            }
        }
    }

    if (out->count == 0) return false;

    // Build the DTZ-optimal principal variation (and hence the true mate
    // distance) by walking the tablebase out to mate from the root.
    syzygy_walk_pv(board, out);
    return true;
}
