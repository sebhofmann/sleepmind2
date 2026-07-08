#include "tt.h"
#include <stdlib.h>
#include <string.h> // For memset
#include <stdio.h>  // For info string output

// =============================================================================
// Bucketed transposition table with generation-based aging
//
// The table is an array of 64-byte clusters holding 5 entries each, so a
// cluster fills exactly one cache line. A probe scans the whole cluster for a
// matching key; a store picks a victim by depth discounted by age, so deep
// entries from the current search survive while stale ones get recycled.
// Indexing uses the high 64 bits of key * cluster_count, which allows
// arbitrary (non power-of-two) table sizes; the low 16 key bits verify hits.
// =============================================================================

// Depth is stored in a uint8 with an offset so that qsearch depths (<= 0)
// still map to a non-zero value; depth8 == 0 marks an unused entry.
#define TT_DEPTH_OFFSET 8

// genBound8 layout: [gggggpbb] - 5 bits generation, 1 bit PV, 2 bits bound
#define TT_GENERATION_BITS  3   // bits reserved for PV flag + bound
#define TT_GENERATION_DELTA (1 << TT_GENERATION_BITS)            // 8
#define TT_GENERATION_CYCLE (255 + TT_GENERATION_DELTA)          // 263
#define TT_GENERATION_MASK  (0xFF & ~(TT_GENERATION_DELTA - 1))  // 0xF8

typedef struct {
    uint16_t key16;      // low 16 bits of the zobrist key
    uint16_t move_lo;    // Move split into two uint16 to keep the entry packed
    uint16_t move_hi;
    int16_t  score16;
    int16_t  eval16;     // static eval (side to move), TT_EVAL_NONE if unknown
    uint8_t  depth8;     // depth + TT_DEPTH_OFFSET, 0 = empty
    uint8_t  genBound8;  // generation | is_pv << 2 | bound
} TTEntry;

#define TT_CLUSTER_SIZE 5

typedef struct {
    TTEntry entry[TT_CLUSTER_SIZE];
    uint8_t padding[4];
} TTCluster;

_Static_assert(sizeof(TTEntry) == 12, "TTEntry must be 12 bytes");
_Static_assert(sizeof(TTCluster) == 64, "TTCluster must be 64 bytes");

static TTCluster* table = NULL;
static uint64_t cluster_count = 0;
static uint8_t generation8 = 0;  // current generation, pre-shifted by TT_GENERATION_BITS

// Age of an entry relative to the current search, in multiples of
// TT_GENERATION_DELTA. The cycle constant keeps the subtraction correct
// across uint8 wraparound.
static inline int relative_age(uint8_t gen_bound) {
    return (TT_GENERATION_CYCLE + generation8 - gen_bound) & TT_GENERATION_MASK;
}

static inline TTCluster* cluster_for(uint64_t key) {
    return &table[(uint64_t)(((unsigned __int128)key * cluster_count) >> 64)];
}

static inline Move entry_move(const TTEntry* e) {
    return ((Move)e->move_hi << 16) | e->move_lo;
}

void init_tt(size_t table_size_mb) {
    if (table != NULL) {
        free_tt();
    }
    if (table_size_mb == 0) {
        printf("info string TT disabled (0 MB)\n");
        return;
    }
    cluster_count = (table_size_mb * 1024 * 1024) / sizeof(TTCluster);
    if (cluster_count == 0) {
        cluster_count = 1;
    }
    void* mem = NULL;
    if (posix_memalign(&mem, 64, cluster_count * sizeof(TTCluster)) != 0) {
        fprintf(stderr, "Failed to allocate transposition table!\n");
        cluster_count = 0;
        return;
    }
    table = (TTCluster*)mem;
    clear_tt();
    printf("info string TT initialized with %llu entries (%.2f MB)\n",
           (unsigned long long)(cluster_count * TT_CLUSTER_SIZE),
           (double)(cluster_count * sizeof(TTCluster)) / (1024 * 1024));
}

void clear_tt() {
    if (table != NULL && cluster_count > 0) {
        memset(table, 0, cluster_count * sizeof(TTCluster));
    }
    generation8 = 0;
}

void tt_new_search() {
    generation8 += TT_GENERATION_DELTA;  // uint8 wraps around by itself
}

TTData tt_probe(uint64_t key) {
    TTData data = {0};
    if (table == NULL) return data;

    TTCluster* cluster = cluster_for(key);
    uint16_t key16 = (uint16_t)key;

    for (int i = 0; i < TT_CLUSTER_SIZE; i++) {
        TTEntry* e = &cluster->entry[i];
        if (e->key16 == key16 && e->depth8) {
            // Refresh generation so entries that keep getting hit survive
            e->genBound8 = (uint8_t)(generation8 | (e->genBound8 & (TT_GENERATION_DELTA - 1)));

            data.found = true;
            data.is_pv = (e->genBound8 >> 2) & 1;
            data.bound = e->genBound8 & 0x3;
            data.depth = (int)e->depth8 - TT_DEPTH_OFFSET;
            data.score = e->score16;
            data.eval  = e->eval16;
            data.move  = entry_move(e);
            return data;
        }
    }
    return data;
}

void tt_store(uint64_t key, int depth, int score, uint8_t bound, Move best_move, bool is_pv, int eval) {
    if (table == NULL) return;

    TTCluster* cluster = cluster_for(key);
    uint16_t key16 = (uint16_t)key;

    // Prefer the entry already holding this position, then any empty slot
    TTEntry* replace = NULL;
    for (int i = 0; i < TT_CLUSTER_SIZE; i++) {
        TTEntry* e = &cluster->entry[i];
        if (e->key16 == key16 || !e->depth8) {
            replace = e;
            break;
        }
    }
    // Otherwise evict the entry with the lowest depth, discounted by age
    if (replace == NULL) {
        replace = &cluster->entry[0];
        for (int i = 1; i < TT_CLUSTER_SIZE; i++) {
            TTEntry* e = &cluster->entry[i];
            if (replace->depth8 - relative_age(replace->genBound8) * 2 >
                e->depth8 - relative_age(e->genBound8) * 2) {
                replace = e;
            }
        }
    }

    // Keep the old move if the new search produced none for the same position
    if (best_move != 0 || key16 != replace->key16) {
        replace->move_lo = (uint16_t)best_move;
        replace->move_hi = (uint16_t)(best_move >> 16);
    }

    int depth8 = depth + TT_DEPTH_OFFSET;
    if (depth8 < 1) depth8 = 1;
    if (depth8 > 255) depth8 = 255;

    // Overwrite less valuable entries: exact bounds and new positions always
    // win, otherwise require comparable depth or a stale generation
    if (bound == TT_EXACT || key16 != replace->key16 ||
        depth8 + 2 * is_pv > replace->depth8 - 4 ||
        relative_age(replace->genBound8)) {
        replace->key16 = key16;
        replace->score16 = (int16_t)score;
        replace->eval16 = (int16_t)eval;
        replace->depth8 = (uint8_t)depth8;
        replace->genBound8 = (uint8_t)(generation8 | ((uint8_t)is_pv << 2) | bound);
    }
}

void tt_prefetch(uint64_t key) {
    if (table == NULL) return;
    __builtin_prefetch(cluster_for(key), 0, 1);
}

int tt_hashfull() {
    if (table == NULL || cluster_count == 0) return 0;

    // Sample the first 1000 clusters, counting entries of the current generation
    int cnt = 0;
    uint64_t samples = cluster_count < 1000 ? cluster_count : 1000;
    for (uint64_t i = 0; i < samples; i++) {
        for (int j = 0; j < TT_CLUSTER_SIZE; j++) {
            const TTEntry* e = &table[i].entry[j];
            cnt += e->depth8 && (e->genBound8 & TT_GENERATION_MASK) == generation8;
        }
    }
    return (int)(cnt * 1000 / (samples * TT_CLUSTER_SIZE));
}

void free_tt() {
    if (table != NULL) {
        free(table);
        table = NULL;
        cluster_count = 0;
    }
}
