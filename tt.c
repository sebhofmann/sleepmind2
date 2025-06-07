#include "tt.h"
#include "bitboard_utils.h" // For GET_BIT, potentially
#include <stdlib.h>
#include <string.h> // For memset
#include <stdio.h> // For printf debugging



// --- Transposition Table ---
static TTEntry* transposition_table = NULL;
static uint64_t tt_size_entries = 0; // Number of entries, not bytes
// static const int TT_ENTRY_SIZE = sizeof(TTEntry); // Not used directly for allocation anymore

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
    if (tt_size_entries == 0 && table_size_mb > 0) { // Ensure at least some entries if MB > 0
        tt_size_entries = 1; // Avoid division by zero if table_size_mb is tiny but non-zero
    }
     if (tt_size_entries > 0) {
        transposition_table = (TTEntry*)malloc(tt_size_entries * sizeof(TTEntry));
        if (transposition_table == NULL) {
            fprintf(stderr, "Failed to allocate transposition table!\n");
            tt_size_entries = 0;
            return;
        }
        clear_tt(); // Initialize entries
        printf("info string TT initialized with %llu entries (%.2f MB)\n", tt_size_entries, (double)(tt_size_entries * sizeof(TTEntry)) / (1024*1024));
    } else {
         printf("info string TT not allocated (tt_size_entries is 0)\n");
    }
}

void clear_tt() {
    if (transposition_table != NULL && tt_size_entries > 0) {
        // Setting only zobristKey to 0 is often enough to mark as empty/invalid
        // For a full clear, use memset:
        memset(transposition_table, 0, tt_size_entries * sizeof(TTEntry));
    }
}

void tt_store(uint64_t key, int depth, int score, uint8_t flag, Move best_move) {
    if (transposition_table == NULL || tt_size_entries == 0) return;

    uint64_t index = key % tt_size_entries;
    TTEntry* entry = &transposition_table[index];

    // Simple "always replace" strategy.
    // Could be improved with depth preference or other replacement schemes.
    // if (entry->zobristKey == 0 || depth >= entry->depth - 2 ) { // Prioritize deeper searches or new entries
        entry->zobristKey = key;
        entry->depth = depth;
        entry->score = score;
        entry->flag = flag;
        entry->bestMove = best_move;
        // entry->age = current_search_age; // If using age for replacement
    // }
}

TTEntry* tt_probe(uint64_t key, int depth_searched, int* alpha, int* beta) {
    if (transposition_table == NULL || tt_size_entries == 0) return NULL;

    uint64_t index = key % tt_size_entries;
    TTEntry* entry = &transposition_table[index];

    if (entry->zobristKey == key) { // Check for collision
        // Entry found
        if (entry->depth >= depth_searched) {
            // Adjust score for mate distance from root if mate score
            int score = entry->score;
            // if (score > MATE_THRESHOLD) score -= ply; // If mate found deeper, it takes longer
            // else if (score < -MATE_THRESHOLD) score += ply;

            if (entry->flag == TT_EXACT) {
                return entry; // Return the entry directly, its score is exact
            }
            if (entry->flag == TT_LOWERBOUND && score >= *beta) {
                 // *alpha = (*alpha > score) ? *alpha : score; // max(*alpha, score)
                return entry; // Score is a lower bound, and it's already causing a beta cutoff
            }
            if (entry->flag == TT_UPPERBOUND && score <= *alpha) {
                // *beta = (*beta < score) ? *beta : score;   // min(*beta, score)
                return entry; // Score is an upper bound, and it's already causing an alpha cutoff
            }
        }
        // Can still use the bestMove from a shallower search for move ordering
        return entry; // Return entry even if depth is not sufficient, bestMove might be useful
    }
    return NULL; // Entry not found or collision
}


void free_tt() {
    if (transposition_table != NULL) {
        free(transposition_table);
        transposition_table = NULL;
        tt_size_entries = 0;
    }
}

