#include "nnue.h"
#include "bitboard_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Global network instance
NNUENetwork nnue_net = {0};

// Input bucket map based on king position (from white's perspective)
// Mirrored for black's perspective
const int NNUE_INPUT_BUCKET_MAP[64] = {
    0, 0, 1, 1, 1, 1, 0, 0, // Rank 1 (from white's perspective)
    2, 2, 3, 3, 3, 3, 2, 2, // Rank 2
    2, 2, 3, 3, 3, 3, 2, 2, // Rank 3
    4, 4, 4, 4, 4, 4, 4, 4, // Rank 4
    4, 4, 4, 4, 4, 4, 4, 4, // Rank 5
    4, 4, 4, 4, 4, 4, 4, 4, // Rank 6
    4, 4, 4, 4, 4, 4, 4, 4, // Rank 7
    4, 4, 4, 4, 4, 4, 4, 4  // Rank 8
};

// Helper: Get LSB index
static inline int get_lsb(Bitboard bb) {
    if (bb == 0) return -1;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(bb);
#else
    int count = 0;
    while (!((bb >> count) & 1)) {
        count++;
        if (count == 64) return -1;
    }
    return count;
#endif
}

// Helper: ReLU activation (clamped)
static inline int16_t clamp_relu(int32_t x) {
    if (x < 0) return 0;
    if (x > NNUE_QA) return NNUE_QA;
    return (int16_t)x;
}

// Get input bucket for king position from a perspective
// Returns bucket index and whether to mirror horizontally
typedef struct {
    int index;
    bool mirrored;
} KingBucket;

static KingBucket get_king_bucket(int king_square, int perspective) {
    // Transform square to perspective's view
    // For black: flip vertically (rank 1 becomes rank 8)
    int transformed_sq = (perspective == 1) ? (king_square ^ 56) : king_square;
    
    KingBucket result;
    result.index = NNUE_INPUT_BUCKET_MAP[transformed_sq];
    // Mirror if king is on the right half of the board (files e-h, i.e., file >= 4)
    result.mirrored = (transformed_sq % 8) >= 4;
    return result;
}

// Get feature index for a piece on a square from a perspective
// Returns the full linear offset into the flat featureWeights array (divided by HIDDEN_SIZE)
// Based on Java code:
// BLACK perspective: (pieceColorIndex ^ 1) * ColorStride (FLIP), square ^ (mirrored ? 63 : 56)
// WHITE perspective: pieceColorIndex * ColorStride (NO flip), square ^ (mirrored ? 7 : 0)
static int get_feature_index(int perspective, int piece_type, int piece_color, int square, KingBucket king_bucket) {
    // Constants matching Java
    const int COLOR_STRIDE = 64 * 6;  // 384
    const int PIECE_STRIDE = 64;
    const int BUCKET_STRIDE = NNUE_INPUT_SIZE;  // 768
    
    int mapped_color;
    int transformed_square;
    
    if (perspective == 1) {  // BLACK perspective
        // Java: mappedColorIndex = pieceColorIndex ^ 1 (FLIP color)
        mapped_color = piece_color ^ 1;
        // Java: square ^ (mirrored ? 63 : 56)
        transformed_square = king_bucket.mirrored ? (square ^ 63) : (square ^ 56);
    } else {  // WHITE perspective
        // Java: mappedColorIndex = pieceColorIndex (NO flip)
        mapped_color = piece_color;
        // Java: square ^ (mirrored ? 7 : 0)
        transformed_square = king_bucket.mirrored ? (square ^ 7) : square;
    }
    
    // Calculate full index including bucket (matching Java)
    int index = king_bucket.index * BUCKET_STRIDE + mapped_color * COLOR_STRIDE + piece_type * PIECE_STRIDE + transformed_square;
    
    return index;
}

// Get output bucket based on piece count (including kings)
// Matches Java: bucketIndex = (pieceCount - 2) / divisor
// where divisor = ceil(30 / outputBuckets) = 4 for 8 buckets
int nnue_get_output_bucket(const Board* board) {
    // Count ALL pieces including kings
    int piece_count = 0;
    piece_count += POPCOUNT(board->whitePawns);
    piece_count += POPCOUNT(board->whiteKnights);
    piece_count += POPCOUNT(board->whiteBishops);
    piece_count += POPCOUNT(board->whiteRooks);
    piece_count += POPCOUNT(board->whiteQueens);
    piece_count += POPCOUNT(board->whiteKings);
    piece_count += POPCOUNT(board->blackPawns);
    piece_count += POPCOUNT(board->blackKnights);
    piece_count += POPCOUNT(board->blackBishops);
    piece_count += POPCOUNT(board->blackRooks);
    piece_count += POPCOUNT(board->blackQueens);
    piece_count += POPCOUNT(board->blackKings);
    
    // Java formula: (pieceCount - 2) / divisor
    // divisor = (30 + 8 - 1) / 8 = 4
    // Bucket mapping:
    // pieceCount 2-5  -> bucket 0
    // pieceCount 6-9  -> bucket 1
    // pieceCount 10-13 -> bucket 2
    // pieceCount 14-17 -> bucket 3
    // pieceCount 18-21 -> bucket 4
    // pieceCount 22-25 -> bucket 5
    // pieceCount 26-29 -> bucket 6
    // pieceCount 30-32 -> bucket 7
    int divisor = 4;
    int bucket_index = (piece_count - 2) / divisor;
    
    // Clamp to valid range [0, OUTPUT_BUCKETS-1]
    if (bucket_index < 0) bucket_index = 0;
    if (bucket_index >= NNUE_OUTPUT_BUCKETS) bucket_index = NNUE_OUTPUT_BUCKETS - 1;
    
    return bucket_index;
}

// Initialize with random weights (for testing)
void nnue_init_random(void) {
    // Initialize feature transformer weights with small random values
    for (int bucket = 0; bucket < NNUE_INPUT_BUCKETS; bucket++) {
        for (int i = 0; i < NNUE_INPUT_SIZE; i++) {
            for (int j = 0; j < NNUE_HIDDEN_SIZE; j++) {
                // Use a simple pseudo-random initialization with proper casting to avoid overflow
                uint64_t seed = (uint64_t)bucket * 768 + (uint64_t)i * 1024 + (uint64_t)j;
                uint64_t val = (seed * 1103515245ULL + 12345ULL) % 65536ULL;
                nnue_net.ft_weights[bucket][i][j] = (int16_t)((val % 64) - 32);
            }
        }
    }
    
    // Initialize feature transformer biases
    for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
        nnue_net.ft_biases[i] = 0;
    }
    
    // Initialize output layer weights
    for (int bucket = 0; bucket < NNUE_OUTPUT_BUCKETS; bucket++) {
        for (int i = 0; i < 2 * NNUE_HIDDEN_SIZE; i++) {
            uint64_t seed = (uint64_t)bucket * 2048 + (uint64_t)i;
            uint64_t val = (seed * 1103515245ULL + 12345ULL) % 65536ULL;
            nnue_net.output_weights[bucket][i] = (int16_t)((val % 32) - 16);
        }
        nnue_net.output_biases[bucket] = 0;
    }
    
    nnue_net.loaded = true;
    printf("info string NNUE initialized with random weights\n");
}

// Load NNUE weights from file
// Layout: featureWeights[inputBuckets * inputSize * layer1Size] (linear)
//         featureBiases[layer1Size] (only ONCE, not per bucket!)
//         outputWeights[outputBuckets * 2 * layer1Size] (for each bucket: us_weights, them_weights)
//         outputBiases[outputBuckets]
//         48-byte trailer "bulletbullet..." (ignored)
bool nnue_load(const char* filename) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "info string Failed to open NNUE file: %s\n", filename);
        return false;
    }
    
    // Get file size for validation
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Expected size calculation:
    // featureWeights: INPUT_BUCKETS * INPUT_SIZE * HIDDEN_SIZE * sizeof(int16_t)
    // featureBiases: HIDDEN_SIZE * sizeof(int16_t) (only once, NOT per bucket!)
    // outputWeights: OUTPUT_BUCKETS * 2 * HIDDEN_SIZE * sizeof(int16_t)
    // outputBiases: OUTPUT_BUCKETS * sizeof(int16_t)
    // + 48 byte trailer
    long expected_data_size = 
        (long)NNUE_INPUT_BUCKETS * NNUE_INPUT_SIZE * NNUE_HIDDEN_SIZE * sizeof(int16_t) +
        (long)NNUE_HIDDEN_SIZE * sizeof(int16_t) +
        (long)NNUE_OUTPUT_BUCKETS * 2 * NNUE_HIDDEN_SIZE * sizeof(int16_t) +
        (long)NNUE_OUTPUT_BUCKETS * sizeof(int16_t);
    long expected_size = expected_data_size + 48;  // 48-byte "bullet..." trailer
    
    printf("info string NNUE file size: %ld bytes, expected: %ld bytes\n", file_size, expected_size);
    
    if (file_size != expected_size) {
        fprintf(stderr, "info string NNUE file size mismatch! Got %ld, expected %ld\n", file_size, expected_size);
        fclose(file);
        return false;
    }
    
    // Read feature transformer weights: [bucket * inputSize + input][hidden]
    // Java reads: readBlocks(in, featureWeights, inputBuckets * inputSize, layer1Size, layer1Size)
    // This means: for each of (inputBuckets * inputSize) blocks, read layer1Size values
    for (int bucket = 0; bucket < NNUE_INPUT_BUCKETS; bucket++) {
        for (int i = 0; i < NNUE_INPUT_SIZE; i++) {
            if (fread(nnue_net.ft_weights[bucket][i], sizeof(int16_t), NNUE_HIDDEN_SIZE, file) != NNUE_HIDDEN_SIZE) {
                fclose(file);
                fprintf(stderr, "info string Failed to read NNUE feature transformer weights\n");
                return false;
            }
        }
    }
    
    // Read feature transformer biases
    if (fread(nnue_net.ft_biases, sizeof(int16_t), NNUE_HIDDEN_SIZE, file) != NNUE_HIDDEN_SIZE) {
        fclose(file);
        fprintf(stderr, "info string Failed to read NNUE feature transformer biases\n");
        return false;
    }
    
    // Read output layer weights: [outputBuckets * 2][layer1Size]
    // Java layout: for each bucket, us_weights[layer1Size] then them_weights[layer1Size]
    // So output_weights[bucket * 2 * layer1Size + 0..layer1Size-1] = us weights
    //    output_weights[bucket * 2 * layer1Size + layer1Size..2*layer1Size-1] = them weights
    for (int bucket = 0; bucket < NNUE_OUTPUT_BUCKETS; bucket++) {
        // Read us weights, then them weights for this bucket
        if (fread(nnue_net.output_weights[bucket], sizeof(int16_t), 2 * NNUE_HIDDEN_SIZE, file) != 2 * NNUE_HIDDEN_SIZE) {
            fclose(file);
            fprintf(stderr, "info string Failed to read NNUE output weights for bucket %d\n", bucket);
            return false;
        }
    }
    
    // Read output layer biases
    if (fread(nnue_net.output_biases, sizeof(int16_t), NNUE_OUTPUT_BUCKETS, file) != NNUE_OUTPUT_BUCKETS) {
        fclose(file);
        fprintf(stderr, "info string Failed to read NNUE output layer biases\n");
        return false;
    }
    
    fclose(file);
    nnue_net.loaded = true;
    printf("info string NNUE loaded successfully from %s\n", filename);
    return true;
}

// Save NNUE weights to file
bool nnue_save(const char* filename) {
    FILE* file = fopen(filename, "wb");
    if (!file) {
        return false;
    }
    
    // Write magic header
    fwrite("NNUE", 1, 4, file);
    
    // Write version
    uint32_t version = 1;
    fwrite(&version, sizeof(uint32_t), 1, file);
    
    // Write feature transformer weights
    for (int bucket = 0; bucket < NNUE_INPUT_BUCKETS; bucket++) {
        for (int i = 0; i < NNUE_INPUT_SIZE; i++) {
            fwrite(nnue_net.ft_weights[bucket][i], sizeof(int16_t), NNUE_HIDDEN_SIZE, file);
        }
    }
    
    // Write feature transformer biases
    fwrite(nnue_net.ft_biases, sizeof(int16_t), NNUE_HIDDEN_SIZE, file);
    
    // Write output layer weights
    for (int bucket = 0; bucket < NNUE_OUTPUT_BUCKETS; bucket++) {
        fwrite(nnue_net.output_weights[bucket], sizeof(int16_t), 2 * NNUE_HIDDEN_SIZE, file);
    }
    
    // Write output layer biases
    fwrite(nnue_net.output_biases, sizeof(int16_t), NNUE_OUTPUT_BUCKETS, file);
    
    fclose(file);
    return true;
}

// Add all features for a specific piece bitboard to accumulator
static void add_piece_features(NNUEAccumulator* acc,
                                Bitboard pieces, int piece_type, int piece_color,
                                KingBucket white_bucket, KingBucket black_bucket) {
    while (pieces) {
        int sq = get_lsb(pieces);
        pieces &= pieces - 1; // Clear LSB
        
        // White's perspective - get full feature index
        int white_index = get_feature_index(0, piece_type, piece_color, sq, white_bucket);
        // Convert to 3D array indices: ft_weights[bucket][input][hidden]
        int white_bucket_idx = white_index / NNUE_INPUT_SIZE;
        int white_input_idx = white_index % NNUE_INPUT_SIZE;
        for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
            acc->white[i] += nnue_net.ft_weights[white_bucket_idx][white_input_idx][i];
        }
        
        // Black's perspective - get full feature index
        int black_index = get_feature_index(1, piece_type, piece_color, sq, black_bucket);
        int black_bucket_idx = black_index / NNUE_INPUT_SIZE;
        int black_input_idx = black_index % NNUE_INPUT_SIZE;
        for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
            acc->black[i] += nnue_net.ft_weights[black_bucket_idx][black_input_idx][i];
        }
    }
}

// Compute full accumulator from scratch
void nnue_refresh_accumulator(const Board* board, NNUEAccumulator* acc) {
    // Get king squares
    int white_king_sq = get_lsb(board->whiteKings);
    int black_king_sq = get_lsb(board->blackKings);
    
    if (white_king_sq < 0 || black_king_sq < 0) {
        // Invalid position, just zero the accumulator
        memset(acc->white, 0, sizeof(acc->white));
        memset(acc->black, 0, sizeof(acc->black));
        acc->computed = false;
        return;
    }
    
    // Get king buckets (with mirroring info)
    // WHITE perspective: use white king square, no vertical flip
    // BLACK perspective: use black king square, flip vertically
    KingBucket white_bucket = get_king_bucket(white_king_sq, 0);
    KingBucket black_bucket = get_king_bucket(black_king_sq, 1);
    
    // Start with biases
    for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
        acc->white[i] = nnue_net.ft_biases[i];
        acc->black[i] = nnue_net.ft_biases[i];
    }
    
    // Add all piece features
    add_piece_features(acc, board->whitePawns, NNUE_PIECE_PAWN, 0, white_bucket, black_bucket);
    add_piece_features(acc, board->whiteKnights, NNUE_PIECE_KNIGHT, 0, white_bucket, black_bucket);
    add_piece_features(acc, board->whiteBishops, NNUE_PIECE_BISHOP, 0, white_bucket, black_bucket);
    add_piece_features(acc, board->whiteRooks, NNUE_PIECE_ROOK, 0, white_bucket, black_bucket);
    add_piece_features(acc, board->whiteQueens, NNUE_PIECE_QUEEN, 0, white_bucket, black_bucket);
    add_piece_features(acc, board->whiteKings, NNUE_PIECE_KING, 0, white_bucket, black_bucket);
    
    add_piece_features(acc, board->blackPawns, NNUE_PIECE_PAWN, 1, white_bucket, black_bucket);
    add_piece_features(acc, board->blackKnights, NNUE_PIECE_KNIGHT, 1, white_bucket, black_bucket);
    add_piece_features(acc, board->blackBishops, NNUE_PIECE_BISHOP, 1, white_bucket, black_bucket);
    add_piece_features(acc, board->blackRooks, NNUE_PIECE_ROOK, 1, white_bucket, black_bucket);
    add_piece_features(acc, board->blackQueens, NNUE_PIECE_QUEEN, 1, white_bucket, black_bucket);
    add_piece_features(acc, board->blackKings, NNUE_PIECE_KING, 1, white_bucket, black_bucket);
    
    acc->computed = true;
}

// Evaluate position using NNUE
int nnue_evaluate(const Board* board, NNUEAccumulator* acc) {
    // Refresh accumulator if needed
    if (!acc->computed) {
        nnue_refresh_accumulator(board, acc);
    }
    
    // Get output bucket
    int output_bucket = nnue_get_output_bucket(board);
    
    // Determine which perspective is "us" (side to move)
    // Java: us = (stm == WHITE) ? white : black
    int16_t* us_acc = board->whiteToMove ? acc->white : acc->black;
    int16_t* them_acc = board->whiteToMove ? acc->black : acc->white;
    
    // Apply SCReLU (Squared Clipped ReLU) and compute output
    // Java formula: 
    //   output = evaluateHiddenLayer(us, them, outputWeights, bucket)
    //   output /= QA
    //   output += outputBiases[bucket]
    //   return (output * SCALE) / (QA * QB)
    //
    // SCReLU: clamped = clamp(x, 0, QA); result = clamped * clamped * weight
    
    int64_t output = 0;  // Use int64_t to avoid overflow!
    
    // Process "us" perspective with SCReLU (first half of output weights for this bucket)
    // Java slow path: sum += clampedValue * clampedValue * weights[i]
    for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
        int32_t clamped = us_acc[i];
        if (clamped < 0) clamped = 0;
        if (clamped > NNUE_QA) clamped = NNUE_QA;
        output += (int64_t)clamped * clamped * nnue_net.output_weights[output_bucket][i];
    }
    
    // Process "them" perspective with SCReLU (second half of output weights for this bucket)
    for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
        int32_t clamped = them_acc[i];
        if (clamped < 0) clamped = 0;
        if (clamped > NNUE_QA) clamped = NNUE_QA;
        output += (int64_t)clamped * clamped * nnue_net.output_weights[output_bucket][NNUE_HIDDEN_SIZE + i];
    }
    
    // Apply scaling as per Java:
    // output /= QA
    // output += outputBiases[bucket]
    // return (output * SCALE) / (QA * QB)
    output /= NNUE_QA;
    output += nnue_net.output_biases[output_bucket];
    
    int eval = (output * NNUE_SCALE) / ((int64_t)NNUE_QA * NNUE_QB);
    
    // Convert from side-to-move perspective to White perspective
    // (to match classical evaluation convention)
    return board->whiteToMove ? eval : -eval;
}

// Add a feature to the accumulator (for incremental updates)
void nnue_add_feature(NNUEAccumulator* acc, int bucket, int feature_index) {
    for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
        acc->white[i] += nnue_net.ft_weights[bucket][feature_index][i];
    }
}

// Remove a feature from the accumulator (for incremental updates)
void nnue_remove_feature(NNUEAccumulator* acc, int bucket, int feature_index) {
    for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
        acc->white[i] -= nnue_net.ft_weights[bucket][feature_index][i];
    }
}
