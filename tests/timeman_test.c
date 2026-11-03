#include "timeman.h"
#include <assert.h>
#include <stdio.h>

static void check_limits(TimeLimits l, long time_ms, const TMParams* p) {
    long avail = time_ms - p->move_overhead;
    if (avail < 1) avail = 1;
    assert(l.soft >= 1);
    assert(l.hard >= l.soft);
    assert(l.hard <= avail);
}

int main(void) {
    TMParams p;
    tm_params_init(&p);

    // Limits stay inside the usable clock for every clock, increment and
    // movestogo, including clocks below the move overhead.
    static const long times[] = {0, 1, 5, 10, 11, 20, 100, 999, 1000, 10000, 60000, 3600000};
    static const long incs[] = {0, 10, 100, 1000, 30000};
    static const int mtgs[] = {0, 1, 2, 10, 40, 200};
    for (unsigned t = 0; t < sizeof(times) / sizeof(times[0]); t++)
        for (unsigned i = 0; i < sizeof(incs) / sizeof(incs[0]); i++)
            for (unsigned m = 0; m < sizeof(mtgs) / sizeof(mtgs[0]); m++) {
                TimeLimits l = tm_allocate(&p, times[t], incs[i], mtgs[m]);
                check_limits(l, times[t], &p);
                // Every scale combination stays within [1, hard].
                for (int stab = 0; stab <= 8; stab++)
                    for (int pct = 0; pct <= 100; pct += 25)
                        for (int drop = -300; drop <= 300; drop += 100) {
                            long s = tm_scaled_soft(&p, &l, stab, pct, drop);
                            assert(s >= 1 && s <= l.hard);
                        }
            }

    // More clock never means less time.
    long prev_soft = 0, prev_hard = 0;
    for (long t = 0; t <= 200000; t += 37) {
        TimeLimits l = tm_allocate(&p, t, 100, 0);
        assert(l.soft >= prev_soft && l.hard >= prev_hard);
        prev_soft = l.soft;
        prev_hard = l.hard;
    }

    // 10+0.1: base = 9990/20 + 75 = 574 -> soft 344, hard 1722.
    TimeLimits l = tm_allocate(&p, 10000, 100, 0);
    assert(l.soft == 344 && l.hard == 1722);

    // The last move before the time control must not use the whole clock.
    l = tm_allocate(&p, 10000, 0, 1);
    assert(l.hard == 9990 * 75 / 100);
    assert(l.soft == 9990 * 60 / 100);

    // A huge increment on an almost empty clock is capped by the clock.
    l = tm_allocate(&p, 50, 30000, 0);
    assert(l.hard == 40 * 75 / 100 && l.soft == l.hard);

    // movetime keeps the overhead and never drops to "unlimited" (0).
    l = tm_movetime(&p, 1000);
    assert(l.soft == 990 && l.hard == 990);
    l = tm_movetime(&p, 5);
    assert(l.soft == 1 && l.hard == 1);

    // Scaling direction: instability, a small node share and a falling
    // score each buy more time.
    l = tm_allocate(&p, 60000, 600, 0);
    assert(tm_scaled_soft(&p, &l, 0, 50, 0) > tm_scaled_soft(&p, &l, 5, 50, 0));
    assert(tm_scaled_soft(&p, &l, 2, 20, 0) > tm_scaled_soft(&p, &l, 2, 90, 0));
    assert(tm_scaled_soft(&p, &l, 2, 50, 40) > tm_scaled_soft(&p, &l, 2, 50, 0));
    assert(tm_scaled_soft(&p, &l, 2, 50, 0) > tm_scaled_soft(&p, &l, 2, 50, -40));
    // A stable, dominant best move stops well before the unscaled soft limit.
    assert(tm_scaled_soft(&p, &l, 5, 95, 0) < l.soft * 60 / 100);

    printf("timeman_test: OK\n");
    return 0;
}
