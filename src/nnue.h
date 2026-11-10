#ifndef NNUE_H
#define NNUE_H

#include "board.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <stddef.h>

// NNUE architecture, selected at compile time (make NNUE_ARCH=deep):
//   NNUE_ARCH_LEGACY: (768x10 -> HIDDEN)x2 -> SCReLU -> 1, 8 output buckets  (training/training.rs)
//   NNUE_ARCH_DEEP:   (768x16 -> 768)x2 -> pairwise CReLU -> 16 -> [x, x^2] -> 32 -> 1,
//                     8 output buckets on every layer                          (training/training_deep.rs)
// A net only loads into a build of the matching architecture (checked via file size).
#define NNUE_ARCH_LEGACY     0
#define NNUE_ARCH_DEEP       1
#ifndef NNUE_ARCH
#define NNUE_ARCH            NNUE_ARCH_LEGACY
#endif

// Net file of this build (set by the Makefile per architecture)
#ifndef NNUE_EMBED_FILE
#if NNUE_ARCH == NNUE_ARCH_DEEP
#define NNUE_EMBED_FILE      "quantised_deep.bin"
#else
#define NNUE_EMBED_FILE      "quantised.bin"
#endif
#endif

#define NNUE_INPUT_SIZE      768   // 64 squares * 6 piece types * 2 colors
#define NNUE_OUTPUT_BUCKETS  8     // Output buckets (material count)

#if NNUE_ARCH == NNUE_ARCH_DEEP
#define NNUE_HIDDEN_SIZE     768   // FT width per perspective (pairwise -> 384 per perspective)
#define NNUE_INPUT_BUCKETS   16    // King position buckets
#define NNUE_L1_IN           NNUE_HIDDEN_SIZE   // 2 perspectives x HIDDEN/2
#define NNUE_L2_SIZE         16    // L1 outputs per bucket
#define NNUE_L3_SIZE         32    // L2 outputs per bucket
#else
#ifndef NNUE_HIDDEN_SIZE
#define NNUE_HIDDEN_SIZE     768  // Hidden layer neurons per perspective
#endif
#define NNUE_INPUT_BUCKETS   10    // King position buckets
#endif

// Feature indices
#define NNUE_PIECE_PAWN      0
#define NNUE_PIECE_KNIGHT    1
#define NNUE_PIECE_BISHOP    2
#define NNUE_PIECE_ROOK      3
#define NNUE_PIECE_QUEEN     4
#define NNUE_PIECE_KING      5

// Quantization constants (must match the SavedFormat in the corresponding training script)
#define NNUE_QA              255   // Feature transformer weight quantization
#define NNUE_QB              64    // Legacy: output layer weight quantization
#define NNUE_Q               64    // Deep: L2/L3 weight quantization
#ifndef NNUE_SCALE
#define NNUE_SCALE           295   // Final output scale (must match bullet's eval_scale)
#endif

// Accumulator for efficient incremental updates (activation values only)
// Aligned to 64 bytes for AVX-512 optimal access
typedef struct NNUEAccumulator {
    alignas(64) int16_t white[NNUE_HIDDEN_SIZE];
    alignas(64) int16_t black[NNUE_HIDDEN_SIZE];
    bool computed;
    bool dirty;
    bool requires_refresh;
    struct NNUEAccumulator* previous;
    int from_sq;
    int to_sq;
    int piece_type;
    int captured_piece_type;
    int moving_color;
    int capture_sq;
    int white_king_sq;
    int black_king_sq;
} NNUEAccumulator;

// NNUE Network weights (separate from accumulator, can be loaded from file)
// Aligned to 64 bytes for AVX-512 optimal access
typedef struct {
    // Feature transformer weights: [INPUT_BUCKETS][INPUT_SIZE][HIDDEN_SIZE]
    alignas(64) int16_t ft_weights[NNUE_INPUT_BUCKETS][NNUE_INPUT_SIZE][NNUE_HIDDEN_SIZE];
    // Feature transformer biases: [HIDDEN_SIZE]
    alignas(64) int16_t ft_biases[NNUE_HIDDEN_SIZE];

#if NNUE_ARCH == NNUE_ARCH_DEEP
    // L1: i8, [bucket][out][in], in = stm pairwise (HIDDEN/2) | ntm pairwise (HIDDEN/2), scale 2^14 with biases
    alignas(64) int8_t l1_weights[NNUE_OUTPUT_BUCKETS][NNUE_L2_SIZE][NNUE_L1_IN];
    alignas(64) int32_t l1_biases[NNUE_OUTPUT_BUCKETS][NNUE_L2_SIZE];
    // L2: in = [crelu(x) | crelu(x^2)] of the L1 outputs, weights x Q, biases x Q^3
    alignas(64) int32_t l2_weights[NNUE_OUTPUT_BUCKETS][NNUE_L3_SIZE][2 * NNUE_L2_SIZE];
    alignas(64) int32_t l2_biases[NNUE_OUTPUT_BUCKETS][NNUE_L3_SIZE];
    // L3: weights x Q, biases x Q^4
    alignas(64) int32_t l3_weights[NNUE_OUTPUT_BUCKETS][NNUE_L3_SIZE];
    alignas(64) int32_t l3_biases[NNUE_OUTPUT_BUCKETS];
    // Not part of the file, derived at load time (nnue_deep_prepare):
    // L1 weights re-laid out for the sparse L1 pass (as in pawnocchio): the u8 pairwise
    // activations are read as 4-byte groups, [bucket][group g][out][4] = w[out][4g..4g+3],
    // so one nonzero group updates all 16 outputs with two dpbusd and zero groups are skipped.
    alignas(64) int8_t l1_weights_sparse[NNUE_OUTPUT_BUCKETS][NNUE_L1_IN / 4][NNUE_L2_SIZE][4];
    // L2 weights as i16 (bullet clips weights to +-1.98, so |w x Q| <= 127) in the same pair
    // layout: [bucket][input pair][out][2]; L2 inputs are <= Q^2 = 4096 and fit i16 as well
    alignas(64) int16_t l2_weights_sparse[NNUE_OUTPUT_BUCKETS][NNUE_L2_SIZE][NNUE_L3_SIZE][2];
#else
    // Output layer weights: [OUTPUT_BUCKETS][2 * HIDDEN_SIZE] (both perspectives concatenated)
    alignas(64) int16_t output_weights[NNUE_OUTPUT_BUCKETS][2 * NNUE_HIDDEN_SIZE];
    // Output layer biases: [OUTPUT_BUCKETS]
    alignas(64) int16_t output_biases[NNUE_OUTPUT_BUCKETS];
#endif

    bool loaded;
} NNUENetwork;

// Global network instance (deprecated - weights now in NNUEAccumulator)
extern NNUENetwork nnue_net;

// Input bucket map for king position

// Initialize NNUE with random weights (for testing without a trained net)
void nnue_init_random(void);

// Allocate a zeroed, 64-byte aligned network (the struct requires it for SIMD). Release with free().
NNUENetwork* nnue_alloc_network(void);

// Load NNUE weights from file into network
bool nnue_load(const char* filename, NNUENetwork* net);

// Load NNUE weights from an in-memory copy of the quantised network file.
bool nnue_load_memory(const unsigned char* data, size_t size, NNUENetwork* net);

// FNV-1a 64-bit hash of a quantised network file (as logged by nnue_load_memory).
uint64_t nnue_hash(const unsigned char* data, size_t size);

// Save NNUE weights to file
bool nnue_save(const char* filename, const NNUENetwork* net);

// Compute full accumulator from scratch - needs network for initial computation
void nnue_reset_accumulator(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net);

// Refresh accumulator from current board state
void nnue_refresh_accumulator(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net);

// Prepare/copy-make a child accumulator frame. The child is updated lazily on eval.
void nnue_prepare_child_accumulator(NNUEAccumulator* child, NNUEAccumulator* parent);

// Mark a prepared child accumulator as dirty for the move just made.
void nnue_mark_accumulator_dirty(NNUEAccumulator* acc, const Board* board, int from_sq, int to_sq,
                                 int piece_type, int captured_piece_type, bool is_white,
                                 bool is_en_passant, bool requires_refresh);

// Materialize pending lazy updates up to this accumulator.
void nnue_materialize_accumulator(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net);

// Evaluate position using NNUE - only needs accumulator
int nnue_evaluate(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net);

// Get output bucket based on piece count
int nnue_get_output_bucket(const Board* board);

// True when a king move changes the moving side's input bucket or mirror state.
bool nnue_king_move_requires_refresh(int from_sq, int to_sq, bool is_white);

// Apply move incrementally - needs network for weights
void nnue_apply_move(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net, int from_sq, int to_sq, 
                     int piece_type, int captured_piece_type, bool is_white, bool is_en_passant);

// Undo move incrementally - needs network for weights
void nnue_undo_move(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net, int from_sq, int to_sq,
                    int piece_type, int captured_piece_type, bool is_white, bool is_en_passant);

#endif // NNUE_H
