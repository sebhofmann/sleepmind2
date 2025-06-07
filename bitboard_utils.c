#include "bitboard_utils.h"
#include <stdio.h>

void printBitboard(Bitboard bb) {
    printf("\n");
    for (int rank = 7; rank >= 0; rank--) {
        printf("%d ", rank + 1);
        for (int file = 0; file < 8; file++) {
            int square = rank * 8 + file;
            printf(" %c", GET_BIT(bb, square) ? 'X' : '.');
        }
        printf("\n");
    }
    printf("   a b c d e f g h\n\n");
    printf("Bitboard: 0x%016llX\n", (unsigned long long)bb);
}
