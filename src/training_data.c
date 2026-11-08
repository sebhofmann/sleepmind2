#include "training_data.h"
#include "board_io.h"
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>  // For pid_t
#include <unistd.h>     // For getpid()

static char record_fen[128];
static Move record_moves[MAX_GAME_RECORD_PLIES];
static int record_scores[MAX_GAME_RECORD_PLIES];
static int record_count = 0;
static bool record_active = false;

static FILE* training_file = NULL;
static char training_data_path[512] = "";

void game_record_begin(const Board* board) {
    const char* fen = outputFEN(board);
    strncpy(record_fen, fen, sizeof(record_fen) - 1);
    record_fen[sizeof(record_fen) - 1] = '\0';
    record_count = 0;
    record_active = true;
}

bool game_record_add(Move move, int white_score) {
    if (!record_active || record_count >= MAX_GAME_RECORD_PLIES) return false;
    record_moves[record_count] = move;
    record_scores[record_count] = white_score;
    record_count++;
    return true;
}

int game_record_length(void) {
    return record_active ? record_count : 0;
}

void game_record_discard(void) {
    record_count = 0;
    record_active = false;
}

void game_record_write(int result) {  // 1 = White won, 0 = draw, -1 = Black won
    if (!record_active) return;
    if (record_count > 0 && training_file) {
        const char* res = result > 0 ? "1.0" : result < 0 ? "0.0" : "0.5";
        fprintf(training_file, "%s |", record_fen);
        char mv[8];
        for (int i = 0; i < record_count; i++) {
            moveToString(record_moves[i], mv);
            fprintf(training_file, " %s:%d", mv, record_scores[i]);
        }
        fprintf(training_file, " | %s\n", res);
        // Flush after each game so a killed instance loses at most one game.
        fflush(training_file);
    }
    game_record_discard();
}

void set_training_data_path(const char* path) {
    close_training_data();
    if (!path || !path[0]) return;
    // Append PID to filename for unique files in parallel runs
    snprintf(training_data_path, sizeof(training_data_path), "%s.%lld",
             path, (long long)getpid());
    training_file = fopen(training_data_path, "a");
    if (training_file) {
        setvbuf(training_file, NULL, _IOFBF, 1 << 16);
    } else {
        fprintf(stderr, "Error: cannot open training data file %s\n", training_data_path);
    }
}

const char* get_training_data_path(void) {
    return training_data_path;
}

void close_training_data(void) {
    if (training_file) {
        fclose(training_file);
        training_file = NULL;
    }
}
