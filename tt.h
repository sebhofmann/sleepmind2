#ifndef TT_H
#define TT_H

#include "board.h" // For Bitboard (used in Zobrist hashing) and Move
#include "move.h"  // For Move type
#include <stdint.h>
#include <stdbool.h>
#include "zobrist.h"

// Transposition Table Entry Flags
#define TT_EXACT 0
#define TT_LOWERBOUND 1 // Alpha (score is at least this good)
#define TT_UPPERBOUND 2 // Beta (score is at most this good)

typedef struct {
    uint64_t zobristKey; // Store the full key to detect collisions
    Move bestMove;
    int score;
    int depth;
    uint8_t flag; // EXACT, LOWERBOUND, UPPERBOUND
    // uint8_t age; // For replacement strategy, if needed
} TTEntry;

void init_zobrist_keys();
void init_tt(size_t table_size_mb);
void clear_tt();
void tt_store(uint64_t key, int depth, int score, uint8_t flag, Move best_move);
TTEntry* tt_probe(uint64_t key, int depth, int* alpha, int* beta); // Probes and may adjust alpha/beta
void free_tt();


#endif // TT_H
