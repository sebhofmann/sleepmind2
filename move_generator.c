#include "move_generator.h"
#include "bitboard_utils.h" // For POPCOUNT and other bit utilities
#include "board.h"          // For Square constants, Board struct, Castling rights
#include "move.h"           // For Move structure, macros, PieceTypeToken, addMove
#include "board_io.h"       // For outputFEN, printBoard
#include <stdio.h>
#include <string.h> // For memset
#include <stdlib.h> // For rand, srand, malloc, calloc, free
#include <time.h>   // For time() to seed rand
#include <stdbool.h>// For bool type

// --- Magic Bitboard Data ---
static Bitboard ROOK_MAGICS[64];
static Bitboard BISHOP_MAGICS[64];

static Bitboard ROOK_MASKS[64];
static Bitboard BISHOP_MASKS[64];

// Array of pointers to attack tables. Each table is dynamically allocated.
static Bitboard* ROOK_ATTACKS_TABLE[64];
static Bitboard* BISHOP_ATTACKS_TABLE[64];

// Number of relevant bits in the mask for each square
static int ROOK_RELEVANT_BITS[64];
static int BISHOP_RELEVANT_BITS[64];

// --- Precomputed Attack Tables (Non-sliding pieces) ---
static Bitboard PAWN_ATTACKS[2][64];   // [color][square] (0 for white, 1 for black)
static Bitboard KNIGHT_ATTACKS[64];
static Bitboard KING_ATTACKS[64];


// --- Helper Functions based on User's Example ---

const int BitTable[64] = { // Used by pop_lsb
  63, 30, 3, 32, 25, 41, 22, 33, 15, 50, 42, 13, 11, 53, 19, 34, 61, 29, 2,
  51, 21, 43, 45, 10, 18, 47, 1, 54, 9, 57, 0, 35, 62, 31, 40, 4, 49, 5, 52,
  26, 60, 6, 23, 44, 46, 27, 56, 16, 7, 39, 48, 24, 59, 14, 12, 55, 38, 28,
  58, 20, 37, 17, 36, 8
};

// Pop the least significant bit and return its index
static inline int pop_lsb(Bitboard *bb) {
  Bitboard b = *bb ^ (*bb - 1); // Isolate LSB
  unsigned int fold = (unsigned) ((b & 0xffffffff) ^ (b >> 32));
  *bb &= (*bb - 1); // Clear LSB
  return BitTable[(fold * 0x783a9b23) >> 26];
}

// Helper to get the index of the least significant bit without modifying the bitboard
static inline int get_lsb_index(Bitboard bb) {
    if (bb == 0) return SQ_NONE; // SQ_NONE is an invalid square index
    Bitboard b = bb ^ (bb - 1); // Isolate LSB - KORRIGIERT, um mit pop_lsb's Hash-Logik übereinzustimmen
    // The rest of this logic is from pop_lsb's way of finding index from LSB
    unsigned int fold = (unsigned) ((b & 0xffffffff) ^ (b >> 32));
    return BitTable[(fold * 0x783a9b23) >> 26];
}

// Helper: Get a pointer to the bitboard of the piece at a given square for modification
static Bitboard* getMutablePieceBitboardAtSquare(Board* board, Square sq, bool isWhiteMoving) {
    Bitboard s = 1ULL << sq;
    if (isWhiteMoving) { // If white is moving, the piece at 'sq' must be white
        if (board->whitePawns & s) return &board->whitePawns;
        if (board->whiteKnights & s) return &board->whiteKnights;
        if (board->whiteBishops & s) return &board->whiteBishops;
        if (board->whiteRooks & s) return &board->whiteRooks;
        if (board->whiteQueens & s) return &board->whiteQueens;
        if (board->whiteKings & s) return &board->whiteKings;
    } else { // If black is moving, the piece at 'sq' must be black
        if (board->blackPawns & s) return &board->blackPawns;
        if (board->blackKnights & s) return &board->blackKnights;
        if (board->blackBishops & s) return &board->blackBishops;
        if (board->blackRooks & s) return &board->blackRooks;
        if (board->blackQueens & s) return &board->blackQueens;
        if (board->blackKings & s) return &board->blackKings;
    }
    return NULL; 
}

// Helper: Remove any piece from a square on all bitboards
static void clearSquareOnAllBitboards(Board* board, Square sq) {
    Bitboard s = ~(1ULL << sq); // Mask to clear the bit at sq
    board->whitePawns &= s; board->whiteKnights &= s; board->whiteBishops &= s;
    board->whiteRooks &= s; board->whiteQueens &= s; board->whiteKings &= s;
    board->blackPawns &= s; board->blackKnights &= s; board->blackBishops &= s;
    board->blackRooks &= s; board->blackQueens &= s; board->blackKings &= s;
}

// Generate a specific occupancy permutation from an index and a mask
static Bitboard index_to_occupancy(int index, int bits, Bitboard mask) {
  Bitboard result = 0ULL;
  Bitboard temp_mask = mask; // Use a copy to pop bits from
  for (int i = 0; i < bits; i++) {
    int sq = pop_lsb(&temp_mask); // Get square from mask's LSB
    if (index & (1 << i)) {      // If this bit is set in the index
      result |= (1ULL << sq);  // Set this bit in the occupancy
    }
  }
  return result;
}

// Mask generation (from user's example, using <7 and >0 for excluding borders)
static Bitboard generate_rook_mask_user(Square sq) {
    Bitboard result = 0ULL;
    int rk = sq / 8, fl = sq % 8;
    int r, f;

    for (r = rk + 1; r < 7; r++) result |= (1ULL << (fl + r * 8));
    for (r = rk - 1; r > 0; r--) result |= (1ULL << (fl + r * 8));
    for (f = fl + 1; f < 7; f++) result |= (1ULL << (f + rk * 8));
    for (f = fl - 1; f > 0; f--) result |= (1ULL << (f + rk * 8));
    return result;
}

static Bitboard generate_bishop_mask_user(Square sq) {
    Bitboard result = 0ULL;
    int rk = sq / 8, fl = sq % 8;
    int r, f;

    for (r = rk + 1, f = fl + 1; r < 7 && f < 7; r++, f++) result |= (1ULL << (f + r * 8));
    for (r = rk + 1, f = fl - 1; r < 7 && f > 0; r++, f--) result |= (1ULL << (f + r * 8));
    for (r = rk - 1, f = fl + 1; r > 0 && f < 7; r--, f++) result |= (1ULL << (f + r * 8));
    for (r = rk - 1, f = fl - 1; r > 0 && f > 0; r--, f--) result |= (1ULL << (f + r * 8));
    return result;
}

// On-the-fly attack generation (from user's example, using <=7 and >=0 for including borders)
static Bitboard generate_rook_attacks_otf_user(Square sq, Bitboard blockers) {
    Bitboard result = 0ULL;
    int rk = sq / 8, fl = sq % 8;
    int r, f;

    for (r = rk + 1; r <= 7; r++) { result |= (1ULL << (fl + r * 8)); if (blockers & (1ULL << (fl + r * 8))) break; }
    for (r = rk - 1; r >= 0; r--) { result |= (1ULL << (fl + r * 8)); if (blockers & (1ULL << (fl + r * 8))) break; }
    for (f = fl + 1; f <= 7; f++) { result |= (1ULL << (f + rk * 8)); if (blockers & (1ULL << (f + rk * 8))) break; }
    for (f = fl - 1; f >= 0; f--) { result |= (1ULL << (f + rk * 8)); if (blockers & (1ULL << (f + rk * 8))) break; }
    return result;
}

static Bitboard generate_bishop_attacks_otf_user(Square sq, Bitboard blockers) {
    Bitboard result = 0ULL;
    int rk = sq / 8, fl = sq % 8;
    int r, f;

    for (r = rk + 1, f = fl + 1; r <= 7 && f <= 7; r++, f++) { result |= (1ULL << (f + r * 8)); if (blockers & (1ULL << (f + r * 8))) break; }
    for (r = rk + 1, f = fl - 1; r <= 7 && f >= 0; r++, f--) { result |= (1ULL << (f + r * 8)); if (blockers & (1ULL << (f + r * 8))) break; }
    for (r = rk - 1, f = fl + 1; r >= 0 && f <= 7; r--, f++) { result |= (1ULL << (f + r * 8)); if (blockers & (1ULL << (f + r * 8))) break; }
    for (r = rk - 1, f = fl - 1; r >= 0 && f >= 0; r--, f--) { result |= (1ULL << (f + r * 8)); if (blockers & (1ULL << (f + r * 8))) break; }
    return result;
}

// Random bitboard generation (adapted from user's reference code)
static Bitboard random_u64() {
  Bitboard u1, u2, u3, u4;
  // Assuming random() is available (common on POSIX systems like macOS)
  // and returns long, so casting to Bitboard (uint64_t) is fine.
  u1 = (Bitboard)(random()) & 0xFFFFULL;
  u2 = (Bitboard)(random()) & 0xFFFFULL;
  u3 = (Bitboard)(random()) & 0xFFFFULL;
  u4 = (Bitboard)(random()) & 0xFFFFULL;
  return u1 | (u2 << 16) | (u3 << 32) | (u4 << 48);
}

static Bitboard random_u64_fewbits() {
    return random_u64() & random_u64() & random_u64();
}

// Magic transformation
static inline unsigned int transform_magic(Bitboard occupancy, Bitboard magic, int relevant_bits) {
// #define USE_32_BIT_MULTIPLICATIONS // Define this if you want to try the 32-bit path
#ifdef USE_32_BIT_MULTIPLICATIONS
    return (unsigned int)(((unsigned int)(occupancy * magic)) >> (32 - relevant_bits));
#else
    return (unsigned int)((occupancy * magic) >> (64 - relevant_bits));
#endif
}

// Main function to find magic for a square
static bool find_magic_for_square_user(Square sq, bool is_rook, int max_attempts) {
    Bitboard mask = is_rook ? ROOK_MASKS[sq] : BISHOP_MASKS[sq];
    int relevant_bits = is_rook ? ROOK_RELEVANT_BITS[sq] : BISHOP_RELEVANT_BITS[sq];
    
    if (relevant_bits == 0 && (is_rook ? ROOK_MASKS[sq] : BISHOP_MASKS[sq]) == 0ULL) { // Edge cases like A1 for bishop mask
        if (is_rook) {
            ROOK_MAGICS[sq] = 0ULL; // No magic needed for empty mask
            ROOK_ATTACKS_TABLE[sq] = (Bitboard*)calloc(1, sizeof(Bitboard)); 
            if (ROOK_ATTACKS_TABLE[sq]) ROOK_ATTACKS_TABLE[sq][0] = generate_rook_attacks_otf_user(sq, 0ULL);
            else { printf("Error: Mem alloc for empty mask rook sq %d\n", sq); return false;}
        } else {
            BISHOP_MAGICS[sq] = 0ULL;
            BISHOP_ATTACKS_TABLE[sq] = (Bitboard*)calloc(1, sizeof(Bitboard));
            if (BISHOP_ATTACKS_TABLE[sq]) BISHOP_ATTACKS_TABLE[sq][0] = generate_bishop_attacks_otf_user(sq, 0ULL);
            else { printf("Error: Mem alloc for empty mask bishop sq %d\n", sq); return false;}
        }
        return true;
    }
    if (relevant_bits < 0 || relevant_bits > 15) { // Safety check for table sizes (max 2^15 entries for practical purposes)
        printf("Error: Invalid relevant_bits %d for sq %d, is_rook %d\n", relevant_bits, sq, is_rook);
        return false;
    }


    int num_occupancy_states = 1 << relevant_bits;

    Bitboard* occupancies = (Bitboard*)malloc(num_occupancy_states * sizeof(Bitboard));
    Bitboard* attacks = (Bitboard*)malloc(num_occupancy_states * sizeof(Bitboard));
    Bitboard* current_attack_table_ptr; // Will point to the dynamically allocated attack table

    size_t attack_table_size_bytes = num_occupancy_states * sizeof(Bitboard);
    current_attack_table_ptr = (Bitboard*)malloc(attack_table_size_bytes);

    if (is_rook) {
        ROOK_ATTACKS_TABLE[sq] = current_attack_table_ptr;
    } else {
        BISHOP_ATTACKS_TABLE[sq] = current_attack_table_ptr;
    }
    
    if (!occupancies || !attacks || !current_attack_table_ptr) {
        printf("Error: Memory allocation failed for magic finding (main arrays) on square %d\n", sq);
        if (occupancies) free(occupancies);
        if (attacks) free(attacks);
        if (current_attack_table_ptr) {
            if (is_rook) ROOK_ATTACKS_TABLE[sq] = NULL; else BISHOP_ATTACKS_TABLE[sq] = NULL;
            free(current_attack_table_ptr);
        }
        return false;
    }

    for (int i = 0; i < num_occupancy_states; i++) {
        occupancies[i] = index_to_occupancy(i, relevant_bits, mask);
        if (is_rook) {
            attacks[i] = generate_rook_attacks_otf_user(sq, occupancies[i]);
        } else {
            attacks[i] = generate_bishop_attacks_otf_user(sq, occupancies[i]);
        }
    }

    bool* index_is_used = (bool*)calloc(num_occupancy_states, sizeof(bool));
     if (!index_is_used) {
        printf("Error: Memory allocation failed for index_is_used on square %d\n", sq);
        free(occupancies);
        free(attacks);
        if (is_rook && ROOK_ATTACKS_TABLE[sq]) { free(ROOK_ATTACKS_TABLE[sq]); ROOK_ATTACKS_TABLE[sq] = NULL; }
        if (!is_rook && BISHOP_ATTACKS_TABLE[sq]) { free(BISHOP_ATTACKS_TABLE[sq]); BISHOP_ATTACKS_TABLE[sq] = NULL; }
        // current_attack_table_ptr was assigned to ROOK/BISHOP_ATTACKS_TABLE[sq]
        return false;
    }

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        Bitboard magic_candidate = random_u64_fewbits();
        // The original condition from user's code:
        // if (POPCOUNT((mask * magic_candidate) & 0xFF00000000000000ULL) < 6) continue; // Temporarily disabled


        memset(index_is_used, 0, num_occupancy_states * sizeof(bool));
        memset(current_attack_table_ptr, 0, attack_table_size_bytes); // Clear before trying to fill
        bool possible_magic = true;

        for (int i = 0; i < num_occupancy_states; i++) {
            unsigned int magic_index = transform_magic(occupancies[i], magic_candidate, relevant_bits);
            
            if (magic_index >= (unsigned int)num_occupancy_states) { // Should not happen if relevant_bits is correct for transform
                 possible_magic = false; break;
            }

            if (!index_is_used[magic_index]) {
                current_attack_table_ptr[magic_index] = attacks[i];
                index_is_used[magic_index] = true;
            } else if (current_attack_table_ptr[magic_index] != attacks[i]) {
                possible_magic = false;
                break;
            }
        }

        if (possible_magic) {
            if (is_rook) {
                ROOK_MAGICS[sq] = magic_candidate;
            } else {
                BISHOP_MAGICS[sq] = magic_candidate;
            }
            free(occupancies);
            free(attacks);
            free(index_is_used);
            return true;
        }
    }

    free(occupancies);
    free(attacks);
    free(index_is_used);
    // If magic not found, free the allocated attack table
    if (is_rook && ROOK_ATTACKS_TABLE[sq]) { free(ROOK_ATTACKS_TABLE[sq]); ROOK_ATTACKS_TABLE[sq] = NULL; }
    if (!is_rook && BISHOP_ATTACKS_TABLE[sq]) { free(BISHOP_ATTACKS_TABLE[sq]); BISHOP_ATTACKS_TABLE[sq] = NULL; }
    return false;
}

bool findAndInitMagicNumbers() {
    srandom((unsigned int)time(NULL)); // Use srandom for random()
    printf("Attempting to find magic numbers (user logic, corrected random)... This may take a while.\n");
    int magicAttemptsPerSquare = 10000000; // Kept high, but should succeed faster if logic is good.

    for (Square sq = 0; sq < 64; sq++) {
        ROOK_MAGICS[sq] = 0ULL; // Reset
        BISHOP_MAGICS[sq] = 0ULL;
        if(ROOK_ATTACKS_TABLE[sq]) { free(ROOK_ATTACKS_TABLE[sq]); ROOK_ATTACKS_TABLE[sq] = NULL; }
        if(BISHOP_ATTACKS_TABLE[sq]) { free(BISHOP_ATTACKS_TABLE[sq]); BISHOP_ATTACKS_TABLE[sq] = NULL; }

        // Rook
        if (ROOK_RELEVANT_BITS[sq] > 0) { // Only find magic if mask is not empty
            // printf("Finding Rook magic for sq %d (mask bits: %d)...\n\", sq, ROOK_RELEVANT_BITS[sq]);
            if (!find_magic_for_square_user(sq, true, magicAttemptsPerSquare)) {
                 // printf("Failed Rook magic for sq %d.\n\", sq);
            }
        } else { // Empty mask, still need to init attack table for 0 index
            ROOK_ATTACKS_TABLE[sq] = (Bitboard*)calloc(1, sizeof(Bitboard));
            if(ROOK_ATTACKS_TABLE[sq]) ROOK_ATTACKS_TABLE[sq][0] = generate_rook_attacks_otf_user(sq, 0ULL);
        }


        // Bishop
        if (BISHOP_RELEVANT_BITS[sq] > 0) {
            // printf("Finding Bishop magic for sq %d (mask bits: %d)...\n\", sq, BISHOP_RELEVANT_BITS[sq]);
            if (!find_magic_for_square_user(sq, false, magicAttemptsPerSquare)) {
                // printf("Failed Bishop magic for sq %d.\n\", sq);
            }
        } else {
            BISHOP_ATTACKS_TABLE[sq] = (Bitboard*)calloc(1, sizeof(Bitboard));
            if(BISHOP_ATTACKS_TABLE[sq]) BISHOP_ATTACKS_TABLE[sq][0] = generate_bishop_attacks_otf_user(sq, 0ULL);
        }
    }

    printf("Magic number search complete.\n");
    int rooksFound = 0, bishopsFound = 0;
    for(int i=0; i<64; ++i) {
        if(ROOK_MAGICS[i] != 0ULL && ROOK_ATTACKS_TABLE[i] != NULL) rooksFound++;
        else if (ROOK_RELEVANT_BITS[i] == 0 && ROOK_ATTACKS_TABLE[i] != NULL) rooksFound++; // Count empty mask cases as "found"
        
        if(BISHOP_MAGICS[i] != 0ULL && BISHOP_ATTACKS_TABLE[i] != NULL) bishopsFound++;
        else if (BISHOP_RELEVANT_BITS[i] == 0 && BISHOP_ATTACKS_TABLE[i] != NULL) bishopsFound++;
    }
    printf("Found %d/64 Rook magics and %d/64 Bishop magics.\n", rooksFound, bishopsFound);
    if (rooksFound < 64 || bishopsFound < 64) {
        printf("Warning: Not all magic numbers were found. Sliding piece move generation might be slow or incorrect for some squares.\n");
        printf("Consider using pre-calculated magics or increasing attempts (currently %d per square with non-zero mask).\n", magicAttemptsPerSquare);
    }
    return (rooksFound == 64 && bishopsFound == 64);
}

void initMoveGenerator() {
    printf("Initializing Move Generator...\n");

    // Initialize Pawn Attacks
    // PAWN_ATTACKS[0] for White, PAWN_ATTACKS[1] for Black
    for (Square sq = 0; sq < 64; sq++) {
        PAWN_ATTACKS[0][sq] = 0ULL; // White
        PAWN_ATTACKS[1][sq] = 0ULL; // Black

        int r = sq / 8; // rank
        int f = sq % 8; // file

        // White pawn attacks (attacking North-West and North-East from sq)
        if (r < 7) { // White pawns move from rank 0 to 6, attack ranks 1 to 7
            if (f > 0) { // Can attack left (NW)
                PAWN_ATTACKS[0][sq] |= (1ULL << (sq + 7));
            }
            if (f < 7) { // Can attack right (NE)
                PAWN_ATTACKS[0][sq] |= (1ULL << (sq + 9));
            }
        }

        // Black pawn attacks (attacking South-West and South-East from sq)
        if (r > 0) { // Black pawns move from rank 7 to 1, attack ranks 6 to 0
            if (f > 0) { // Can attack left (SW from black's perspective, visually sq - 9)
                PAWN_ATTACKS[1][sq] |= (1ULL << (sq - 9));
            }
            if (f < 7) { // Can attack right (SE from black's perspective, visually sq - 7)
                PAWN_ATTACKS[1][sq] |= (1ULL << (sq - 7));
            }
        }
    }

    // Initialize Knight Attacks
    for (Square sq = 0; sq < 64; sq++) {
        KNIGHT_ATTACKS[sq] = 0ULL;
        int r = sq / 8;
        int f = sq % 8;
        // Possible knight moves (delta_rank, delta_file)
        int dr[] = {-2, -2, -1, -1,  1,  1,  2,  2};
        int dc[] = {-1,  1, -2,  2, -2,  2, -1,  1};
        for (int i = 0; i < 8; i++) {
            int nr = r + dr[i]; // new rank
            int nc = f + dc[i]; // new file
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) { // Check if on board
                KNIGHT_ATTACKS[sq] |= (1ULL << (nr * 8 + nc));
            }
        }
    }

    // Initialize King Attacks
    for (Square sq = 0; sq < 64; sq++) {
        KING_ATTACKS[sq] = 0ULL;
        int r = sq / 8;
        int f = sq % 8;
        // Possible king moves (delta_rank, delta_file)
        int dr[] = {-1, -1, -1,  0,  0,  1,  1,  1};
        int dc[] = {-1,  0,  1, -1,  1, -1,  0,  1};
        for (int i = 0; i < 8; i++) {
            int nr = r + dr[i];
            int nc = f + dc[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) { // Check if on board
                KING_ATTACKS[sq] |= (1ULL << (nr * 8 + nc));
            }
        }
    }

    for (Square sq = 0; sq < 64; sq++) {
        ROOK_MASKS[sq] = generate_rook_mask_user(sq);
        ROOK_RELEVANT_BITS[sq] = POPCOUNT(ROOK_MASKS[sq]);

        BISHOP_MASKS[sq] = generate_bishop_mask_user(sq);
        BISHOP_RELEVANT_BITS[sq] = POPCOUNT(BISHOP_MASKS[sq]);
        
        ROOK_ATTACKS_TABLE[sq] = NULL; // Initialize pointers
        BISHOP_ATTACKS_TABLE[sq] = NULL;
    }

    findAndInitMagicNumbers();
}

Bitboard getRookAttacks(Square square, Bitboard occupancy) {
    if (ROOK_RELEVANT_BITS[square] == 0) { // Empty mask (e.g. corner for bishop mask, but rook masks are usually not empty)
        return ROOK_ATTACKS_TABLE[square] ? ROOK_ATTACKS_TABLE[square][0] : generate_rook_attacks_otf_user(square, occupancy);
    }
    if (ROOK_MAGICS[square] == 0ULL || ROOK_ATTACKS_TABLE[square] == NULL) {
        return generate_rook_attacks_otf_user(square, occupancy);
    }
    Bitboard relevant_occupancy = occupancy & ROOK_MASKS[square];
    unsigned int index = transform_magic(relevant_occupancy, ROOK_MAGICS[square], ROOK_RELEVANT_BITS[square]);
    if (index >= (1U << ROOK_RELEVANT_BITS[square])) { /* Should not happen */ return generate_rook_attacks_otf_user(square, occupancy); }
    return ROOK_ATTACKS_TABLE[square][index];
}

Bitboard getBishopAttacks(Square square, Bitboard occupancy) {
    if (BISHOP_RELEVANT_BITS[square] == 0) { // Empty mask (e.g. corner for bishop mask)
         return BISHOP_ATTACKS_TABLE[square] ? BISHOP_ATTACKS_TABLE[square][0] : generate_bishop_attacks_otf_user(square, occupancy);
    }
    if (BISHOP_MAGICS[square] == 0ULL || BISHOP_ATTACKS_TABLE[square] == NULL) {
        return generate_bishop_attacks_otf_user(square, occupancy);
    }
    Bitboard relevant_occupancy = occupancy & BISHOP_MASKS[square];
    unsigned int index = transform_magic(relevant_occupancy, BISHOP_MAGICS[square], BISHOP_RELEVANT_BITS[square]);
    if (index >= (1U << BISHOP_RELEVANT_BITS[square])) { /* Should not happen */ return generate_bishop_attacks_otf_user(square, occupancy); }
    return BISHOP_ATTACKS_TABLE[square][index];
}

Bitboard getQueenAttacks(Square square, Bitboard occupancy) {
    return getRookAttacks(square, occupancy) | getBishopAttacks(square, occupancy);
}

static Bitboard getOccupiedByColor(const Board* board, bool isWhite) {
    if (isWhite) {
        return board->whitePawns | board->whiteKnights | board->whiteBishops | board->whiteRooks | board->whiteQueens | board->whiteKings;
    } else {
        return board->blackPawns | board->blackKnights | board->blackBishops | board->blackRooks | board->blackQueens | board->blackKings;
    }
}

static void generatePawnMoves(const Board* board, MoveList* list, bool isWhite) {
    Bitboard pawns = isWhite ? board->whitePawns : board->blackPawns;
    Bitboard friendlyPieces = getOccupiedByColor(board, isWhite);
    Bitboard enemyPieces = getOccupiedByColor(board, !isWhite);
    Bitboard allPieces = friendlyPieces | enemyPieces;

    int direction = isWhite ? 1 : -1;
    int startRank = isWhite ? 1 : 6; 
    int promotionRank = isWhite ? 7 : 0;

    Bitboard currentPawns = pawns;
    while (currentPawns) {
        Square fromSq = BIT_SCAN_FORWARD(currentPawns); // Assumes BIT_SCAN_FORWARD is available from bitboard_utils.h
        CLEAR_BIT(currentPawns, fromSq);                // Assumes CLEAR_BIT is available
        int rank = fromSq / 8;

        // 1. Single Push
        Square toSq_single = fromSq + 8 * direction;
        if (toSq_single >=0 && toSq_single < 64 && !GET_BIT(allPieces, toSq_single)) { // Assumes GET_BIT
            if (rank + direction == promotionRank) {
                addMove(list, CREATE_MOVE(fromSq, toSq_single, PROMOTION_Q, 0,0,0,0));
                addMove(list, CREATE_MOVE(fromSq, toSq_single, PROMOTION_R, 0,0,0,0));
                addMove(list, CREATE_MOVE(fromSq, toSq_single, PROMOTION_B, 0,0,0,0));
                addMove(list, CREATE_MOVE(fromSq, toSq_single, PROMOTION_N, 0,0,0,0));
            } else {
                addMove(list, CREATE_MOVE(fromSq, toSq_single, 0,0,0,0,0));
            }

            // 2. Double Push
            if (rank == startRank) {
                Square toSq_double = fromSq + 16 * direction;
                if (toSq_double >=0 && toSq_double < 64 && !GET_BIT(allPieces, toSq_double)) {
                    addMove(list, CREATE_MOVE(fromSq, toSq_double, 0,0,1,0,0));
                }
            }
        }

        // 3. Captures
        Bitboard pawnAttacks = PAWN_ATTACKS[isWhite ? 0 : 1][fromSq];
        Bitboard validCaptures = pawnAttacks & enemyPieces;
        while (validCaptures) {
            Square toSq_capture = BIT_SCAN_FORWARD(validCaptures);
            CLEAR_BIT(validCaptures, toSq_capture);
            if (rank + direction == promotionRank) {
                addMove(list, CREATE_MOVE(fromSq, toSq_capture, PROMOTION_Q, 1,0,0,0));
                addMove(list, CREATE_MOVE(fromSq, toSq_capture, PROMOTION_R, 1,0,0,0));
                addMove(list, CREATE_MOVE(fromSq, toSq_capture, PROMOTION_B, 1,0,0,0));
                addMove(list, CREATE_MOVE(fromSq, toSq_capture, PROMOTION_N, 1,0,0,0));
            } else {
                addMove(list, CREATE_MOVE(fromSq, toSq_capture, 0,1,0,0,0));
            }
        }
        
        // 4. En Passant
        if (board->enPassantSquare != -1) {
            if (GET_BIT(pawnAttacks, board->enPassantSquare)) {
                 addMove(list, CREATE_MOVE(fromSq, board->enPassantSquare, 0,1,0,1,0));
            }
        }
    }
}

static void generatePieceMoves(const Board* board, MoveList* list, bool isWhite, Bitboard pieces, Bitboard attackTable[64]) {
    Bitboard friendlyPieces = getOccupiedByColor(board, isWhite);
    Bitboard enemyPieces = getOccupiedByColor(board, !isWhite);
    Bitboard currentPieces = pieces;
    while (currentPieces) {
        Square fromSq = BIT_SCAN_FORWARD(currentPieces);
        CLEAR_BIT(currentPieces, fromSq);
        Bitboard attacks = attackTable[fromSq];
        Bitboard validMoves = attacks & ~friendlyPieces;
        while (validMoves) {
            Square toSq = BIT_SCAN_FORWARD(validMoves);
            CLEAR_BIT(validMoves, toSq);
            addMove(list, CREATE_MOVE(fromSq, toSq, 0, GET_BIT(enemyPieces, toSq),0,0,0));
        }
    }
}

static void generateSlidingPieceMoves(const Board* board, MoveList* list, bool isWhite, Bitboard pieces, PieceTypeToken pieceType) {
    Bitboard friendlyPieces = getOccupiedByColor(board, isWhite);
    Bitboard enemyPieces = getOccupiedByColor(board, !isWhite);
    Bitboard allPieces = friendlyPieces | enemyPieces;
    Bitboard currentPieces = pieces;

    while (currentPieces) {
        Square fromSq = BIT_SCAN_FORWARD(currentPieces);
        CLEAR_BIT(currentPieces, fromSq);
        Bitboard attacks;
        if (pieceType == ROOK_T) attacks = getRookAttacks(fromSq, allPieces);
        else if (pieceType == BISHOP_T) attacks = getBishopAttacks(fromSq, allPieces);
        else /* QUEEN_T */ attacks = getQueenAttacks(fromSq, allPieces);
        
        Bitboard validMoves = attacks & ~friendlyPieces;
        while (validMoves) {
            Square toSq = BIT_SCAN_FORWARD(validMoves);
            CLEAR_BIT(validMoves, toSq);
            addMove(list, CREATE_MOVE(fromSq, toSq, 0, GET_BIT(enemyPieces, toSq),0,0,0));
        }
    }
}

// Helper function to check if squares are attacked
// Parameter 'byWhite' means "is the attacker white?"
static bool isSquareAttacked(const Board* board, Square sq, bool byWhite) {
    Bitboard targetSqBit = 1ULL << sq;

    Bitboard occupiedWhite = getOccupiedByColor(board, true);
    Bitboard occupiedBlack = getOccupiedByColor(board, false);
    Bitboard allPieces = occupiedWhite | occupiedBlack;

    if (byWhite) { // Check attacks by white pieces
        // White pawns: PAWN_ATTACKS[1][sq] gives squares from which a black pawn would attack sq.
        // So, if a white pawn is on one of those squares, it attacks sq.
        if (PAWN_ATTACKS[1][sq] & board->whitePawns) return true;
        if (KNIGHT_ATTACKS[sq] & board->whiteKnights) return true;
        if (KING_ATTACKS[sq] & board->whiteKings) return true;

        // Sliding pieces: Iterate over opponent's pieces and see if they attack 'sq'
        Bitboard attackers;
        // White Rooks
        attackers = board->whiteRooks;
        while(attackers) {
            Square attackerSq = pop_lsb(&attackers);
            if (getRookAttacks(attackerSq, allPieces) & targetSqBit) return true;
        }
        // White Bishops
        attackers = board->whiteBishops;
        while(attackers) {
            Square attackerSq = pop_lsb(&attackers);
            if (getBishopAttacks(attackerSq, allPieces) & targetSqBit) return true;
        }
        // White Queens
        attackers = board->whiteQueens;
        while(attackers) {
            Square attackerSq = pop_lsb(&attackers);
            if (getQueenAttacks(attackerSq, allPieces) & targetSqBit) return true;
        }

    } else { // Check attacks by black pieces
        // Black pawns: PAWN_ATTACKS[0][sq] gives squares from which a white pawn would attack sq.
        // So, if a black pawn is on one of those squares, it attacks sq.
        if (PAWN_ATTACKS[0][sq] & board->blackPawns) return true;
        if (KNIGHT_ATTACKS[sq] & board->blackKnights) return true;
        if (KING_ATTACKS[sq] & board->blackKings) return true;

        // Sliding pieces: Iterate over opponent's pieces and see if they attack 'sq'
        Bitboard attackers;
        // Black Rooks
        attackers = board->blackRooks;
        while(attackers) {
            Square attackerSq = pop_lsb(&attackers);
            if (getRookAttacks(attackerSq, allPieces) & targetSqBit) return true;
        }
        // Black Bishops
        attackers = board->blackBishops;
        while(attackers) {
            Square attackerSq = pop_lsb(&attackers);
            if (getBishopAttacks(attackerSq, allPieces) & targetSqBit) return true;
        }
        // Black Queens
        attackers = board->blackQueens;
        while(attackers) {
            Square attackerSq = pop_lsb(&attackers);
            if (getQueenAttacks(attackerSq, allPieces) & targetSqBit) return true;
        }
    }

    return false; // Square is not attacked by the specified side
}

static void generateCastlingMoves(const Board* board, MoveList* list, bool isWhite) {
    Bitboard occupied = board->whitePawns | board->whiteKnights | board->whiteBishops | board->whiteRooks | board->whiteQueens | board->whiteKings |
                         board->blackPawns | board->blackKnights | board->blackBishops | board->blackRooks | board->blackQueens | board->blackKings; // Ensure occupied is complete here too

    if (isWhite) {
        // White Kingside Castle (K)
        if ((board->castlingRights & WHITE_KINGSIDE_CASTLE) &&
            !(occupied & ( (1ULL << SQ_F1) | (1ULL << SQ_G1) )) && // Squares between king and rook are empty
            !isSquareAttacked(board, SQ_E1, false) &&
            !isSquareAttacked(board, SQ_F1, false) &&
            !isSquareAttacked(board, SQ_G1, false)) {
            addMove(list, CREATE_MOVE(SQ_E1, SQ_G1, 0, 0, 0, 0, 1)); // Add castling flag
        }
        // White Queenside Castle (Q)
        if ((board->castlingRights & WHITE_QUEENSIDE_CASTLE) &&
            !(occupied & ( (1ULL << SQ_D1) | (1ULL << SQ_C1) | (1ULL << SQ_B1) )) && // Squares between king and rook are empty
            !isSquareAttacked(board, SQ_E1, false) &&
            !isSquareAttacked(board, SQ_D1, false) &&
            !isSquareAttacked(board, SQ_C1, false)) {
            addMove(list, CREATE_MOVE(SQ_E1, SQ_C1, 0, 0, 0, 0, 1)); // Add castling flag
        }
    } else {
        // Black Kingside Castle (k)
        if ((board->castlingRights & BLACK_KINGSIDE_CASTLE) &&
            !(occupied & ( (1ULL << SQ_F8) | (1ULL << SQ_G8) )) && // Squares between king and rook are empty
            !isSquareAttacked(board, SQ_E8, true) &&
            !isSquareAttacked(board, SQ_F8, true) &&
            !isSquareAttacked(board, SQ_G8, true)) {
            addMove(list, CREATE_MOVE(SQ_E8, SQ_G8, 0, 0, 0, 0, 1)); // Add castling flag
        }
        // Black Queenside Castle (q)
        if ((board->castlingRights & BLACK_QUEENSIDE_CASTLE) &&
            !(occupied & ( (1ULL << SQ_D8) | (1ULL << SQ_C8) | (1ULL << SQ_B8) )) && // Squares between king and rook are empty
            !isSquareAttacked(board, SQ_E8, true) &&
            !isSquareAttacked(board, SQ_D8, true) &&
            !isSquareAttacked(board, SQ_C8, true)) {
            addMove(list, CREATE_MOVE(SQ_E8, SQ_C8, 0, 0, 0, 0, 1)); // Add castling flag
        }
    }
}

void generateMoves(const Board* board, MoveList* list) {
    MoveList pseudoLegalMoves;
    pseudoLegalMoves.count = 0;
    list->count = 0; // Initialize final list count

    bool isWhite = board->whiteToMove;

    // Generate all pseudo-legal moves into pseudoLegalMoves
    generatePawnMoves(board, &pseudoLegalMoves, isWhite);
    generatePieceMoves(board, &pseudoLegalMoves, isWhite, isWhite ? board->whiteKnights : board->blackKnights, KNIGHT_ATTACKS);
    generatePieceMoves(board, &pseudoLegalMoves, isWhite, isWhite ? board->whiteKings : board->blackKings, KING_ATTACKS);
    generateSlidingPieceMoves(board, &pseudoLegalMoves, isWhite, isWhite ? board->whiteBishops : board->blackBishops, BISHOP_T);
    generateSlidingPieceMoves(board, &pseudoLegalMoves, isWhite, isWhite ? board->whiteRooks : board->blackRooks, ROOK_T);
    generateSlidingPieceMoves(board, &pseudoLegalMoves, isWhite, isWhite ? board->whiteQueens : board->blackQueens, QUEEN_T);
    generateCastlingMoves(board, &pseudoLegalMoves, isWhite);

    list->count = 0; // Reset the original list to store only legal moves

    for (int i = 0; i < pseudoLegalMoves.count; i++) {
        Move currentMove = pseudoLegalMoves.moves[i];
        Board tempBoard = *board; // Create a copy of the board to simulate the move

        Square from = MOVE_FROM(currentMove);
        Square to = MOVE_TO(currentMove);
        Bitboard fromBit = 1ULL << from;
        Bitboard toBit = 1ULL << to;
        int promotionPieceType = MOVE_PROMOTION(currentMove);

        Bitboard* movingPieceBitboard = getMutablePieceBitboardAtSquare(&tempBoard, from, isWhite);

        if (!movingPieceBitboard) {
            // Should not happen if pseudo-legal generation is correct and 'from' has a piece of current player's color
            continue; 
        }

        // 1. Handle captures (must be done before moving the piece to 'to')
        if (MOVE_IS_CAPTURE(currentMove)) {
            if (MOVE_IS_EN_PASSANT(currentMove)) {
                Square capturedPawnSq;
                if (isWhite) { // White makes en passant capture, black pawn is captured
                    capturedPawnSq = to - 8; // Black pawn was one square behind 'to'
                    tempBoard.blackPawns &= ~(1ULL << capturedPawnSq);
                } else { // Black makes en passant capture, white pawn is captured
                    capturedPawnSq = to + 8; // White pawn was one square behind 'to'
                    tempBoard.whitePawns &= ~(1ULL << capturedPawnSq);
                }
            } else {
                // Regular capture: remove whatever piece is on the 'to' square
                // Note: clearSquareOnAllBitboards clears from ALL bitboards.
                // This is okay as only one piece (opponent's) should be there.
                clearSquareOnAllBitboards(&tempBoard, to);
            }
        }

        // 2. Move the piece: clear 'from', set 'to'
        *movingPieceBitboard &= ~fromBit; // Clear piece from 'from' square

        if (promotionPieceType != 0) { // Handle promotion
            // The pawn is removed from its bitboard above. Now add the new piece.
            if (isWhite) {
                if (promotionPieceType == PROMOTION_N) tempBoard.whiteKnights |= toBit;
                else if (promotionPieceType == PROMOTION_B) tempBoard.whiteBishops |= toBit;
                else if (promotionPieceType == PROMOTION_R) tempBoard.whiteRooks |= toBit;
                else if (promotionPieceType == PROMOTION_Q) tempBoard.whiteQueens |= toBit;
            } else { // Black promotes
                if (promotionPieceType == PROMOTION_N) tempBoard.blackKnights |= toBit;
                else if (promotionPieceType == PROMOTION_B) tempBoard.blackBishops |= toBit;
                else if (promotionPieceType == PROMOTION_R) tempBoard.blackRooks |= toBit;
                else if (promotionPieceType == PROMOTION_Q) tempBoard.blackQueens |= toBit;
            }
        } else {
            // No promotion, just move the piece to the 'to' square in its original bitboard
            *movingPieceBitboard |= toBit;
        }

        // 3. Special handling for castling (rook movement)
        if (MOVE_IS_CASTLING(currentMove)) {
            // King's part of the move (e.g., E1->G1) is handled by the general logic above
            // as movingPieceBitboard would point to the king's bitboard.
            // Now, move the rook.
            if (to == SQ_G1) { // White kingside (King E1->G1, Rook H1->F1)
                tempBoard.whiteRooks &= ~(1ULL << SQ_H1); tempBoard.whiteRooks |= (1ULL << SQ_F1);
            } else if (to == SQ_C1) { // White queenside (King E1->C1, Rook A1->D1)
                tempBoard.whiteRooks &= ~(1ULL << SQ_A1); tempBoard.whiteRooks |= (1ULL << SQ_D1);
            } else if (to == SQ_G8) { // Black kingside (King E8->G8, Rook H8->F8)
                tempBoard.blackRooks &= ~(1ULL << SQ_H8); tempBoard.blackRooks |= (1ULL << SQ_F8);
            } else if (to == SQ_C8) { // Black queenside (King E8->C8, Rook A8->D8)
                tempBoard.blackRooks &= ~(1ULL << SQ_A8); tempBoard.blackRooks |= (1ULL << SQ_D8);
            }
        }
        
        // 4. Find the current player's king on the temporary board
        Bitboard kingPosBitboard = isWhite ? tempBoard.whiteKings : tempBoard.blackKings;
        Square kingSq = get_lsb_index(kingPosBitboard);

        if (kingSq == SQ_NONE) { // King not found (e.g., captured itself - illegal)
            continue; 
        }

        // 5. Update the board state: toggle turn
        tempBoard.whiteToMove = !isWhite; // Toggle turn for the next player
        // Note: Castling rights and en passant square are not updated here.

        // 5. Check if the king is attacked by the opponent
        // If current player is white (isWhite=true), check attacks by black (byWhite=false)
        // If current player is black (isWhite=false), check attacks by white (byWhite=true)
        // So, the 'byWhite' argument for isSquareAttacked should be !isWhite.
        // if (!isSquareAttacked(&tempBoard, kingSq, isWhite)) { // THIS WAS THE ORIGINAL LINE causing issues with previous user input
        if (!isSquareAttacked(&tempBoard, kingSq, !isWhite)) { // CORRECTED: check attacks by the OPPONENT
            addMove(list, currentMove); // If king is not in check, the move is legal
        } else {
            /*char moveStr[16];
            moveToString(currentMove, moveStr);
            char kingSqStr[3];
            squareToString(kingSq, kingSqStr);
            const char* fen_after_move = outputFEN(&tempBoard); // Requires board_io.h
            printf("DEBUG: Move %s (King on %s) puts king in check. Skipping. FEN after move: %s\n", moveStr, kingSqStr, fen_after_move);
            fflush(stdout);*/
        }
    }
}