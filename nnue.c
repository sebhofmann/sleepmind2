#include "nnue.h"
#include "bitboard_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

// Input bucket map based on king position
const int NNUE_INPUT_BUCKET_MAP[64] = {
    0, 0, 1, 1, 1, 1, 0, 0,
    2, 2, 3, 3, 3, 3, 2, 2,
    2, 2, 3, 3, 3, 3, 2, 2,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4
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

// Get input bucket for king position
typedef struct {
    int index;
    bool mirrored;
} KingBucket;

static KingBucket get_king_bucket(int king_square, int perspective) {
    int transformed_sq = (perspective == 1) ? (king_square ^ 56) : king_square;
    KingBucket result;
    result.index = NNUE_INPUT_BUCKET_MAP[transformed_sq];
    result.mirrored = (transformed_sq % 8) >= 4;
    return result;
}

// Get feature index for a piece
static int get_feature_index(int perspective, int piece_type, int piece_color, int square, KingBucket king_bucket) {
    const int COLOR_STRIDE = 64 * 6;
    const int PIECE_STRIDE = 64;
    const int BUCKET_STRIDE = NNUE_INPUT_SIZE;
    
    int mapped_color;
    int transformed_square;
    
    if (perspective == 1) {
        mapped_color = piece_color ^ 1;
        transformed_square = king_bucket.mirrored ? (square ^ 63) : (square ^ 56);
    } else {
        mapped_color = piece_color;
        transformed_square = king_bucket.mirrored ? (square ^ 7) : square;
    }
    
    return king_bucket.index * BUCKET_STRIDE + mapped_color * COLOR_STRIDE + piece_type * PIECE_STRIDE + transformed_square;
}

// Get output bucket based on piece count
int nnue_get_output_bucket(const Board* board) {
    int piece_count = 0;
    piece_count += POPCOUNT(board->whitePawns) + POPCOUNT(board->whiteKnights) + POPCOUNT(board->whiteBishops);
    piece_count += POPCOUNT(board->whiteRooks) + POPCOUNT(board->whiteQueens) + POPCOUNT(board->whiteKings);
    piece_count += POPCOUNT(board->blackPawns) + POPCOUNT(board->blackKnights) + POPCOUNT(board->blackBishops);
    piece_count += POPCOUNT(board->blackRooks) + POPCOUNT(board->blackQueens) + POPCOUNT(board->blackKings);
    
    int bucket_index = (piece_count - 2) / 4;
    if (bucket_index < 0) bucket_index = 0;
    if (bucket_index >= NNUE_OUTPUT_BUCKETS) bucket_index = NNUE_OUTPUT_BUCKETS - 1;
    return bucket_index;
}

// Load NNUE weights from file into network
bool nnue_load(const char* filename, NNUENetwork* net) {
    if (!filename || !net) {
        fprintf(stderr, "info string NNUE filename or network is NULL\n");
        return false;
    }
    
    FILE* file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "info string Failed to open NNUE file: %s (errno: %d)\n", filename, errno);
        char build_filename[256];
        snprintf(build_filename, sizeof(build_filename), "build/%s", filename);
        file = fopen(build_filename, "rb");
        if (!file) {
            fprintf(stderr, "info string Also tried: %s (not found)\n", build_filename);
            return false;
        }
        printf("info string Found NNUE file in build/ subdirectory\n");
    } else {
        printf("info string Found NNUE file: %s\n", filename);
    }
    
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    long expected_data_size = 
        (long)NNUE_INPUT_BUCKETS * NNUE_INPUT_SIZE * NNUE_HIDDEN_SIZE * sizeof(int16_t) +
        (long)NNUE_HIDDEN_SIZE * sizeof(int16_t) +
        (long)NNUE_OUTPUT_BUCKETS * 2 * NNUE_HIDDEN_SIZE * sizeof(int16_t) +
        (long)NNUE_OUTPUT_BUCKETS * sizeof(int16_t);
    long expected_size = expected_data_size + 48;
    
    printf("info string NNUE file size: %ld bytes, expected: %ld bytes\n", file_size, expected_size);
    
    if (file_size != expected_size) {
        fprintf(stderr, "info string NNUE file size mismatch! Got %ld, expected %ld\n", file_size, expected_size);
        fclose(file);
        return false;
    }
    
    // Read weights
    for (int bucket = 0; bucket < NNUE_INPUT_BUCKETS; bucket++) {
        for (int i = 0; i < NNUE_INPUT_SIZE; i++) {
            if (fread(net->ft_weights[bucket][i], sizeof(int16_t), NNUE_HIDDEN_SIZE, file) != NNUE_HIDDEN_SIZE) {
                fclose(file);
                fprintf(stderr, "info string Failed to read NNUE feature transformer weights\n");
                return false;
            }
        }
    }
    
    if (fread(net->ft_biases, sizeof(int16_t), NNUE_HIDDEN_SIZE, file) != NNUE_HIDDEN_SIZE) {
        fclose(file);
        fprintf(stderr, "info string Failed to read NNUE feature transformer biases\n");
        return false;
    }
    
    for (int bucket = 0; bucket < NNUE_OUTPUT_BUCKETS; bucket++) {
        if (fread(net->output_weights[bucket], sizeof(int16_t), 2 * NNUE_HIDDEN_SIZE, file) != 2 * NNUE_HIDDEN_SIZE) {
            fclose(file);
            fprintf(stderr, "info string Failed to read NNUE output weights for bucket %d\n", bucket);
            return false;
        }
    }
    
    if (fread(net->output_biases, sizeof(int16_t), NNUE_OUTPUT_BUCKETS, file) != NNUE_OUTPUT_BUCKETS) {
        fclose(file);
        fprintf(stderr, "info string Failed to read NNUE output layer biases\n");
        return false;
    }
    
    fclose(file);
    net->loaded = true;
    printf("info string NNUE loaded successfully from %s\n", filename);
    return true;
}

// Save NNUE weights to file
bool nnue_save(const char* filename, const NNUENetwork* net) {
    if (!filename || !net) return false;
    FILE* file = fopen(filename, "wb");
    if (!file) return false;
    
    for (int bucket = 0; bucket < NNUE_INPUT_BUCKETS; bucket++) {
        for (int i = 0; i < NNUE_INPUT_SIZE; i++) {
            fwrite(net->ft_weights[bucket][i], sizeof(int16_t), NNUE_HIDDEN_SIZE, file);
        }
    }
    fwrite(net->ft_biases, sizeof(int16_t), NNUE_HIDDEN_SIZE, file);
    for (int bucket = 0; bucket < NNUE_OUTPUT_BUCKETS; bucket++) {
        fwrite(net->output_weights[bucket], sizeof(int16_t), 2 * NNUE_HIDDEN_SIZE, file);
    }
    fwrite(net->output_biases, sizeof(int16_t), NNUE_OUTPUT_BUCKETS, file);
    
    fclose(file);
    return true;
}

// Add all pieces for accumulator computation
static void add_piece_features(NNUEAccumulator* acc, const NNUENetwork* net,
                                Bitboard pieces, int piece_type, int piece_color,
                                KingBucket white_bucket, KingBucket black_bucket) {
    while (pieces) {
        int sq = get_lsb(pieces);
        pieces &= pieces - 1;
        
        int white_index = get_feature_index(0, piece_type, piece_color, sq, white_bucket);
        int white_bucket_idx = white_index / NNUE_INPUT_SIZE;
        int white_input_idx = white_index % NNUE_INPUT_SIZE;
        for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
            acc->white[i] += net->ft_weights[white_bucket_idx][white_input_idx][i];
        }
        
        int black_index = get_feature_index(1, piece_type, piece_color, sq, black_bucket);
        int black_bucket_idx = black_index / NNUE_INPUT_SIZE;
        int black_input_idx = black_index % NNUE_INPUT_SIZE;
        for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
            acc->black[i] += net->ft_weights[black_bucket_idx][black_input_idx][i];
        }
    }
}

// Compute full accumulator from scratch
void nnue_refresh_accumulator(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net) {
    if (acc == NULL || net == NULL || board == NULL) return;
    
    int white_king_sq = get_lsb(board->whiteKings);
    int black_king_sq = get_lsb(board->blackKings);
    
    if (white_king_sq < 0 || black_king_sq < 0) {
        memset(acc->white, 0, sizeof(acc->white));
        memset(acc->black, 0, sizeof(acc->black));
        acc->computed = false;
        return;
    }
    
    KingBucket white_bucket = get_king_bucket(white_king_sq, 0);
    KingBucket black_bucket = get_king_bucket(black_king_sq, 1);
    
    for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
        acc->white[i] = net->ft_biases[i];
        acc->black[i] = net->ft_biases[i];
    }
    
    add_piece_features(acc, net, board->whitePawns, NNUE_PIECE_PAWN, 0, white_bucket, black_bucket);
    add_piece_features(acc, net, board->whiteKnights, NNUE_PIECE_KNIGHT, 0, white_bucket, black_bucket);
    add_piece_features(acc, net, board->whiteBishops, NNUE_PIECE_BISHOP, 0, white_bucket, black_bucket);
    add_piece_features(acc, net, board->whiteRooks, NNUE_PIECE_ROOK, 0, white_bucket, black_bucket);
    add_piece_features(acc, net, board->whiteQueens, NNUE_PIECE_QUEEN, 0, white_bucket, black_bucket);
    add_piece_features(acc, net, board->whiteKings, NNUE_PIECE_KING, 0, white_bucket, black_bucket);
    
    add_piece_features(acc, net, board->blackPawns, NNUE_PIECE_PAWN, 1, white_bucket, black_bucket);
    add_piece_features(acc, net, board->blackKnights, NNUE_PIECE_KNIGHT, 1, white_bucket, black_bucket);
    add_piece_features(acc, net, board->blackBishops, NNUE_PIECE_BISHOP, 1, white_bucket, black_bucket);
    add_piece_features(acc, net, board->blackRooks, NNUE_PIECE_ROOK, 1, white_bucket, black_bucket);
    add_piece_features(acc, net, board->blackQueens, NNUE_PIECE_QUEEN, 1, white_bucket, black_bucket);
    add_piece_features(acc, net, board->blackKings, NNUE_PIECE_KING, 1, white_bucket, black_bucket);
    
    acc->computed = true;
}

// Reset accumulator for new position using network
void nnue_reset_accumulator(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net) {
    if (acc == NULL || net == NULL) return;
    nnue_refresh_accumulator(board, acc, net);
}

// Evaluate position using NNUE
int nnue_evaluate(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net) {
    if (acc == NULL || net == NULL) return 0;
    
    if (!acc->computed) {
        nnue_refresh_accumulator(board, acc, net);
    }
    
    int output_bucket = nnue_get_output_bucket(board);
    int16_t* us_acc = board->whiteToMove ? acc->white : acc->black;
    int16_t* them_acc = board->whiteToMove ? acc->black : acc->white;
    
    int64_t output = 0;
    
    for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
        int32_t clamped = us_acc[i];
        if (clamped < 0) clamped = 0;
        if (clamped > NNUE_QA) clamped = NNUE_QA;
        output += (int64_t)clamped * clamped * net->output_weights[output_bucket][i];
    }
    
    for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
        int32_t clamped = them_acc[i];
        if (clamped < 0) clamped = 0;
        if (clamped > NNUE_QA) clamped = NNUE_QA;
        output += (int64_t)clamped * clamped * net->output_weights[output_bucket][NNUE_HIDDEN_SIZE + i];
    }
    
    output /= NNUE_QA;
    output += net->output_biases[output_bucket];
    
    int eval = (output * NNUE_SCALE) / ((int64_t)NNUE_QA * NNUE_QB);
    return board->whiteToMove ? eval : -eval;
}

// Helper: Update piece move for both perspectives
// WICHTIG: white_king_sq und black_king_sq müssen die Positionen VOR dem Zug sein!
static void nnue_update_piece_move_with_kings(NNUEAccumulator* acc, const NNUENetwork* net,
                                              int from_sq, int to_sq, int piece_type, int piece_color, 
                                              int white_king_sq, int black_king_sq, bool apply) {
    if (acc == NULL || net == NULL) return;
    
    if (white_king_sq < 0 || black_king_sq < 0) {
        return;  // Invalid - caller should refresh
    }
    
    KingBucket white_bucket = get_king_bucket(white_king_sq, 0);
    KingBucket black_bucket = get_king_bucket(black_king_sq, 1);
    
    int white_from_idx = get_feature_index(0, piece_type, piece_color, from_sq, white_bucket);
    int white_to_idx = get_feature_index(0, piece_type, piece_color, to_sq, white_bucket);
    int black_from_idx = get_feature_index(1, piece_type, piece_color, from_sq, black_bucket);
    int black_to_idx = get_feature_index(1, piece_type, piece_color, to_sq, black_bucket);
    
    // Extract bucket and input indices for all four feature updates
    int white_from_bucket = white_from_idx / NNUE_INPUT_SIZE;
    int white_from_input = white_from_idx % NNUE_INPUT_SIZE;
    int white_to_bucket = white_to_idx / NNUE_INPUT_SIZE;
    int white_to_input = white_to_idx % NNUE_INPUT_SIZE;
    
    int black_from_bucket = black_from_idx / NNUE_INPUT_SIZE;
    int black_from_input = black_from_idx % NNUE_INPUT_SIZE;
    int black_to_bucket = black_to_idx / NNUE_INPUT_SIZE;
    int black_to_input = black_to_idx % NNUE_INPUT_SIZE;
    
    if (apply) {
        for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
            acc->white[i] -= net->ft_weights[white_from_bucket][white_from_input][i];
            acc->white[i] += net->ft_weights[white_to_bucket][white_to_input][i];
            acc->black[i] -= net->ft_weights[black_from_bucket][black_from_input][i];
            acc->black[i] += net->ft_weights[black_to_bucket][black_to_input][i];
        }
    } else {
        for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
            acc->white[i] += net->ft_weights[white_from_bucket][white_from_input][i];
            acc->white[i] -= net->ft_weights[white_to_bucket][white_to_input][i];
            acc->black[i] += net->ft_weights[black_from_bucket][black_from_input][i];
            acc->black[i] -= net->ft_weights[black_to_bucket][black_to_input][i];
        }
    }
}

// Legacy wrapper that reads king positions from board
static void nnue_update_piece_move(NNUEAccumulator* acc, const Board* board, const NNUENetwork* net,
                                   int from_sq, int to_sq, int piece_type, int piece_color, bool apply) {
    if (acc == NULL || net == NULL) return;
    
    int white_king_sq = get_lsb(board->whiteKings);
    int black_king_sq = get_lsb(board->blackKings);
    
    if (white_king_sq < 0 || black_king_sq < 0) {
        nnue_refresh_accumulator(board, acc, net);
        return;
    }
    
    nnue_update_piece_move_with_kings(acc, net, from_sq, to_sq, piece_type, piece_color,
                                       white_king_sq, black_king_sq, apply);
}

// Apply move incrementally
// piece_type and captured_piece_type use NNUE indices (PAWN=0, KNIGHT=1, ...)
// captured_piece_type = -1 means no capture
void nnue_apply_move(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net, int from_sq, int to_sq, 
                     int piece_type, int captured_piece_type, bool is_white, bool is_en_passant) {
    if (acc == NULL || net == NULL) return;
    
    // WICHTIG: Nur inkrementell updaten wenn Accumulator bereits initialisiert!
    // Sonst würden wir Deltas auf Müllwerte anwenden.
    if (!acc->computed) return;
    
    nnue_update_piece_move(acc, board, net, from_sq, to_sq, piece_type, is_white ? 0 : 1, true);
    
    if (captured_piece_type >= 0) {  // -1 means no capture
        int capture_sq = to_sq;
        if (is_en_passant) {
            capture_sq = is_white ? (to_sq - 8) : (to_sq + 8);
        }
        
        int white_king_sq = get_lsb(board->whiteKings);
        int black_king_sq = get_lsb(board->blackKings);
        
        if (white_king_sq >= 0 && black_king_sq >= 0) {
            KingBucket white_bucket = get_king_bucket(white_king_sq, 0);
            KingBucket black_bucket = get_king_bucket(black_king_sq, 1);
            
            int captured_color = is_white ? 1 : 0;
            int white_cap_idx = get_feature_index(0, captured_piece_type, captured_color, capture_sq, white_bucket);
            int black_cap_idx = get_feature_index(1, captured_piece_type, captured_color, capture_sq, black_bucket);
            
            int white_bucket_idx = white_cap_idx / NNUE_INPUT_SIZE;
            int white_cap_input = white_cap_idx % NNUE_INPUT_SIZE;
            int black_bucket_idx = black_cap_idx / NNUE_INPUT_SIZE;
            int black_cap_input = black_cap_idx % NNUE_INPUT_SIZE;
            
            for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
                acc->white[i] -= net->ft_weights[white_bucket_idx][white_cap_input][i];
                acc->black[i] -= net->ft_weights[black_bucket_idx][black_cap_input][i];
            }
        }
    }
    // acc->computed bleibt true (war schon true, sonst hätten wir oben returned)
}

// Undo move incrementally
// piece_type and captured_piece_type use NNUE indices (PAWN=0, KNIGHT=1, ...)
// captured_piece_type = -1 means no capture
void nnue_undo_move(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net, int from_sq, int to_sq,
                    int piece_type, int captured_piece_type, bool is_white, bool is_en_passant) {
    if (acc == NULL || net == NULL) return;
    
    // WICHTIG: Nur inkrementell updaten wenn Accumulator bereits initialisiert!
    if (!acc->computed) return;
    
    nnue_update_piece_move(acc, board, net, from_sq, to_sq, piece_type, is_white ? 0 : 1, false);
    
    if (captured_piece_type >= 0) {  // -1 means no capture
        int capture_sq = to_sq;
        if (is_en_passant) {
            capture_sq = is_white ? (to_sq - 8) : (to_sq + 8);
        }
        
        int white_king_sq = get_lsb(board->whiteKings);
        int black_king_sq = get_lsb(board->blackKings);
        
        if (white_king_sq >= 0 && black_king_sq >= 0) {
            KingBucket white_bucket = get_king_bucket(white_king_sq, 0);
            KingBucket black_bucket = get_king_bucket(black_king_sq, 1);
            
            int captured_color = is_white ? 1 : 0;
            int white_cap_idx = get_feature_index(0, captured_piece_type, captured_color, capture_sq, white_bucket);
            int black_cap_idx = get_feature_index(1, captured_piece_type, captured_color, capture_sq, black_bucket);
            
            int white_bucket_idx = white_cap_idx / NNUE_INPUT_SIZE;
            int white_cap_input = white_cap_idx % NNUE_INPUT_SIZE;
            int black_bucket_idx = black_cap_idx / NNUE_INPUT_SIZE;
            int black_cap_input = black_cap_idx % NNUE_INPUT_SIZE;
            
            for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) {
                acc->white[i] += net->ft_weights[white_bucket_idx][white_cap_input][i];
                acc->black[i] += net->ft_weights[black_bucket_idx][black_cap_input][i];
            }
        }
    }
    // acc->computed bleibt true (war schon true, sonst hätten wir oben returned)
}
