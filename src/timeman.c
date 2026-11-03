#include "timeman.h"

static long clamp_long(long v, long lo, long hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void tm_params_init(TMParams* p) {
    p->move_overhead = 10;
    p->moves_div = 20;
    p->inc_pct = 75;
    p->soft_pct = 60;
    p->hard_pct = 300;
    p->hard_max_pct = 75;
    p->stab_base = 160;
    p->stab_step = 17;
    p->node_base = 150;
    p->node_scale = 135;
    p->score_mult = 100;
}

TimeLimits tm_allocate(const TMParams* p, long time_ms, long inc_ms, int movestogo) {
    // Usable clock. A clock at or below the overhead still gets 1 ms so the
    // search returns the depth-1 move instead of running without a limit.
    long avail = time_ms - p->move_overhead;
    if (avail < 1) avail = 1;
    if (inc_ms < 0) inc_ms = 0;

    int moves = p->moves_div;
    if (movestogo > 0 && movestogo < moves) moves = movestogo;
    if (moves < 1) moves = 1;

    long base = avail / moves + inc_ms * p->inc_pct / 100;

    TimeLimits limits;
    limits.hard = base * p->hard_pct / 100;
    long hard_cap = avail * p->hard_max_pct / 100;
    if (limits.hard > hard_cap) limits.hard = hard_cap;
    limits.hard = clamp_long(limits.hard, 1, avail);
    limits.soft = clamp_long(base * p->soft_pct / 100, 1, limits.hard);
    return limits;
}

TimeLimits tm_movetime(const TMParams* p, long movetime_ms) {
    long t = movetime_ms - p->move_overhead;
    if (t < 1) t = 1;
    TimeLimits limits = { t, t };
    return limits;
}

long tm_scaled_soft(const TMParams* p, const TimeLimits* limits,
                    int stability, int best_node_pct, int score_drop) {
    if (stability < 0) stability = 0;
    if (stability > TM_MAX_STABILITY) stability = TM_MAX_STABILITY;
    long stab = p->stab_base - (long)p->stab_step * stability;

    best_node_pct = (int)clamp_long(best_node_pct, 0, 100);
    long node = (long)(p->node_base - best_node_pct) * p->node_scale / 100;

    long score = 100 + clamp_long((long)score_drop * p->score_mult / 100, -20, 50);

    if (stab < 10) stab = 10;
    if (node < 10) node = 10;

    long soft = limits->soft * stab * node * score / 1000000;
    return clamp_long(soft, 1, limits->hard);
}
