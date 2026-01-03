#ifndef NNUE_H
#define NNUE_H

#include "board.h"
#include <stdint.h>
#include <stdbool.h>

// NNUE Network Architecture
#define NNUE_INPUT_SIZE      768   // 64 squares * 6 piece types * 2 colors
#define NNUE_HIDDEN_SIZE     1024  // Hidden layer neurons per perspective
#define NNUE_INPUT_BUCKETS   5     // King position buckets
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

// Accumulator for efficient incremental updates
typedef struct {
    int16_t white[NNUE_HIDDEN_SIZE];
    int16_t black[NNUE_HIDDEN_SIZE];
    bool computed;
} NNUEAccumulator;

// NNUE Network weights
typedef struct {
    // Feature transformer weights: [INPUT_BUCKETS][INPUT_SIZE][HIDDEN_SIZE]
    int16_t ft_weights[NNUE_INPUT_BUCKETS][NNUE_INPUT_SIZE][NNUE_HIDDEN_SIZE];
    // Feature transformer biases: [HIDDEN_SIZE]
    int16_t ft_biases[NNUE_HIDDEN_SIZE];
    
    // Output layer weights: [OUTPUT_BUCKETS][2 * HIDDEN_SIZE] (both perspectives concatenated)
    int16_t output_weights[NNUE_OUTPUT_BUCKETS][2 * NNUE_HIDDEN_SIZE];
    // Output layer biases: [OUTPUT_BUCKETS]
    int16_t output_biases[NNUE_OUTPUT_BUCKETS];
    
    bool loaded;
} NNUENetwork;

// Global network instance
extern NNUENetwork nnue_net;

// Input bucket map for king position
extern const int NNUE_INPUT_BUCKET_MAP[64];

// Initialize NNUE with random weights (for testing without a trained net)
void nnue_init_random(void);

// Load NNUE weights from file
bool nnue_load(const char* filename);

// Save NNUE weights to file
bool nnue_save(const char* filename);

// Compute full accumulator from scratch
void nnue_refresh_accumulator(const Board* board, NNUEAccumulator* acc);

// Evaluate position using NNUE
int nnue_evaluate(const Board* board, NNUEAccumulator* acc);

// Get feature index for a piece on a square from a perspective
int nnue_get_feature_index(int piece_type, int piece_color, int square, int perspective);

// Get input bucket for a king square from a perspective
int nnue_get_input_bucket(int king_square, int perspective);

// Get output bucket based on piece count
int nnue_get_output_bucket(const Board* board);

// Incremental accumulator update functions
void nnue_add_feature(NNUEAccumulator* acc, int bucket, int feature_index);
void nnue_remove_feature(NNUEAccumulator* acc, int bucket, int feature_index);

#endif // NNUE_H
