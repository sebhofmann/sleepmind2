#include "training_data.h"
#include "board_io.h"
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>  // For pid_t
#include <unistd.h>     // For getpid()

// Global variables
TrainingEntry training_data[MAX_TRAINING_ENTRIES];
int training_data_count = 0;
FILE* training_file = NULL;
char training_data_path[256] = "";
bool training_enabled = false;

void init_training_data() {
    training_data_count = 0;
    if (training_file) {
        fclose(training_file);
        training_file = NULL;
    }
    training_enabled = false;
    strcpy(training_data_path, "");
}

void add_training_entry(const Board* board, int eval, int ply) {
    if (!training_enabled || training_data_count >= MAX_TRAINING_ENTRIES) return;
    const char* fen = outputFEN(board);
    strncpy(training_data[training_data_count].fen, fen, sizeof(training_data[training_data_count].fen) - 1);
    training_data[training_data_count].eval = eval;
    training_data[training_data_count].ply = ply;
    training_data[training_data_count].white_to_move = board->whiteToMove;
    training_data_count++;
}

void write_training_data(int result) {  // 1 = win for white, 0 = draw, -1 = loss for white
    if (!training_enabled || !training_data_path[0]) return;
    if (!training_file) {
        training_file = fopen(training_data_path, "a");
        if (!training_file) return;
    }
    for (int i = 0; i < training_data_count; i++) {
        // Score is white-relative: positive = good for white
        // The stored eval is STM-relative, so flip if black to move
        int white_relative_eval = training_data[i].white_to_move 
            ? training_data[i].eval 
            : -training_data[i].eval;
        
        // WDL is white-relative: 1.0 = white wins, 0.5 = draw, 0.0 = white loses
        const char* wdl;
        if (result == 1) wdl = "1.0";       // White won
        else if (result == -1) wdl = "0.0"; // White lost
        else wdl = "0.5";                   // Draw
        fprintf(training_file, "%s | %d | %s\n", training_data[i].fen, white_relative_eval, wdl);
    }
    fflush(training_file);
    training_data_count = 0;  // Reset
    // Close file after writing
    if (training_file) {
        fclose(training_file);
        training_file = NULL;
    }
}

void set_training_data_path(const char* path) {
    if (path && strlen(path) > 0) {
        // Append PID to filename for unique files in parallel runs
        pid_t pid = getpid();
        snprintf(training_data_path, sizeof(training_data_path), "%s.%d", path, pid);
        training_enabled = true;
        if (training_file) {
            fclose(training_file);
        }
        training_file = fopen(training_data_path, "a");
    } else {
        training_enabled = false;
        if (training_file) {
            fclose(training_file);
            training_file = NULL;
        }
        strcpy(training_data_path, "");
    }
}

void enable_training(bool enable) {
    training_enabled = enable;
}