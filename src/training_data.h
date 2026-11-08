#ifndef TRAINING_DATA_H
#define TRAINING_DATA_H

#include "board.h"
#include "move.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Game records for the viriformat pipeline
// =============================================================================
//
// The generator writes whole games, one game per line, in a simple text
// intermediate format that `training/datatools` (viri_pack) converts into a
// viriformat binpack via the viriformat crate:
//
//   <start FEN> | <uci>:<score> <uci>:<score> ... | <result>
//
// - start FEN: the position after the random opening phase (first recorded
//   position), including halfmove clock and fullmove number.
// - score:     search score of the position BEFORE the move, White's point of
//              view (viriformat convention), in centipawns. Mate/TB scores are
//              mapped to +/-TRAINING_MATE_SCORE.
// - result:    1.0 = White won, 0.5 = draw, 0.0 = Black won.
//
// One line per game keeps files append-safe and line-oriented (wc -l counts
// games; a truncated last line is rejected by the packer).

#define MAX_GAME_RECORD_PLIES 4096

// Mate and tablebase scores do not fit into the i16 eval of viriformat; they
// are stored as this fixed magnitude, which the loader's max_eval filter drops.
#define TRAINING_MATE_SCORE 32000
#define TRAINING_MAX_NORMAL_SCORE 31999

// Start a new game record at the given position.
void game_record_begin(const Board* board);
// Append a move and the White-relative score of the position it was played in.
// Returns false if the record is full.
bool game_record_add(Move move, int white_score);
// Number of moves (= positions) in the current record.
int game_record_length(void);
// Write the current record with the given result (1 = White won, 0 = draw,
// -1 = Black won) and flush. Empty records are not written.
void game_record_write(int result);
// Drop the current record without writing it.
void game_record_discard(void);

void set_training_data_path(const char* path);
const char* get_training_data_path(void);
void close_training_data(void);

#endif // TRAINING_DATA_H
