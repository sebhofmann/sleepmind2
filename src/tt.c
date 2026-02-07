#include "tt.h"
#include "bitboard_utils.h" // For GET_BIT, potentially
#include <stdlib.h>
#include <string.h> // For memset
#include <stdio.h> // For printf debugging

// --- Transposition Table ---
static TTEntry* transposition_table = NULL;
static uint64_t tt_size_entries = 0; // Number of entries, not bytes
uint8_t tt_age = 0;  // Current search age

void init_tt(size_t table_size_mb) {
    if (transposition_table != NULL) {
        free_tt(); // Free existing table if any
    }
    if (table_size_mb == 0) {
        tt_size_entries = 0;
        transposition_table = NULL;
        printf("info string TT disabled (0 MB)\n");
        return;
    }
    tt_size_entries = (table_size_mb * 1024 * 1024) / sizeof(TTEntry);
    if (tt_size_entries == 0 && table_size_mb > 0) {
        tt_size_entries = 1;
    }
    if (tt_size_entries > 0) {
        transposition_table = (TTEntry*)malloc(tt_size_entries * sizeof(TTEntry));
        if (transposition_table == NULL) {
            fprintf(stderr, "Failed to allocate transposition table!\n");
            tt_size_entries = 0;
            return;
        }
        clear_tt();
        printf("info string TT initialized with %llu entries (%.2f MB)\n", 
               (unsigned long long)tt_size_entries, 
               (double)(tt_size_entries * sizeof(TTEntry)) / (1024*1024));
    } else {
        printf("info string TT not allocated (tt_size_entries is 0)\n");
    }
}

void clear_tt() {
    if (transposition_table != NULL && tt_size_entries > 0) {
        memset(transposition_table, 0, tt_size_entries * sizeof(TTEntry));
    }
    tt_age = 0;
}

void tt_new_search() {
    tt_age++;
    // Handle wraparound (63 is max for 6-bit age comparison)
    if (tt_age > 63) {
        tt_age = 0;
    }
}

void tt_store(uint64_t key, int depth, int score, uint8_t flag, Move best_move) {
    if (transposition_table == NULL || tt_size_entries == 0) return;

    uint64_t index = key % tt_size_entries;
    TTEntry* entry = &transposition_table[index];
    uint32_t key16 = (uint32_t)(key >> 48);  // Upper 16 bits for verification

    // Replacement strategy: 
    // 1. Always replace if empty (key16 == 0 and bestMove == 0)
    // 2. Always replace if same position (update with new info)
    // 3. Replace if new depth is >= old depth - 3 (depth preference with some tolerance)
    // 4. Replace if entry is from an old search (age difference >= 2)
    // 5. Always replace UPPERBOUND entries with other types (failed lows are less valuable)
    
    bool should_replace = false;
    uint8_t entry_age = TT_GET_AGE(entry);
    uint8_t entry_flag = TT_GET_FLAG(entry);
    
    if (entry->key16 == 0 && entry->bestMove == 0) {
        // Empty entry
        should_replace = true;
    } else if (entry->key16 == key16) {
        // Same position - update if deeper or same depth with better flag
        should_replace = (depth >= entry->depth) || 
                        (depth == entry->depth - 1 && flag == TT_EXACT && entry_flag != TT_EXACT);
    } else {
        // Different position - use age and depth for replacement
        int age_diff = (tt_age - entry_age) & 63;  // Handle wraparound
        
        if (age_diff >= 2) {
            // Old entry, replace it
            should_replace = true;
        } else if (depth >= entry->depth - 2) {
            // New search is deep enough
            should_replace = true;
        } else if (entry_flag == TT_UPPERBOUND && flag != TT_UPPERBOUND) {
            // Replace failed low with better info
            should_replace = true;
        }
    }
    
    if (should_replace) {
        entry->key16 = key16;
        entry->depth = (int8_t)depth;
        entry->score = (int16_t)score;
        entry->flag_age = TT_MAKE_FLAG_AGE(flag, tt_age);
        entry->bestMove = best_move;
    }
}

TTEntry* tt_probe(uint64_t key) {
    if (transposition_table == NULL || tt_size_entries == 0) return NULL;

    uint64_t index = key % tt_size_entries;
    TTEntry* entry = &transposition_table[index];
    uint32_t key16 = (uint32_t)(key >> 48);

    if (entry->key16 == key16) {
        return entry;
    }
    return NULL;
}

void tt_prefetch(uint64_t key) {
    if (transposition_table == NULL || tt_size_entries == 0) return;
    
    uint64_t index = key % tt_size_entries;
    __builtin_prefetch(&transposition_table[index], 0, 1);
}

int tt_hashfull() {
    if (transposition_table == NULL || tt_size_entries == 0) return 0;
    
    int used = 0;
    // Sample 1000 entries distributed across the table for accurate measurement
    uint64_t sample_size = 1000;
    uint64_t step = tt_size_entries / sample_size;
    if (step == 0) step = 1;
    
    for (uint64_t i = 0; i < sample_size && i * step < tt_size_entries; i++) {
        uint64_t idx = i * step;
        if ((transposition_table[idx].key16 != 0 || transposition_table[idx].bestMove != 0) && 
            TT_GET_AGE(&transposition_table[idx]) == tt_age) {
            used++;
        }
    }
    
    return used;  // Already in permille since sample_size is 1000
}

void free_tt() {
    if (transposition_table != NULL) {
        free(transposition_table);
        transposition_table = NULL;
        tt_size_entries = 0;
    }
}

