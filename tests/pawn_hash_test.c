#include "board_io.h"
#include "board_modifiers.h"
#include "zobrist.h"
#include <assert.h>
#include <stdio.h>

static void check_non_pawn_keys(const Board* board) {
    assert(board->nonPawnKey[WHITE] == calculate_non_pawn_key(board, WHITE));
    assert(board->nonPawnKey[BLACK] == calculate_non_pawn_key(board, BLACK));
}

static void check_move(Board* board, Move move) {
    uint64_t old = board->pawnKey;
    uint64_t old_white = board->nonPawnKey[WHITE];
    uint64_t old_black = board->nonPawnKey[BLACK];
    MoveUndoInfo undo;
    applyMove(board, move, &undo, NULL, NULL);
    assert(board->pawnKey == calculate_pawn_key(board));
    check_non_pawn_keys(board);
    undoMove(board, move, &undo, NULL, NULL);
    assert(board->pawnKey == old);
    assert(board->pawnKey == calculate_pawn_key(board));
    assert(board->nonPawnKey[WHITE] == old_white);
    assert(board->nonPawnKey[BLACK] == old_black);
    check_non_pawn_keys(board);
}

int main(void) {
    init_zobrist_keys();

    Board start = parseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    check_move(&start, CREATE_MOVE(SQ_E2, SQ_E4, 0, 0, 1, 0, 0));
    check_move(&start, CREATE_MOVE(SQ_G1, SQ_F3, 0, 0, 0, 0, 0));

    Board capture = parseFEN("4k3/8/3p4/4P3/8/8/8/4K3 w - - 0 1");
    check_move(&capture, CREATE_MOVE(SQ_E5, SQ_D6, 0, 1, 0, 0, 0));

    Board piece_capture = parseFEN("4k3/8/3n4/4P3/8/8/8/4K3 w - - 0 1");
    check_move(&piece_capture, CREATE_MOVE(SQ_E5, SQ_D6, 0, 1, 0, 0, 0));

    Board ep = parseFEN("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    check_move(&ep, CREATE_MOVE(SQ_E5, SQ_D6, 0, 1, 0, 1, 0));

    Board promotion = parseFEN("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    check_move(&promotion, CREATE_MOVE(SQ_A7, SQ_A8, PROMOTION_Q, 0, 0, 0, 0));

    Board capture_promotion = parseFEN("1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    check_move(&capture_promotion,
               CREATE_MOVE(SQ_A7, SQ_B8, PROMOTION_Q, 1, 0, 0, 0));

    Board castle = parseFEN("4k3/8/8/8/8/8/8/4K2R w K - 0 1");
    check_move(&castle, CREATE_MOVE(SQ_E1, SQ_G1, 0, 0, 0, 0, 1));

    Board black_castle = parseFEN("r3k3/8/8/8/8/8/8/4K3 b q - 0 1");
    check_move(&black_castle, CREATE_MOVE(SQ_E8, SQ_C8, 0, 0, 0, 0, 1));

    Board black_capture_promotion = parseFEN("4k3/8/8/8/8/8/p7/1R2K3 b - - 0 1");
    check_move(&black_capture_promotion,
               CREATE_MOVE(SQ_A2, SQ_B1, PROMOTION_N, 1, 0, 0, 0));

    Board piece_takes_piece = parseFEN("4k3/8/5n2/8/3B4/8/8/4K3 w - - 0 1");
    check_move(&piece_takes_piece, CREATE_MOVE(SQ_D4, SQ_F6, 0, 1, 0, 0, 0));
    assert(piece_takes_piece.nonPawnKey[WHITE] !=
           parseFEN("4k3/8/5n2/8/8/8/8/4K3 w - - 0 1").nonPawnKey[WHITE]);

    uint64_t pawn_key = start.pawnKey;
    start.whiteToMove = !start.whiteToMove;
    start.zobristKey ^= zobrist_side_to_move_key;
    assert(start.pawnKey == pawn_key);
    assert(start.pawnKey == calculate_pawn_key(&start));
    start.whiteToMove = !start.whiteToMove;
    start.zobristKey ^= zobrist_side_to_move_key;
    assert(start.pawnKey == pawn_key);

    puts("pawn hash make/undo/null tests passed");
    return 0;
}
