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
    int16_t score;
    int8_t depth;
    uint8_t flag;    // EXACT, LOWERBOUND, UPPERBOUND
    uint8_t age;     // For replacement strategy
} __attribute__((packed)) TTEntry;

// Current search age for replacement strategy
extern uint8_t tt_age;

void init_zobrist_keys();
void init_tt(size_t table_size_mb);
void clear_tt();
void tt_new_search();  // Call at start of each search to increment age
void tt_store(uint64_t key, int depth, int score, uint8_t flag, Move best_move);
TTEntry* tt_probe(uint64_t key);
void tt_prefetch(uint64_t key);  // Prefetch TT entry for better cache performance
void free_tt();
int tt_hashfull();  // Returns permille of TT usage


#endif // TT_H
