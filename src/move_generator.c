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
// Magic multipliers were found offline (random search over sparse candidates). Fixed constants keep
// startup instant and deterministic; findAndInitMagicNumbers() verifies them while filling the attack tables.
// Build with RUNTIME_MAGICS=1 (make runtime_magics=1) to search for the magics at startup instead.
#ifdef RUNTIME_MAGICS
static Bitboard ROOK_MAGICS[64];
static Bitboard BISHOP_MAGICS[64];
#else
static const Bitboard ROOK_MAGICS[64] = {
    0x0080002882400210ULL, 0x0140002000100040ULL, 0x3880082000821000ULL, 0x1600045200204008ULL,
    0x5100028500580010ULL, 0x420008020001104cULL, 0x0100240200008100ULL, 0x0080002141000180ULL,
    0x4000800520400290ULL, 0x2410802004c00080ULL, 0x0022002090804201ULL, 0x8041001000082101ULL,
    0x2001808064000800ULL, 0x00c2000200500804ULL, 0x0600800a00800100ULL, 0x0100800880004100ULL,
    0x0002808002400060ULL, 0x0040404004201002ULL, 0x1410008020023880ULL, 0x1108420020114a00ULL,
    0x8424808004000800ULL, 0x2500808004000200ULL, 0x020944000e900908ULL, 0x20000e000440840dULL,
    0x0880005040042000ULL, 0xa010014040002001ULL, 0x2000110100200142ULL, 0x80360012004008a0ULL,
    0x1000080080140080ULL, 0x2008404801042010ULL, 0x490200020001080cULL, 0x0002048600040549ULL,
    0x0104c00280800024ULL, 0x0100400280802008ULL, 0x0000842000801000ULL, 0x0408080080801000ULL,
    0x0010080101000410ULL, 0x4000800201800400ULL, 0x1000011604001810ULL, 0xf40100006900058aULL,
    0x0002204018808000ULL, 0x4004201004414000ULL, 0x1010102082020040ULL, 0x00020040100a0020ULL,
    0x0408000400808048ULL, 0x0001040002008080ULL, 0x0101001200c10024ULL, 0x80220100824a0004ULL,
    0x3201804211082200ULL, 0x2052200440100840ULL, 0x20141000a0008c80ULL, 0x8c06080010028180ULL,
    0x8050080180640080ULL, 0x2082010488100200ULL, 0x0005b00211084400ULL, 0x00003040830c0200ULL,
    0x8020201042800101ULL, 0x32a0400094810021ULL, 0x802100440a200011ULL, 0x2008100104200901ULL,
    0x8101000258001491ULL, 0x810100484e040005ULL, 0x0111d10210086084ULL, 0x00010100c0840022ULL,
};

static const Bitboard BISHOP_MAGICS[64] = {
    0x0042204802008022ULL, 0x0084040084010000ULL, 0x0010048a00444001ULL, 0x0004410060000000ULL,
    0x0002021002000512ULL, 0x0002080288010000ULL, 0x000a221002291001ULL, 0x0020840841102800ULL,
    0x0009902048008081ULL, 0x301b021002020140ULL, 0x2f00089801002040ULL, 0x5000040401801401ULL,
    0x83080405a0000001ULL, 0x000102080c050040ULL, 0x0980008208208408ULL, 0x3624220044440484ULL,
    0x028820102018c482ULL, 0x210400b10102040cULL, 0x1018004c12240050ULL, 0x0c0c0050c4048031ULL,
    0x0000840400a01100ULL, 0x2002000888140204ULL, 0x0000400a12022028ULL, 0x6809840042049000ULL,
    0x1088090c20204300ULL, 0x008450100a100102ULL, 0x0008480430202040ULL, 0x0a30240000401020ULL,
    0x00c0840008802028ULL, 0x013001020a104200ULL, 0x00184c8101008800ULL, 0x5044024180210420ULL,
    0x8002084000041000ULL, 0x8001082001890102ULL, 0x0084024104080200ULL, 0x6401b108000c01c0ULL,
    0x8168020400001100ULL, 0x20008824400a0100ULL, 0x0002084100060083ULL, 0x3008010220450086ULL,
    0x80020211400104a0ULL, 0x024100900400b002ULL, 0x0202309088013001ULL, 0x0000002038000301ULL,
    0x4001a44102100402ULL, 0x2e01411101000200ULL, 0x24210825010000c0ULL, 0x0008084044402088ULL,
    0x044200c404c000a0ULL, 0x0880410090304028ULL, 0x1040090409240182ULL, 0x03000210208805d0ULL,
    0x80011088470c0100ULL, 0x0000400204810880ULL, 0x0214200206020100ULL, 0x0a18900400404003ULL,
    0x04020a01420a1004ULL, 0x0104034048280802ULL, 0x084000004e080440ULL, 0x0000004481084800ULL,
    0x0045040010e20600ULL, 0x0001148408500100ULL, 0xd080a04490008100ULL, 0x034008830c008034ULL,
};
#endif

MagicEntry g_rook_magics[64];
MagicEntry g_bishop_magics[64];

// Backing storage for all attack tables (one contiguous block per piece type)
static Bitboard* ROOK_ATTACK_STORAGE = NULL;
static Bitboard* BISHOP_ATTACK_STORAGE = NULL;

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

static bool init_magic_table(MagicEntry* entries, const Bitboard* magics, Bitboard** storage, bool is_rook) {
    size_t total = 0;
    for (Square sq = 0; sq < 64; sq++) {
        Bitboard mask = is_rook ? generate_rook_mask_user(sq) : generate_bishop_mask_user(sq);
        total += (size_t)1 << POPCOUNT(mask);
    }
    free(*storage);
    *storage = (Bitboard*)malloc(total * sizeof(Bitboard));
    unsigned char* used = (unsigned char*)malloc(total);
    if (!*storage || !used) {
        free(used);
        return false;
    }
    memset(used, 0, total);

    size_t offset = 0;
    bool ok = true;
    for (Square sq = 0; sq < 64; sq++) {
        MagicEntry* e = &entries[sq];
        e->mask = is_rook ? generate_rook_mask_user(sq) : generate_bishop_mask_user(sq);
        e->magic = magics[sq];
        int bits = POPCOUNT(e->mask);
        e->shift = 64 - bits;
        e->attacks = *storage + offset;

        for (int i = 0; i < (1 << bits); i++) {
            Bitboard occ = index_to_occupancy(i, bits, e->mask);
            Bitboard att = is_rook ? generate_rook_attacks_otf_user(sq, occ) : generate_bishop_attacks_otf_user(sq, occ);
            size_t idx = (size_t)((occ * e->magic) >> e->shift);
            if (used[offset + idx] && e->attacks[idx] != att) ok = false;
            e->attacks[idx] = att;
            used[offset + idx] = 1;
        }
        offset += (size_t)1 << bits;
    }
    free(used);
    return ok;
}

#ifdef RUNTIME_MAGICS
// Random bitboard generation
static Bitboard random_u64() {
    Bitboard u1 = (Bitboard)(rand()) & 0xFFFFULL;
    Bitboard u2 = (Bitboard)(rand()) & 0xFFFFULL;
    Bitboard u3 = (Bitboard)(rand()) & 0xFFFFULL;
    Bitboard u4 = (Bitboard)(rand()) & 0xFFFFULL;
    return u1 | (u2 << 16) | (u3 << 32) | (u4 << 48);
}

static Bitboard random_u64_fewbits() {
    return random_u64() & random_u64() & random_u64();
}

// Random search for a collision-free magic multiplier for one square
static bool find_magic_for_square(Square sq, bool is_rook, int max_attempts, Bitboard* magic_out) {
    Bitboard mask = is_rook ? generate_rook_mask_user(sq) : generate_bishop_mask_user(sq);
    int bits = POPCOUNT(mask);
    int states = 1 << bits;
    Bitboard* occupancies = (Bitboard*)malloc(states * sizeof(Bitboard));
    Bitboard* attacks = (Bitboard*)malloc(states * sizeof(Bitboard));
    Bitboard* table = (Bitboard*)malloc(states * sizeof(Bitboard));
    unsigned char* used = (unsigned char*)malloc(states);
    bool found = false;

    if (occupancies && attacks && table && used) {
        for (int i = 0; i < states; i++) {
            occupancies[i] = index_to_occupancy(i, bits, mask);
            attacks[i] = is_rook ? generate_rook_attacks_otf_user(sq, occupancies[i])
                                 : generate_bishop_attacks_otf_user(sq, occupancies[i]);
        }
        for (int attempt = 0; attempt < max_attempts && !found; attempt++) {
            Bitboard candidate = random_u64_fewbits();
            memset(used, 0, states);
            bool ok = true;
            for (int i = 0; i < states; i++) {
                unsigned int idx = (unsigned int)((occupancies[i] * candidate) >> (64 - bits));
                if (!used[idx]) {
                    table[idx] = attacks[i];
                    used[idx] = 1;
                } else if (table[idx] != attacks[i]) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                *magic_out = candidate;
                found = true;
            }
        }
    }
    free(occupancies);
    free(attacks);
    free(table);
    free(used);
    return found;
}

static bool search_magics(void) {
    srand((unsigned int)time(NULL));
    printf("Searching for magic numbers at runtime...\n");
    bool ok = true;
    for (Square sq = 0; sq < 64; sq++) {
        ok = find_magic_for_square(sq, true, 10000000, &ROOK_MAGICS[sq]) && ok;
        ok = find_magic_for_square(sq, false, 10000000, &BISHOP_MAGICS[sq]) && ok;
    }
    return ok;
}
#endif

bool findAndInitMagicNumbers() {
#ifdef RUNTIME_MAGICS
    if (!search_magics()) {
        fprintf(stderr, "Error: magic number search failed\n");
        return false;
    }
#endif
    bool ok = init_magic_table(g_rook_magics, ROOK_MAGICS, &ROOK_ATTACK_STORAGE, true);
    ok = init_magic_table(g_bishop_magics, BISHOP_MAGICS, &BISHOP_ATTACK_STORAGE, false) && ok;
    if (!ok) {
        fprintf(stderr, "Error: precomputed magic numbers are invalid\n");
    }
    return ok;
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

    findAndInitMagicNumbers();
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

// Generate only pseudo-legal QUIET moves (no captures, no promotions).
// Together with generateCaptureAndPromotionMoves this covers exactly the
// move set of generateMoves - the staged move picker relies on that.
void generateQuietMoves(const Board* board, MoveList* list) {
    list->count = 0;
    bool isWhite = board->whiteToMove;

    Bitboard friendlyPieces = getOccupiedByColor(board, isWhite);
    Bitboard enemyPieces = getOccupiedByColor(board, !isWhite);
    Bitboard allPieces = friendlyPieces | enemyPieces;
    Bitboard emptySquares = ~allPieces;

    // 1. Pawn pushes (pushes to the promotion rank are in the capture/promotion list)
    Bitboard pawns = isWhite ? board->whitePawns : board->blackPawns;
    int direction = isWhite ? 1 : -1;
    int startRank = isWhite ? 1 : 6;
    int promotionRank = isWhite ? 7 : 0;
    Bitboard currentPawns = pawns;
    while (currentPawns) {
        Square fromSq = BIT_SCAN_FORWARD(currentPawns);
        CLEAR_BIT(currentPawns, fromSq);
        int rank = fromSq / 8;
        if (rank + direction == promotionRank) continue;

        Square toSq = fromSq + 8 * direction;
        if (!GET_BIT(allPieces, toSq)) {
            addMove(list, CREATE_MOVE(fromSq, toSq, 0,0,0,0,0));
            if (rank == startRank) {
                Square toSq_double = fromSq + 16 * direction;
                if (!GET_BIT(allPieces, toSq_double)) {
                    addMove(list, CREATE_MOVE(fromSq, toSq_double, 0,0,1,0,0));
                }
            }
        }
    }

    // 2. Knight quiet moves
    Bitboard knights = isWhite ? board->whiteKnights : board->blackKnights;
    while (knights) {
        Square fromSq = BIT_SCAN_FORWARD(knights);
        CLEAR_BIT(knights, fromSq);
        Bitboard quietTargets = KNIGHT_ATTACKS[fromSq] & emptySquares;
        while (quietTargets) {
            Square toSq = BIT_SCAN_FORWARD(quietTargets);
            CLEAR_BIT(quietTargets, toSq);
            addMove(list, CREATE_MOVE(fromSq, toSq, 0,0,0,0,0));
        }
    }

    // 3. King quiet moves (castling handled separately below)
    Bitboard king = isWhite ? board->whiteKings : board->blackKings;
    if (king) {
        Square fromSq = BIT_SCAN_FORWARD(king);
        Bitboard quietTargets = KING_ATTACKS[fromSq] & emptySquares;
        while (quietTargets) {
            Square toSq = BIT_SCAN_FORWARD(quietTargets);
            CLEAR_BIT(quietTargets, toSq);
            addMove(list, CREATE_MOVE(fromSq, toSq, 0,0,0,0,0));
        }
    }

    // 4. Sliding piece quiet moves
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

        while (pieces) {
            Square fromSq = BIT_SCAN_FORWARD(pieces);
            CLEAR_BIT(pieces, fromSq);
            Bitboard attacks;
            if (pieceType == ROOK_T) attacks = getRookAttacks(fromSq, allPieces);
            else if (pieceType == BISHOP_T) attacks = getBishopAttacks(fromSq, allPieces);
            else /* QUEEN_T */ attacks = getQueenAttacks(fromSq, allPieces);

            Bitboard quietTargets = attacks & emptySquares;
            while (quietTargets) {
                Square toSq = BIT_SCAN_FORWARD(quietTargets);
                CLEAR_BIT(quietTargets, toSq);
                addMove(list, CREATE_MOVE(fromSq, toSq, 0,0,0,0,0));
            }
        }
    }

    // 5. Castling
    generateCastlingMoves(board, list, isWhite);
}

// Check whether a move is pseudo-legal in the given position, i.e. whether
// generateMoves would emit exactly this move. The staged move picker uses
// this to validate TT moves before trying them without any generated list -
// a corrupted or colliding TT entry must never reach applyMove.
bool moveIsPseudoLegal(const Board* board, Move m) {
    if (m == 0) return false;
    int from = MOVE_FROM(m);
    int to = MOVE_TO(m);
    bool isWhite = board->whiteToMove;

    uint8_t p = board->piece[from];
    if (p == NO_PIECE || PIECE_COLOR_OF(p) != (isWhite ? WHITE : BLACK)) return false;
    int type = PIECE_TYPE_OF(p);

    Bitboard friendlyPieces = getOccupiedByColor(board, isWhite);
    Bitboard enemyPieces = getOccupiedByColor(board, !isWhite);
    Bitboard allPieces = friendlyPieces | enemyPieces;

    if (MOVE_IS_CASTLING(m)) {
        if (type != KING) return false;
        MoveList list;
        list.count = 0;
        generateCastlingMoves(board, &list, isWhite);
        for (int i = 0; i < list.count; i++) {
            if (list.moves[i] == m) return true;
        }
        return false;
    }

    if (type == PAWN) {
        int direction = isWhite ? 1 : -1;
        int startRank = isWhite ? 1 : 6;
        int promotionRank = isWhite ? 7 : 0;
        int rank = from / 8;
        Bitboard pawnAttacks = PAWN_ATTACKS[isWhite ? 0 : 1][from];

        if (MOVE_IS_EN_PASSANT(m)) {
            if (board->enPassantSquare == SQ_NONE || to != board->enPassantSquare) return false;
            if (!GET_BIT(pawnAttacks, to)) return false;
            return m == CREATE_MOVE(from, to, 0, 1, 0, 1, 0);
        }

        if (GET_BIT(pawnAttacks, to) && GET_BIT(enemyPieces, to)) {
            if (rank + direction == promotionRank) {
                int promo = (int)MOVE_PROMOTION(m);
                if (promo < PROMOTION_N || promo > PROMOTION_Q) return false;
                return m == CREATE_MOVE(from, to, promo, 1, 0, 0, 0);
            }
            return m == CREATE_MOVE(from, to, 0, 1, 0, 0, 0);
        }

        if (to == from + 8 * direction && !GET_BIT(allPieces, to)) {
            if (rank + direction == promotionRank) {
                int promo = (int)MOVE_PROMOTION(m);
                if (promo < PROMOTION_N || promo > PROMOTION_Q) return false;
                return m == CREATE_MOVE(from, to, promo, 0, 0, 0, 0);
            }
            return m == CREATE_MOVE(from, to, 0, 0, 0, 0, 0);
        }

        if (to == from + 16 * direction && rank == startRank &&
            !GET_BIT(allPieces, from + 8 * direction) && !GET_BIT(allPieces, to)) {
            return m == CREATE_MOVE(from, to, 0, 0, 1, 0, 0);
        }

        return false;
    }

    Bitboard attacks;
    switch (type) {
        case KNIGHT: attacks = KNIGHT_ATTACKS[from]; break;
        case BISHOP: attacks = getBishopAttacks(from, allPieces); break;
        case ROOK:   attacks = getRookAttacks(from, allPieces); break;
        case QUEEN:  attacks = getQueenAttacks(from, allPieces); break;
        case KING:   attacks = KING_ATTACKS[from]; break;
        default:     return false;
    }
    if (!GET_BIT(attacks & ~friendlyPieces, to)) return false;
    return m == CREATE_MOVE(from, to, 0, GET_BIT(enemyPieces, to), 0, 0, 0);
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