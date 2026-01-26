#include <string.h>
#include "board.h"
#include "board_io.h"
#include "move_generator.h"
#include "move.h"
#include <stdio.h>
#include "uci.h"
#include "zobrist.h"
#include "tt.h"

int main(int argc, char const *argv[])
{
   (void)argc;
   (void)argv;
   init_zobrist_keys(); // Initialize Zobrist hashing keys
   init_tt(256);        // Initialize transposition table with 256MB size
   // initMoveGenerator() is called in uci_loop() - don't call it twice
   
   uci_loop(); // Start the UCI loop - handles NNUE initialization and move generator init

    return 0;
}
