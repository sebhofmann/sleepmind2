#include <string.h>
#include "board.h"
#include "board_io.h"
#include "move_generator.h"
#include "move.h"
#include <stdio.h>
#include "uci.h"
#include "zobrist.h"
#include "tt.h" // Added for Zobrist and TT initialization
#include "evaluation.h" // Added for NNUE initialization

int main(int argc, char const *argv[])
{
   init_zobrist_keys(); // Initialize Zobrist hashing keys
   init_tt(256);         // Initialize transposition table with 64MB size
   initMoveGenerator(); // Initialize move generator data
   
   // Initialize NNUE evaluation
   // Try to load nnue.bin from current directory, or use random weights if not found
   eval_init("nnue_i768_h1024_bin5_bout8_v2_1200.bin");

    uci_loop(); // Start the UCI loop

    return 0;
}
