#ifndef NNUE_H
#define NNUE_H

#include "board.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>

// NNUE Network Architecture
#define NNUE_INPUT_SIZE      768   // 64 squares * 6 piece types * 2 colors
#define NNUE_HIDDEN_SIZE     256  // Hidden layer neurons per perspective
#define NNUE_INPUT_BUCKETS   10    // King position buckets
#define NNUE_OUTPUT_BUCKETS  8     // Output buckets

// Feature indices
#define NNUE_PIECE_PAWN      0
#define NNUE_PIECE_KNIGHT    1
#define NNUE_PIECE_BISHOP    2
#define NNUE_PIECE_ROOK      3
#define NNUE_PIECE_QUEEN     4
#define NNUE_PIECE_KING      5

// Quantization constants
#define NNUE_QA              255   // Feature transformer weight quantization
#define NNUE_QB              64    // Output layer weight quantization
#define NNUE_SCALE           400   // Final output scale

// Accumulator for efficient incremental updates (activation values only)
// Aligned to 64 bytes for AVX-512 optimal access
typedef struct {
    alignas(64) int16_t white[NNUE_HIDDEN_SIZE];
    alignas(64) int16_t black[NNUE_HIDDEN_SIZE];
    bool computed;
} NNUEAccumulator;

// NNUE Network weights (separate from accumulator, can be loaded from file)
// Aligned to 64 bytes for AVX-512 optimal access
typedef struct {
    // Feature transformer weights: [INPUT_BUCKETS][INPUT_SIZE][HIDDEN_SIZE]
    alignas(64) int16_t ft_weights[NNUE_INPUT_BUCKETS][NNUE_INPUT_SIZE][NNUE_HIDDEN_SIZE];
    // Feature transformer biases: [HIDDEN_SIZE]
    alignas(64) int16_t ft_biases[NNUE_HIDDEN_SIZE];

    // Output layer weights: [OUTPUT_BUCKETS][2 * HIDDEN_SIZE] (both perspectives concatenated)
    alignas(64) int16_t output_weights[NNUE_OUTPUT_BUCKETS][2 * NNUE_HIDDEN_SIZE];
    // Output layer biases: [OUTPUT_BUCKETS]
    alignas(64) int16_t output_biases[NNUE_OUTPUT_BUCKETS];

    bool loaded;
} NNUENetwork;

// Global network instance (deprecated - weights now in NNUEAccumulator)
extern NNUENetwork nnue_net;

// Input bucket map for king position

// Initialize NNUE with random weights (for testing without a trained net)
void nnue_init_random(void);

// Load NNUE weights from file into network
bool nnue_load(const char* filename, NNUENetwork* net);

// Save NNUE weights to file
bool nnue_save(const char* filename, const NNUENetwork* net);

// Load NNUE weights from file into network
bool nnue_load(const char* filename, NNUENetwork* net);

// Save NNUE weights to file from network
bool nnue_save(const char* filename, const NNUENetwork* net);

// Compute full accumulator from scratch - needs network for initial computation
void nnue_reset_accumulator(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net);

// Refresh accumulator from current board state
void nnue_refresh_accumulator(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net);

// Evaluate position using NNUE - only needs accumulator
int nnue_evaluate(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net);

// Get output bucket based on piece count
int nnue_get_output_bucket(const Board* board);

// Apply move incrementally - needs network for weights
void nnue_apply_move(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net, int from_sq, int to_sq, 
                     int piece_type, int captured_piece_type, bool is_white, bool is_en_passant);

// Undo move incrementally - needs network for weights
void nnue_undo_move(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net, int from_sq, int to_sq,
                    int piece_type, int captured_piece_type, bool is_white, bool is_en_passant);

#endif // NNUE_H
