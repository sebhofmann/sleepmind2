// Regression test: the repetition history must hold a full-length datagen game
// (MAX_GAME_RECORD_PLIES) plus the search stack on top. A too-small buffer let
// applyMove write past board->history into board->historyIndex, which made
// is_draw() scan wild memory and crash.
#include "board_io.h"
#include "board_modifiers.h"
#include "search.h"
#include "training_data.h"
#include "zobrist.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    init_zobrist_keys();

    Board board = parseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const Move cycle[4] = {
        CREATE_MOVE(SQ_G1, SQ_F3, 0, 0, 0, 0, 0),
        CREATE_MOVE(SQ_G8, SQ_F6, 0, 0, 0, 0, 0),
        CREATE_MOVE(SQ_F3, SQ_G1, 0, 0, 0, 0, 0),
        CREATE_MOVE(SQ_F6, SQ_G8, 0, 0, 0, 0, 0),
    };

    const int plies = MAX_GAME_RECORD_PLIES + MAX_PLY;
    for (int i = 0; i < plies; i++) {
        MoveUndoInfo undo;
        applyMove(&board, cycle[i % 4], &undo, NULL, NULL);
        assert(board.historyIndex == i + 1);
        assert(board.history[i] == board.zobristKey);
    }
    assert((size_t)plies <= sizeof(board.history) / sizeof(board.history[0]));

    printf("long_game_history_test: OK (%d plies)\n", plies);
    return 0;
}
