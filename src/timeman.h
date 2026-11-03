#ifndef TIMEMAN_H
#define TIMEMAN_H

// Time management: pure functions, independent of board and search state.
//
// tm_allocate() turns the clock into a soft and a hard limit once per move.
// tm_scaled_soft() then stretches or shrinks the soft limit after every
// completed iteration, based on feedback from the search.

// Tunable parameters. Percent values use 100 as 1.0.
typedef struct {
    int move_overhead;   // ms reserved per move for GUI/network latency (default: 10)
    int moves_div;       // expected remaining moves in sudden death (default: 20)
    int inc_pct;         // share of the increment spent per move (default: 75)
    int soft_pct;        // soft limit as share of the base time (default: 60)
    int hard_pct;        // hard limit as share of the base time (default: 300)
    int hard_max_pct;    // hard limit cap as share of the remaining clock (default: 75)
    int stab_base;       // soft scale when the best move just changed (default: 160)
    int stab_step;       // scale reduction per stable iteration (default: 17)
    int node_base;       // node scale = (node_base - best move node %) * node_scale / 100
    int node_scale;      // (defaults: 150, 135)
    int score_mult;      // soft scale change in % per 100 cp score drop (default: 100)
} TMParams;

typedef struct {
    long soft;  // no new iteration is started after this (before scaling)
    long hard;  // the search is aborted at this point
} TimeLimits;

// Best-move stability saturates after this many unchanged iterations.
#define TM_MAX_STABILITY 5
// Iterations below this depth are too noisy to scale the soft limit.
#define TM_MIN_SCALE_DEPTH 7

void tm_params_init(TMParams* p);

// Limits for a normal clock (wtime/btime, optional increment and movestogo).
// Both limits are >= 1 and never exceed the clock minus the move overhead.
TimeLimits tm_allocate(const TMParams* p, long time_ms, long inc_ms, int movestogo);

// Limits for "go movetime": the full time minus the move overhead.
TimeLimits tm_movetime(const TMParams* p, long movetime_ms);

// Soft limit for the next stop decision.
//   stability:      iterations in a row with the same best move (0 = just changed)
//   best_node_pct:  share of this iteration's nodes spent on the best move (0-100)
//   score_drop:     previous iteration's score minus the current one, in cp
// The result lies in [1, limits->hard].
long tm_scaled_soft(const TMParams* p, const TimeLimits* limits,
                    int stability, int best_node_pct, int score_drop);

#endif // TIMEMAN_H
