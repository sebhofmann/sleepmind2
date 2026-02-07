#ifndef BITBOARD_UTILS_H
#define BITBOARD_UTILS_H

#include "board.h" // For Bitboard type
#include <stdint.h>
#include <stdio.h> // For printf in printBitboard

// Get the bit at a given square
#define GET_BIT(bb, sq) (((bb) >> (sq)) & 1ULL)

// Set the bit at a given square
#define SET_BIT(bb, sq) ((bb) |= (1ULL << (sq)))

// Clear the bit at a given square
#define CLEAR_BIT(bb, sq) ((bb) &= ~(1ULL << (sq)))

// Count set bits (popcount)
#if defined(__GNUC__) || defined(__clang__)
#define POPCOUNT(bb) __builtin_popcountll(bb)
#else
// Fallback popcount (can be slow, consider a lookup table or better algorithm for other compilers)
static inline int POPCOUNT(Bitboard bb) {
    int count = 0;
    while (bb > 0) {
        bb &= (bb - 1);
        count++;
    }
    return count;
}
#endif

// Get index of least significant bit (LSB) - Bit Scan Forward (BSF)
#if defined(__GNUC__) || defined(__clang__)
#define BIT_SCAN_FORWARD(bb) (bb ? __builtin_ctzll(bb) : -1) // Returns -1 if bb is 0
#else
// Fallback LSB (can be slow)
static inline int BIT_SCAN_FORWARD(Bitboard bb) {
    if (bb == 0) return -1;
    unsigned long index;
    // Manual scan or use compiler intrinsics if available for MSVC e.g. _BitScanForward64
    for (index = 0; index < 64; index++) {
        if ((bb >> index) & 1ULL) return index;
    }
    return -1; // Should be unreachable if bb != 0
}
#endif

// Get index of most significant bit (MSB) - Bit Scan Reverse (BSR)
#if defined(__GNUC__) || defined(__clang__)
#define BIT_SCAN_REVERSE(bb) (bb ? (63 - __builtin_clzll(bb)) : -1) // Returns -1 if bb is 0
#else
// Fallback MSB (can be slow)
static inline int BIT_SCAN_REVERSE(Bitboard bb) {
    if (bb == 0) return -1;
    unsigned long index;
     // Manual scan or use compiler intrinsics if available for MSVC e.g. _BitScanReverse64
    for (index = 63; index >= 0; index--) {
        if ((bb >> index) & 1ULL) return index;
        if (index == 0) break; // prevent underflow with unsigned
    }
    return -1; // Should be unreachable if bb != 0
}
#endif

// Helper to print a bitboard (for debugging)
void printBitboard(Bitboard bb);

#endif // BITBOARD_UTILS_H
