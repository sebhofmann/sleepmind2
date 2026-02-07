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

// Packed TT Entry - 16 bytes for optimal cache line usage
// Key verification uses upper 16 bits stored separately
typedef struct {
    uint32_t key16;      // Upper 16 bits of zobrist key for verification (4 bytes due to alignment)
    Move bestMove;       // 4 bytes
    int16_t score;       // 2 bytes
    int8_t depth;        // 1 byte
    uint8_t flag_age;    // 2 bits flag, 6 bits age (1 byte)
} TTEntry;

// Helper macros for flag_age field
#define TT_GET_FLAG(e) ((e)->flag_age & 0x03)
#define TT_GET_AGE(e) ((e)->flag_age >> 2)
#define TT_MAKE_FLAG_AGE(flag, age) (((age) << 2) | ((flag) & 0x03))

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
