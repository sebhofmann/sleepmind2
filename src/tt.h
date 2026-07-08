#ifndef TT_H
#define TT_H

#include "board.h" // For Bitboard (used in Zobrist hashing) and Move
#include "move.h"  // For Move type
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "zobrist.h"

// Bound types (NONE=0 marks unused entries, EXACT=UPPER|LOWER)
#define TT_NONE       0
#define TT_UPPERBOUND 1 // Score is at most this (failed low)
#define TT_LOWERBOUND 2 // Score is at least this (failed high)
#define TT_EXACT      3

// Sentinel for "no static eval stored" (e.g. in-check nodes, TB stores)
#define TT_EVAL_NONE  INT16_MIN

// Decoded copy of a matching TT entry, returned by tt_probe.
// When found is false the remaining fields are zero.
typedef struct {
    bool    found;
    bool    is_pv;   // Position was ever searched as a PV node
    uint8_t bound;   // TT_EXACT / TT_LOWERBOUND / TT_UPPERBOUND
    int     depth;
    int     score;
    int     eval;    // Static eval (side to move), TT_EVAL_NONE if unknown
    Move    move;
} TTData;

void init_zobrist_keys();
void init_tt(size_t table_size_mb);
void clear_tt();
void tt_new_search();  // Call at start of each search to bump the generation
void tt_store(uint64_t key, int depth, int score, uint8_t bound, Move best_move, bool is_pv, int eval);
TTData tt_probe(uint64_t key);
void tt_prefetch(uint64_t key);  // Prefetch TT cluster for better cache performance
void free_tt();
int tt_hashfull();  // Returns permille of TT usage

#endif // TT_H
