#ifndef TRAINING_DATA_H
#define TRAINING_DATA_H

#include "board.h"
#include <stdio.h>
#include <stdbool.h>

// Training data collection
#define MAX_TRAINING_ENTRIES 10000
typedef struct {
    char fen[100];
    int eval;
    int ply;
    bool white_to_move;
} TrainingEntry;

// Global variables (declared in training_data.c)
extern TrainingEntry training_data[MAX_TRAINING_ENTRIES];
extern int training_data_count;
extern FILE* training_file;
extern char training_data_path[256];
extern bool training_enabled;

// Functions
void init_training_data();
void add_training_entry(const Board* board, int eval, int ply);
void write_training_data(int result);
void flush_training_data();
void set_training_data_path(const char* path);
void enable_training(bool enable);

#endif // TRAINING_DATA_H