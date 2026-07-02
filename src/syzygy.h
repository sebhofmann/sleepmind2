#ifndef SYZYGY_H
#define SYZYGY_H

#include "board.h"
#include "move.h"
#include <stdbool.h>

// =============================================================================
// Syzygy tablebase adapter
//
// This is the ONLY interface between the engine and the vendored Fathom probing
// code (tbprobe.c). Nothing outside this module should include tbprobe.h.
// =============================================================================

// Load tablebases from `path` (a Fathom path string, e.g. "/home/user/syzygy").
// An empty or NULL path disables the tablebases. Safe to call repeatedly.
void syzygy_init(const char* path);

// Release tablebase resources. Safe to call when nothing is loaded.
void syzygy_free(void);

// Largest number of pieces for which tablebases are loaded (0 = disabled).
int syzygy_max_pieces(void);

// True if a position with exactly `piece_count` pieces can be probed.
bool syzygy_available(int piece_count);

// WDL probe for use inside the search (non-root nodes).
// On success returns true and sets *wdl, from the side-to-move's perspective:
//   -1 = loss, 0 = draw (incl. cursed win / blessed loss), +1 = win.
// Returns false if the probe failed (e.g. file missing, rule50 != 0).
bool syzygy_probe_wdl(const Board* board, int* wdl);

#define SYZYGY_MAX_PV 256

// Result of a root DTZ probe: the set of TB-optimal moves (engine encoding),
// plus the DTZ-optimal principal variation walked out to mate.
typedef struct {
    Move moves[MAX_MOVES]; // optimal moves, in engine Move encoding
    int count;             // number of optimal moves
    int wdl;               // -1 loss, 0 draw, +1 win (side to move)

    Move pv[SYZYGY_MAX_PV]; // DTZ-optimal principal variation from the root
    int pvLen;              // number of plies in pv
    int matePlies;          // plies until mate along pv, or -1 if no forced mate
} SyzygyRootResult;

// Root DTZ probe (uses the DTZ tables, falls back to WDL ranking).
// On success fills `out` with the optimal root moves and WDL, returns true.
// Should be called once at the root, never inside the search.
bool syzygy_probe_root(const Board* board, SyzygyRootResult* out);

// Probe the TB-optimal move for a position within tablebase range (no PV walk).
// Outputs (side-to-move perspective): *move = optimal engine move, *wdl in
// {-1,0,+1}, *dtz = distance-to-zero (>= 0, "steps toward conversion/mate").
// Return value:
//    1 = move found (all outputs set)
//    0 = checkmate (side to move is mated)
//   -1 = stalemate
//   -2 = probe failed / not available / move unmappable
int syzygy_probe_play(const Board* board, Move* move, int* wdl, int* dtz);

#endif // SYZYGY_H
