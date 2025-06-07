#include "move.h"
#include <stdio.h> // For sprintf, printf

// Helper to convert square to algebraic notation (e.g., 0 -> "a1")
static void squareToAlgebraic(Square s, char* alg) {
    alg[0] = (s % 8) + 'a';
    alg[1] = (s / 8) + '1';
    alg[2] = '\0';
}

// Implementation for the declared squareToString
void squareToString(Square sq, char* strBuffer) {
    squareToAlgebraic(sq, strBuffer);
}

// Helper to convert promotion piece to char
static char promotionToChar(int promotion_piece_val) {
    switch (promotion_piece_val) {
        case PROMOTION_N: return 'n';
        case PROMOTION_B: return 'b';
        case PROMOTION_R: return 'r';
        case PROMOTION_Q: return 'q';
        default: return ' '; // Should not happen if promotion_piece_val is 0
    }
}

char* moveToString(Move move, char* strBuffer) {
    Square from = MOVE_FROM(move);
    Square to = MOVE_TO(move);
    int promotion = MOVE_PROMOTION(move);

    char fromStr[3];
    char toStr[3];
    squareToAlgebraic(from, fromStr);
    squareToAlgebraic(to, toStr);

    if (promotion != 0) {
        sprintf(strBuffer, "%s%s%c", fromStr, toStr, promotionToChar(promotion));
    } else {
        sprintf(strBuffer, "%s%s", fromStr, toStr);
    }
    return strBuffer;
}

void printMove(Move move) {
    char moveStr[6]; // Max: "a1h8q\0"
    printf("%s", moveToString(move, moveStr));
}
