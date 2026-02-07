#ifndef BOARD_IO_H
#define BOARD_IO_H

#include "board.h"
#include <stdio.h>
#include <string.h>

Board parseFEN(const char* fen);
const char* outputFEN(const Board* board);
void printBoard(const Board* board);


#endif // BOARD_IO_H

