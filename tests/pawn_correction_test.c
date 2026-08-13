#include "search.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    int value = 0;
    for (int i = 0; i < 1000; i++)
        value = pawn_correction_ewma(value, 80, 16);
    assert(value == 80);

    value = 0;
    for (int i = 0; i < 1000; i++)
        value = pawn_correction_ewma(value, -80, 16);
    assert(value == -80);

    // One grain-scaled centipawn must learn even at the minimum update depth.
    value = pawn_correction_ewma(0, 256, 4);
    assert(value != 0);

    // A sign change must converge to the new target.
    value = 80;
    for (int i = 0; i < 1000; i++)
        value = pawn_correction_ewma(value, -80, 16);
    assert(value == -80);

    // Zero bonus decays history; sub-centipawn residue is immaterial when read.
    value = 1024;
    for (int i = 0; i < 1000; i++)
        value = pawn_correction_ewma(value, 0, 4);
    assert(value < 256);

    assert(pawn_correction_ewma(100, 100, 16) == 100);
    assert(pawn_correction_ewma(-100, -100, 16) == -100);

    puts("pawn correction EWMA tests passed");
    return 0;
}
