#include <string.h>
#include "board.h"
#include "board_io.h"
#include "move_generator.h"
#include "move.h"
#include <stdio.h>
#include "uci.h"
#include "zobrist.h"
#include "tt.h" // Added for Zobrist and TT initialization

int main(int argc, char const *argv[])
{
   init_zobrist_keys(); // Initialize Zobrist hashing keys
   init_tt(64);         // Initialize transposition table with 64MB size
   initMoveGenerator(); // Initialize move generator data

    uci_loop(); // Start the UCI loop

    return 0;
}
