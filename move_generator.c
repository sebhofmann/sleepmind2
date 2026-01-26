#include "move_generator.h"
#include "bitboard_utils.h" // For POPCOUNT and other bit utilities
#include "board_modifiers.h" // For Board struct, PieceTypeToken, Square
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
// Now uses O(1) piece array lookup instead of scanning bitboards
__attribute__((unused))
static Bitboard* getMutablePieceBitboardAtSquare(Board* board, Square sq, bool isWhiteMoving) {
    uint8_t p = board->piece[sq];
    if (p == NO_PIECE) return NULL;
    int color = PIECE_COLOR_OF(p);
    // Verify color matches expectation
    if ((color == WHITE) != isWhiteMoving) return NULL;
    int type = PIECE_TYPE_OF(p);
    return &board->byTypeBB[color][type];
}

// Helper: Remove any piece from a square - now O(1) using piece array
__attribute__((unused))
static void clearSquareOnAllBitboards(Board* board, Square sq) {
    uint8_t p = board->piece[sq];
    if (p == NO_PIECE) return;
    int color = PIECE_COLOR_OF(p);
    int type = PIECE_TYPE_OF(p);
    board->byTypeBB[color][type] &= ~(1ULL << sq);
    board->piece[sq] = NO_PIECE;
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
    // Use rand() from stdlib; mask pieces to build a 64-bit value
    u1 = (Bitboard)(rand()) & 0xFFFFULL;
    u2 = (Bitboard)(rand()) & 0xFFFFULL;
    u3 = (Bitboard)(rand()) & 0xFFFFULL;
    u4 = (Bitboard)(rand()) & 0xFFFFULL;
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
    srand((unsigned int)time(NULL)); // Seed rand()
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
    int c = isWhite ? WHITE : BLACK;
    return board->byTypeBB[c][PAWN] | board->byTypeBB[c][KNIGHT] | board->byTypeBB[c][BISHOP] | 
           board->byTypeBB[c][ROOK] | board->byTypeBB[c][QUEEN] | board->byTypeBB[c][KING];
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
        if (board->enPassantSquare != SQ_NONE) {
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
    // Bitboard targetSqBit = 1ULL << sq; // Not needed for sliding optimization

    Bitboard occupiedWhite = getOccupiedByColor(board, true);
    Bitboard occupiedBlack = getOccupiedByColor(board, false);
    Bitboard allPieces = occupiedWhite | occupiedBlack;

    if (byWhite) { // Check attacks by white pieces
        // White pawns
        if (PAWN_ATTACKS[1][sq] & board->whitePawns) return true;
        // White Knights
        if (KNIGHT_ATTACKS[sq] & board->whiteKnights) return true;
        // White King
        if (KING_ATTACKS[sq] & board->whiteKings) return true;

        // White Sliding pieces (Optimized)
        if (getRookAttacks(sq, allPieces) & (board->whiteRooks | board->whiteQueens)) return true;
        if (getBishopAttacks(sq, allPieces) & (board->whiteBishops | board->whiteQueens)) return true;

    } else { // Check attacks by black pieces
        // Black pawns
        if (PAWN_ATTACKS[0][sq] & board->blackPawns) return true;
        // Black Knights
        if (KNIGHT_ATTACKS[sq] & board->blackKnights) return true;
        // Black King
        if (KING_ATTACKS[sq] & board->blackKings) return true;

        // Black Sliding pieces (Optimized)
        if (getRookAttacks(sq, allPieces) & (board->blackRooks | board->blackQueens)) return true;
        if (getBishopAttacks(sq, allPieces) & (board->blackBishops | board->blackQueens)) return true;
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

bool isKingAttacked(const Board* board, bool kingColor) {
    Bitboard kingPosBitboard = kingColor ? board->whiteKings : board->blackKings;
    Square kingSq = get_lsb_index(kingPosBitboard);

    if (kingSq == SQ_NONE) { 
        return false; 
    }
    return isSquareAttacked(board, kingSq, !kingColor);
}

// Generate only pseudo-legal CAPTURE moves for the current player
void generateCaptureMoves(const Board* board, MoveList* list) {
    list->count = 0; // Initialize list
    bool isWhite = board->whiteToMove;

    Bitboard friendlyPieces = getOccupiedByColor(board, isWhite);
    Bitboard enemyPieces = getOccupiedByColor(board, !isWhite);
    Bitboard allPieces = friendlyPieces | enemyPieces;

    // 1. Pawn Captures (including en-passant, excluding promotions for this specific function)
    Bitboard pawns = isWhite ? board->whitePawns : board->blackPawns;
    int direction = isWhite ? 1 : -1;
    int promotionRank = isWhite ? 7 : 0;
    Bitboard currentPawns = pawns;
    while (currentPawns) {
        Square fromSq = BIT_SCAN_FORWARD(currentPawns);
        CLEAR_BIT(currentPawns, fromSq);
        int rank = fromSq / 8;

        Bitboard pawnAttacks = PAWN_ATTACKS[isWhite ? 0 : 1][fromSq];
        Bitboard validCaptures = pawnAttacks & enemyPieces;
        while (validCaptures) {
            Square toSq_capture = BIT_SCAN_FORWARD(validCaptures);
            CLEAR_BIT(validCaptures, toSq_capture);
            // Exclude promotions if this function is strictly for non-promoting captures
            if (rank + direction != promotionRank) { 
                addMove(list, CREATE_MOVE(fromSq, toSq_capture, 0, 1, 0, 0, 0));
            }
        }
        if (board->enPassantSquare != SQ_NONE) {
            if (GET_BIT(pawnAttacks, board->enPassantSquare)) {
                 addMove(list, CREATE_MOVE(fromSq, board->enPassantSquare, 0, 1, 0, 1, 0));
            }
        }
    }

    // 2. Knight Captures
    Bitboard knights = isWhite ? board->whiteKnights : board->blackKnights;
    Bitboard currentKnights = knights;
    while (currentKnights) {
        Square fromSq = BIT_SCAN_FORWARD(currentKnights);
        CLEAR_BIT(currentKnights, fromSq);
        Bitboard attacks = KNIGHT_ATTACKS[fromSq];
        Bitboard validCapturesOnEnemy = attacks & enemyPieces;
        while (validCapturesOnEnemy) {
            Square toSq = BIT_SCAN_FORWARD(validCapturesOnEnemy);
            CLEAR_BIT(validCapturesOnEnemy, toSq);
            addMove(list, CREATE_MOVE(fromSq, toSq, 0, 1, 0, 0, 0));
        }
    }

    // 3. King Captures (Kings cannot castle and capture simultaneously)
    Bitboard king = isWhite ? board->whiteKings : board->blackKings;
    if (king) { // Should always be true unless king is captured (which means game over)
        Square fromSq = BIT_SCAN_FORWARD(king);
        Bitboard attacks = KING_ATTACKS[fromSq];
        Bitboard validCapturesOnEnemy = attacks & enemyPieces;
        while (validCapturesOnEnemy) {
            Square toSq = BIT_SCAN_FORWARD(validCapturesOnEnemy);
            CLEAR_BIT(validCapturesOnEnemy, toSq);
            addMove(list, CREATE_MOVE(fromSq, toSq, 0, 1, 0, 0, 0));
        }
    }

    // 4. Sliding Piece Captures (Bishops, Rooks, Queens)
    PieceTypeToken slidingPieceTypes[] = {BISHOP_T, ROOK_T, QUEEN_T};
    for (int i = 0; i < 3; ++i) {
        PieceTypeToken pieceType = slidingPieceTypes[i];
        Bitboard pieces;
        if (isWhite) {
            if (pieceType == BISHOP_T) pieces = board->whiteBishops;
            else if (pieceType == ROOK_T) pieces = board->whiteRooks;
            else pieces = board->whiteQueens;
        } else {
            if (pieceType == BISHOP_T) pieces = board->blackBishops;
            else if (pieceType == ROOK_T) pieces = board->blackRooks;
            else pieces = board->blackQueens;
        }

        Bitboard currentSlidingPieces = pieces;
        while (currentSlidingPieces) {
            Square fromSq = BIT_SCAN_FORWARD(currentSlidingPieces);
            CLEAR_BIT(currentSlidingPieces, fromSq);
            Bitboard attacks;
            if (pieceType == ROOK_T) attacks = getRookAttacks(fromSq, allPieces);
            else if (pieceType == BISHOP_T) attacks = getBishopAttacks(fromSq, allPieces);
            else /* QUEEN_T */ attacks = getQueenAttacks(fromSq, allPieces);
            
            Bitboard validCapturesOnEnemy = attacks & enemyPieces;
            while (validCapturesOnEnemy) {
                Square toSq = BIT_SCAN_FORWARD(validCapturesOnEnemy);
                CLEAR_BIT(validCapturesOnEnemy, toSq);
                addMove(list, CREATE_MOVE(fromSq, toSq, 0, 1, 0, 0, 0));
            }
        }
    }
}

// Generate only pseudo-legal CAPTURE and PROMOTION moves for the current player
void generateCaptureAndPromotionMoves(const Board* board, MoveList* list) {
    list->count = 0; // Initialize list
    bool isWhite = board->whiteToMove;

    Bitboard friendlyPieces = getOccupiedByColor(board, isWhite);
    Bitboard enemyPieces = getOccupiedByColor(board, !isWhite);
    Bitboard allPieces = friendlyPieces | enemyPieces;

    // 1. Pawn Moves (Captures, Promotions, Capture-Promotions)
    Bitboard pawns = isWhite ? board->whitePawns : board->blackPawns;
    int direction = isWhite ? 1 : -1;
    int promotionRank = isWhite ? 7 : 0;
    Bitboard currentPawns = pawns;
    while (currentPawns) {
        Square fromSq = BIT_SCAN_FORWARD(currentPawns);
        CLEAR_BIT(currentPawns, fromSq);
        int rank = fromSq / 8;

        // Pawn Pushes resulting in Promotion (no capture)
        Square toSq_single_push = fromSq + 8 * direction;
        if (rank + direction == promotionRank && toSq_single_push >=0 && toSq_single_push < 64 && !GET_BIT(allPieces, toSq_single_push)) {
            addMove(list, CREATE_MOVE(fromSq, toSq_single_push, PROMOTION_Q, 0,0,0,0));
            addMove(list, CREATE_MOVE(fromSq, toSq_single_push, PROMOTION_R, 0,0,0,0));
            addMove(list, CREATE_MOVE(fromSq, toSq_single_push, PROMOTION_B, 0,0,0,0));
            addMove(list, CREATE_MOVE(fromSq, toSq_single_push, PROMOTION_N, 0,0,0,0));
        }

        // Pawn Captures (including those that result in promotion)
        Bitboard pawnAttacks = PAWN_ATTACKS[isWhite ? 0 : 1][fromSq];
        Bitboard validPawnAttackTargets = pawnAttacks & enemyPieces;
        while (validPawnAttackTargets) {
            Square toSq_capture = BIT_SCAN_FORWARD(validPawnAttackTargets);
            CLEAR_BIT(validPawnAttackTargets, toSq_capture);
            if (rank + direction == promotionRank) {
                addMove(list, CREATE_MOVE(fromSq, toSq_capture, PROMOTION_Q, 1,0,0,0));
                addMove(list, CREATE_MOVE(fromSq, toSq_capture, PROMOTION_R, 1,0,0,0));
                addMove(list, CREATE_MOVE(fromSq, toSq_capture, PROMOTION_B, 1,0,0,0));
                addMove(list, CREATE_MOVE(fromSq, toSq_capture, PROMOTION_N, 1,0,0,0));
            } else {
                addMove(list, CREATE_MOVE(fromSq, toSq_capture, 0,1,0,0,0));
            }
        }
        
        // En Passant (is a capture, cannot be a promotion)
        if (board->enPassantSquare != SQ_NONE) {
            if (GET_BIT(pawnAttacks, board->enPassantSquare)) {
                 addMove(list, CREATE_MOVE(fromSq, board->enPassantSquare, 0,1,0,1,0));
            }
        }
    }

    // 2. Knight Captures (Knights cannot promote)
    Bitboard knights = isWhite ? board->whiteKnights : board->blackKnights;
    Bitboard currentKnights = knights;
    while (currentKnights) {
        Square fromSq = BIT_SCAN_FORWARD(currentKnights);
        CLEAR_BIT(currentKnights, fromSq);
        Bitboard attacks = KNIGHT_ATTACKS[fromSq];
        Bitboard validCapturesOnEnemy = attacks & enemyPieces;
        while (validCapturesOnEnemy) {
            Square toSq = BIT_SCAN_FORWARD(validCapturesOnEnemy);
            CLEAR_BIT(validCapturesOnEnemy, toSq);
            addMove(list, CREATE_MOVE(fromSq, toSq, 0, 1, 0, 0, 0));
        }
    }

    // 3. King Captures (Kings cannot promote or castle-capture)
    Bitboard king = isWhite ? board->whiteKings : board->blackKings;
    if (king) { 
        Square fromSq = BIT_SCAN_FORWARD(king);
        Bitboard attacks = KING_ATTACKS[fromSq];
        Bitboard validCapturesOnEnemy = attacks & enemyPieces;
        while (validCapturesOnEnemy) {
            Square toSq = BIT_SCAN_FORWARD(validCapturesOnEnemy);
            CLEAR_BIT(validCapturesOnEnemy, toSq);
            addMove(list, CREATE_MOVE(fromSq, toSq, 0, 1, 0, 0, 0));
        }
    }

    // 4. Sliding Piece Captures (Bishops, Rooks, Queens - cannot promote)
    PieceTypeToken slidingPieceTypes[] = {BISHOP_T, ROOK_T, QUEEN_T};
    for (int i = 0; i < 3; ++i) {
        PieceTypeToken pieceType = slidingPieceTypes[i];
        Bitboard pieces;
        if (isWhite) {
            if (pieceType == BISHOP_T) pieces = board->whiteBishops;
            else if (pieceType == ROOK_T) pieces = board->whiteRooks;
            else pieces = board->whiteQueens;
        } else {
            if (pieceType == BISHOP_T) pieces = board->blackBishops;
            else if (pieceType == ROOK_T) pieces = board->blackRooks;
            else pieces = board->blackQueens;
        }

        Bitboard currentSlidingPieces = pieces;
        while (currentSlidingPieces) {
            Square fromSq = BIT_SCAN_FORWARD(currentSlidingPieces);
            CLEAR_BIT(currentSlidingPieces, fromSq);
            Bitboard attacks;
            if (pieceType == ROOK_T) attacks = getRookAttacks(fromSq, allPieces);
            else if (pieceType == BISHOP_T) attacks = getBishopAttacks(fromSq, allPieces);
            else /* QUEEN_T */ attacks = getQueenAttacks(fromSq, allPieces);
            
            Bitboard validCapturesOnEnemy = attacks & enemyPieces;
            while (validCapturesOnEnemy) {
                Square toSq = BIT_SCAN_FORWARD(validCapturesOnEnemy);
                CLEAR_BIT(validCapturesOnEnemy, toSq);
                addMove(list, CREATE_MOVE(fromSq, toSq, 0, 1, 0, 0, 0));
            }
        }
    }
    // Note: This function does not check for legality (king in check after move).
    // That should be handled by the caller or a subsequent filtering step if needed.
}


// Generate all pseudo-legal moves (no legality check - king may be left in check)
// Legality check is deferred to search/perft for better performance
void generateMoves(const Board* board, MoveList* list) {
    list->count = 0;
    bool isWhite = board->whiteToMove;

    // Generate all pseudo-legal moves directly into the output list
    generatePawnMoves(board, list, isWhite);
    generatePieceMoves(board, list, isWhite, isWhite ? board->whiteKnights : board->blackKnights, KNIGHT_ATTACKS);
    generatePieceMoves(board, list, isWhite, isWhite ? board->whiteKings : board->blackKings, KING_ATTACKS);
    generateSlidingPieceMoves(board, list, isWhite, isWhite ? board->whiteBishops : board->blackBishops, BISHOP_T);
    generateSlidingPieceMoves(board, list, isWhite, isWhite ? board->whiteRooks : board->blackRooks, ROOK_T);
    generateSlidingPieceMoves(board, list, isWhite, isWhite ? board->whiteQueens : board->blackQueens, QUEEN_T);
    generateCastlingMoves(board, list, isWhite);
}

// Generate all legal moves (filters out moves that leave king in check)
// Use this when you need guaranteed legal moves (e.g., training data generation)
void generateLegalMoves(Board* board, MoveList* list) {
    MoveList pseudoLegalMoves;
    pseudoLegalMoves.count = 0;
    list->count = 0;

    bool isWhite = board->whiteToMove;

    // Generate all pseudo-legal moves
    generatePawnMoves(board, &pseudoLegalMoves, isWhite);
    generatePieceMoves(board, &pseudoLegalMoves, isWhite, isWhite ? board->whiteKnights : board->blackKnights, KNIGHT_ATTACKS);
    generatePieceMoves(board, &pseudoLegalMoves, isWhite, isWhite ? board->whiteKings : board->blackKings, KING_ATTACKS);
    generateSlidingPieceMoves(board, &pseudoLegalMoves, isWhite, isWhite ? board->whiteBishops : board->blackBishops, BISHOP_T);
    generateSlidingPieceMoves(board, &pseudoLegalMoves, isWhite, isWhite ? board->whiteRooks : board->blackRooks, ROOK_T);
    generateSlidingPieceMoves(board, &pseudoLegalMoves, isWhite, isWhite ? board->whiteQueens : board->blackQueens, QUEEN_T);
    generateCastlingMoves(board, &pseudoLegalMoves, isWhite);

    // Filter for legality
    for (int i = 0; i < pseudoLegalMoves.count; i++) {
        Move currentMove = pseudoLegalMoves.moves[i];
        
        MoveUndoInfo undo_info;
        applyMove(board, currentMove, &undo_info, NULL, NULL); 
            
        // Check if the king of the side that just moved is in check
        if (!isKingAttacked(board, !board->whiteToMove)) { 
            addMove(list, currentMove); 
        }
        
        undoMove(board, currentMove, &undo_info, NULL, NULL);
    }
}