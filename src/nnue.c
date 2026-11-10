#include "nnue.h"
#include "bitboard_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#ifdef _WIN32
#include <malloc.h>  // _aligned_malloc / _aligned_free
#endif

// SIMD support - compile-time detection (use -march=native)
#if defined(__AVX512F__) && defined(__AVX512BW__)
    #define NNUE_AVX512
    #include <immintrin.h>
#elif defined(__AVX2__)
    #define NNUE_AVX2
    #include <immintrin.h>
#endif

// Input bucket map based on king position (32 elements for half-board with mirroring)
// Index = rank * 8 + file (files 4-7 are mirrored from files 0-3)
// Must match BUCKET_LAYOUT in the training script of the selected architecture.
#if NNUE_IS_DEEP
// 16 buckets (training/training_deep.rs, layout from bullet's advanced example)
const int NNUE_INPUT_BUCKET_MAP[64] = {
     0,  1,  2,  3,  3,  2,  1,  0,  // Rank 1
     4,  5,  6,  7,  7,  6,  5,  4,  // Rank 2
     8,  8,  9,  9,  9,  9,  8,  8,  // Rank 3
    10, 10, 11, 11, 11, 11, 10, 10,  // Rank 4
    12, 12, 13, 13, 13, 13, 12, 12,  // Rank 5
    12, 12, 13, 13, 13, 13, 12, 12,  // Rank 6
    14, 14, 15, 15, 15, 15, 14, 14,  // Rank 7
    14, 14, 15, 15, 15, 15, 14, 14,  // Rank 8
};
#else
// 10 buckets: fine-grained bucketing based on king position from bullet
// Expanded from [(idx / 8) * 4 + [0, 1, 2, 3, 3, 2, 1, 0][idx % 8]]
const int NNUE_INPUT_BUCKET_MAP[64] = {
    0, 1, 2, 3, 3, 2, 1, 0,  // Rank 1
    4, 4, 5, 5, 5, 5, 4, 4,  // Rank 2
    6, 6, 6, 6, 6, 6, 6, 6,  // Rank 3
    7, 7, 7, 7, 7, 7, 7, 7,  // Rank 4
    8, 8, 8, 8, 8, 8, 8, 8,  // Rank 5
    8, 8, 8, 8, 8, 8, 8, 8,  // Rank 6
    9, 9, 9, 9, 9, 9, 9, 9,  // Rank 7
    9, 9, 9, 9, 9, 9, 9, 9,  // Rank 8
};
#endif

// Bumped whenever weights are (re)loaded so the Finny table can be invalidated
static uint32_t net_generation = 1;

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

bool nnue_king_move_requires_refresh(int from_sq, int to_sq, bool is_white) {
    int perspective = is_white ? 0 : 1;
    KingBucket from_bucket = get_king_bucket(from_sq, perspective);
    KingBucket to_bucket = get_king_bucket(to_sq, perspective);
    return from_bucket.index != to_bucket.index || from_bucket.mirrored != to_bucket.mirrored;
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

// The weight arrays in the order bullet writes them (see SavedFormat in the training script)
typedef struct {
    void* ptr;
    size_t size;
} NNUESection;

// net may be NULL to only query the sizes (sizeof does not evaluate its operand)
#define NNUE_SECTION(field) (NNUESection){ net ? (void*)net->field : NULL, sizeof(net->field) }

static int nnue_sections(NNUENetwork* net, NNUESection* out) {
    int n = 0;
    out[n++] = NNUE_SECTION(ft_weights);
#if NNUE_ARCH == NNUE_ARCH_THREATS
    out[n++] = NNUE_SECTION(tp_weights);
#endif
    out[n++] = NNUE_SECTION(ft_biases);
#if NNUE_IS_DEEP
    out[n++] = NNUE_SECTION(l1_weights);
    out[n++] = NNUE_SECTION(l1_biases);
    out[n++] = NNUE_SECTION(l2_weights);
    out[n++] = NNUE_SECTION(l2_biases);
    out[n++] = NNUE_SECTION(l3_weights);
    out[n++] = NNUE_SECTION(l3_biases);
#else
    out[n++] = NNUE_SECTION(output_weights);
    out[n++] = NNUE_SECTION(output_biases);
#endif
    return n;
}

#define NNUE_MAX_SECTIONS 9

static size_t nnue_parameter_size(void) {
    NNUESection sections[NNUE_MAX_SECTIONS];
    int n = nnue_sections(NULL, sections);
    size_t size = 0;
    for (int i = 0; i < n; i++) size += sections[i].size;
    return size;
}

// bullet pads the quantised file to a multiple of 64 bytes
static size_t nnue_file_size(void) {
    return (nnue_parameter_size() + 63) / 64 * 64;
}

uint64_t nnue_hash(const unsigned char* data, size_t size) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

#if NNUE_IS_DEEP
// Derived (non-file) data, refreshed after every load
#if defined(NNUE_AVX2) || defined(NNUE_AVX512)
static void deep_init_nnz_lut(void);
#endif

#if NNUE_ARCH == NNUE_ARCH_THREATS && (defined(NNUE_AVX2) || defined(NNUE_AVX512))
// In place, per 32-value block of every row: byte 2k = w[k] + 128 (as unsigned), byte 2k + 1 =
// w[16 + k] (see tp_apply). tp_weights is not in file order afterwards.
static void tp_interleave_weights(NNUENetwork* net) {
    for (int f = 0; f < NNUE_TP_INPUTS; f++) {
        int8_t* row = net->tp_weights[f];
        for (int b = 0; b < NNUE_HIDDEN_SIZE; b += 32) {
            int8_t tmp[32];
            for (int k = 0; k < 16; k++) {
                tmp[2 * k] = (int8_t)(row[b + k] ^ 0x80);
                tmp[2 * k + 1] = row[b + 16 + k];
            }
            memcpy(row + b, tmp, sizeof(tmp));
        }
    }
}
#endif

static void nnue_deep_prepare(NNUENetwork* net) {
#if defined(NNUE_AVX2) || defined(NNUE_AVX512)
    deep_init_nnz_lut();
#endif
#if NNUE_ARCH == NNUE_ARCH_THREATS
    nnue_tp_init();
#if defined(NNUE_AVX2) || defined(NNUE_AVX512)
    tp_interleave_weights(net);
#endif
#endif
    for (int b = 0; b < NNUE_OUTPUT_BUCKETS; b++) {
        for (int o = 0; o < NNUE_L2_SIZE; o++) {
            for (int i = 0; i < NNUE_L1_IN; i++) {
                net->l1_weights_sparse[b][i / 4][o][i % 4] = net->l1_weights[b][o][i];
            }
        }
        for (int o = 0; o < NNUE_L3_SIZE; o++) {
            for (int i = 0; i < 2 * NNUE_L2_SIZE; i++) {
                const int32_t w = net->l2_weights[b][o][i];
                if (w < INT16_MIN || w > INT16_MAX) {
                    fprintf(stderr, "info string NNUE L2 weight %d out of i16 range\n", (int)w);
                    exit(1);
                }
                net->l2_weights_sparse[b][i / 2][o][i % 2] = (int16_t)w;
            }
        }
    }
}
#endif

bool nnue_load_memory(const unsigned char* data, size_t size, NNUENetwork* net) {
    const size_t expected_size = nnue_file_size();
    if (net) net->loaded = false;
    if (!data || !net) {
        fprintf(stderr, "info string NNUE memory buffer or network is NULL\n");
        return false;
    }
    if (size != expected_size) {
        fprintf(stderr, "info string NNUE data size mismatch! Got %zu, expected %zu\n",
                size, expected_size);
        return false;
    }

    const unsigned char* cursor = data;
    NNUESection sections[NNUE_MAX_SECTIONS];
    int n = nnue_sections(net, sections);
    for (int i = 0; i < n; i++) {
        memcpy(sections[i].ptr, cursor, sections[i].size);
        cursor += sections[i].size;
    }
#if NNUE_IS_DEEP
    nnue_deep_prepare(net);
#endif

    net->loaded = true;
    net_generation++;
    printf("info string NNUE loaded (size %zu, fnv1a64 %016" PRIx64 ")\n",
           size, nnue_hash(data, size));
    return true;
}

NNUENetwork* nnue_alloc_network(void) {
    size_t size = (sizeof(NNUENetwork) + 63) / 64 * 64;  // aligned_alloc needs a multiple of the alignment
#ifdef _WIN32
    // The Windows CRT has no aligned_alloc; its aligned blocks need _aligned_free.
    NNUENetwork* net = _aligned_malloc(size, 64);
#else
    NNUENetwork* net = aligned_alloc(64, size);
#endif
    if (net) memset(net, 0, size);
    return net;
}

void nnue_free_network(NNUENetwork* net) {
#ifdef _WIN32
    _aligned_free(net);
#else
    free(net);
#endif
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
    
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long file_size = ftell(file);
    if (file_size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    size_t size = (size_t)file_size;
    const size_t expected_size = nnue_file_size();
    if (size != expected_size) {
        fprintf(stderr, "info string NNUE file size mismatch! Got %zu, expected %zu\n",
                size, expected_size);
        fclose(file);
        net->loaded = false;
        return false;
    }
    unsigned char* data = malloc(size == 0 ? 1 : size);
    if (!data) {
        fclose(file);
        fprintf(stderr, "info string Failed to allocate NNUE input buffer\n");
        return false;
    }

    size_t bytes_read = fread(data, 1, size, file);
    fclose(file);
    if (bytes_read != size) {
        fprintf(stderr, "info string Failed to read complete NNUE file\n");
        free(data);
        return false;
    }

    bool loaded = nnue_load_memory(data, size, net);
    free(data);
    if (loaded) printf("info string NNUE source: %s\n", filename);
    return loaded;
}

// Save NNUE weights to file
bool nnue_save(const char* filename, const NNUENetwork* net) {
    if (!filename || !net) return false;
    FILE* file = fopen(filename, "wb");
    if (!file) return false;
    
    NNUESection sections[NNUE_MAX_SECTIONS];
    int n = nnue_sections((NNUENetwork*)net, sections);
    size_t written = 0;
    for (int i = 0; i < n; i++) {
        fwrite(sections[i].ptr, 1, sections[i].size, file);
        written += sections[i].size;
    }
    static const char pad[] = "bullet";
    for (size_t i = written; i < nnue_file_size(); i++) fputc(pad[(i - written) % 6], file);
    
    fclose(file);
    return true;
}

// SIMD vector operations - compile-time selected
static inline void vec_add(int16_t* restrict dst, const int16_t* restrict src, int size) {
#if defined(NNUE_AVX512)
    for (int i = 0; i < size; i += 32) {
        __m512i d = _mm512_load_si512((const __m512i*)(dst + i));
        __m512i s = _mm512_loadu_si512((const __m512i*)(src + i));
        _mm512_store_si512((__m512i*)(dst + i), _mm512_add_epi16(d, s));
    }
#elif defined(NNUE_AVX2)
    for (int i = 0; i < size; i += 16) {
        __m256i d = _mm256_load_si256((const __m256i*)(dst + i));
        __m256i s = _mm256_loadu_si256((const __m256i*)(src + i));
        _mm256_store_si256((__m256i*)(dst + i), _mm256_add_epi16(d, s));
    }
#else
    for (int i = 0; i < size; i++) {
        dst[i] += src[i];
    }
#endif
}

static inline void vec_sub(int16_t* restrict dst, const int16_t* restrict src, int size) {
#if defined(NNUE_AVX512)
    for (int i = 0; i < size; i += 32) {
        __m512i d = _mm512_load_si512((const __m512i*)(dst + i));
        __m512i s = _mm512_loadu_si512((const __m512i*)(src + i));
        _mm512_store_si512((__m512i*)(dst + i), _mm512_sub_epi16(d, s));
    }
#elif defined(NNUE_AVX2)
    for (int i = 0; i < size; i += 16) {
        __m256i d = _mm256_load_si256((const __m256i*)(dst + i));
        __m256i s = _mm256_loadu_si256((const __m256i*)(src + i));
        _mm256_store_si256((__m256i*)(dst + i), _mm256_sub_epi16(d, s));
    }
#else
    for (int i = 0; i < size; i++) {
        dst[i] -= src[i];
    }
#endif
}

// SIMD memcpy for bias initialization
static inline void vec_copy(int16_t* restrict dst, const int16_t* restrict src, int size) {
#if defined(NNUE_AVX512)
    for (int i = 0; i < size; i += 32) {
        __m512i v = _mm512_loadu_si512((const __m512i*)(src + i));
        _mm512_store_si512((__m512i*)(dst + i), v);
    }
#elif defined(NNUE_AVX2)
    for (int i = 0; i < size; i += 16) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(src + i));
        _mm256_store_si256((__m256i*)(dst + i), v);
    }
#else
    memcpy(dst, src, size * sizeof(int16_t));
#endif
}

// Finny table: per perspective and king bucket (incl. mirror state) the accumulator of the last
// position refreshed into that bucket plus the piece bitboards it was built from. A refresh then only
// applies the piece differences instead of re-adding every piece.
typedef struct {
    alignas(64) int16_t values[NNUE_HIDDEN_SIZE];
    Bitboard pieces[2][6];
    bool valid;
} FinnyEntry;

static FinnyEntry finny_cache[2][NNUE_INPUT_BUCKETS * 2];
static const NNUENetwork* finny_net = NULL;
static uint32_t finny_generation = 0;

static void finny_refresh_perspective(const Board* board, int16_t* dst, const NNUENetwork* net,
                                      int perspective, KingBucket bucket) {
    if (finny_net != net || finny_generation != net_generation) {
        for (int p = 0; p < 2; p++)
            for (int b = 0; b < NNUE_INPUT_BUCKETS * 2; b++) finny_cache[p][b].valid = false;
        finny_net = net;
        finny_generation = net_generation;
    }

    FinnyEntry* entry = &finny_cache[perspective][bucket.index * 2 + (bucket.mirrored ? 1 : 0)];
    if (!entry->valid) {
        vec_copy(entry->values, net->ft_biases, NNUE_HIDDEN_SIZE);
        memset(entry->pieces, 0, sizeof(entry->pieces));
        entry->valid = true;
    }

    for (int color = 0; color < 2; color++) {
        for (int type = 0; type < 6; type++) {
            Bitboard cur = board->byTypeBB[color][type];
            Bitboard old = entry->pieces[color][type];
            Bitboard added = cur & ~old;
            Bitboard removed = old & ~cur;
            while (added) {
                int sq = get_lsb(added);
                added &= added - 1;
                int idx = get_feature_index(perspective, type, color, sq, bucket);
                vec_add(entry->values, net->ft_weights[idx / NNUE_INPUT_SIZE][idx % NNUE_INPUT_SIZE],
                        NNUE_HIDDEN_SIZE);
            }
            while (removed) {
                int sq = get_lsb(removed);
                removed &= removed - 1;
                int idx = get_feature_index(perspective, type, color, sq, bucket);
                vec_sub(entry->values, net->ft_weights[idx / NNUE_INPUT_SIZE][idx % NNUE_INPUT_SIZE],
                        NNUE_HIDDEN_SIZE);
            }
            entry->pieces[color][type] = cur;
        }
    }

    vec_copy(dst, entry->values, NNUE_HIDDEN_SIZE);
}

// Compute full accumulator (via the Finny table, only piece differences are applied)
void nnue_refresh_accumulator(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net) {
    if (acc == NULL || net == NULL || board == NULL) return;

    int white_king_sq = get_lsb(board->whiteKings);
    int black_king_sq = get_lsb(board->blackKings);

    if (white_king_sq < 0 || black_king_sq < 0) {
        memset(acc->white, 0, sizeof(acc->white));
        memset(acc->black, 0, sizeof(acc->black));
        acc->computed = false;
        acc->dirty = false;
        acc->requires_refresh = false;
        acc->previous = NULL;
        return;
    }

    finny_refresh_perspective(board, acc->white, net, 0, get_king_bucket(white_king_sq, 0));
    finny_refresh_perspective(board, acc->black, net, 1, get_king_bucket(black_king_sq, 1));

    acc->computed = true;
    acc->dirty = false;
    acc->requires_refresh = false;
    acc->previous = NULL;
#if NNUE_ARCH == NNUE_ARCH_THREATS
    acc->tp_computed[0] = acc->tp_computed[1] = false;  // rebuilt from the board on the next eval
    acc->tp_state = NNUE_TP_STATE_NONE;
#endif
}

#if NNUE_ARCH == NNUE_ARCH_THREATS
// PSQT-only refresh inside the lazy chain: keeps the threat state and the link to the parent,
// which the threat materialisation still needs
static void psqt_refresh_in_chain(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net) {
    NNUEAccumulator* previous = acc->previous;
    const int8_t state = acc->tp_state;
    const bool computed[2] = { acc->tp_computed[0], acc->tp_computed[1] };
    nnue_refresh_accumulator(board, acc, net);
    acc->previous = previous;
    acc->tp_state = state;
    acc->tp_computed[0] = computed[0];
    acc->tp_computed[1] = computed[1];
}
#else
#define psqt_refresh_in_chain nnue_refresh_accumulator
#endif

// Reset accumulator for new position using network
void nnue_reset_accumulator(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net) {
    if (acc == NULL || net == NULL) return;
    nnue_refresh_accumulator(board, acc, net);
}

void nnue_prepare_child_accumulator(NNUEAccumulator* child, NNUEAccumulator* parent) {
    if (child == NULL) return;

    child->computed = false;
#if NNUE_ARCH == NNUE_ARCH_THREATS
    child->tp_computed[0] = child->tp_computed[1] = false;
    child->tp_state = NNUE_TP_STATE_PREPARED;
#endif
    child->dirty = true;
    child->requires_refresh = false;
    child->previous = parent;
    child->from_sq = -1;
    child->to_sq = -1;
    child->piece_type = -1;
    child->captured_piece_type = -1;
    child->moving_color = -1;
    child->capture_sq = -1;
    child->white_king_sq = -1;
    child->black_king_sq = -1;
}

void nnue_mark_accumulator_dirty(NNUEAccumulator* acc, const Board* board, int from_sq, int to_sq,
                                 int piece_type, int captured_piece_type, bool is_white,
                                 bool is_en_passant, bool requires_refresh) {
    if (acc == NULL) return;

    acc->computed = false;
#if NNUE_ARCH == NNUE_ARCH_THREATS
    acc->tp_computed[0] = acc->tp_computed[1] = false;
#endif
    acc->dirty = true;
    acc->requires_refresh = requires_refresh;
    acc->from_sq = from_sq;
    acc->to_sq = to_sq;
    acc->piece_type = piece_type;
    acc->captured_piece_type = captured_piece_type;
    acc->moving_color = is_white ? 0 : 1;
    acc->capture_sq = -1;
    acc->white_king_sq = board ? get_lsb(board->whiteKings) : -1;
    acc->black_king_sq = board ? get_lsb(board->blackKings) : -1;

    if (captured_piece_type >= 0) {
        acc->capture_sq = is_en_passant ? (is_white ? to_sq - 8 : to_sq + 8) : to_sq;
    }
}

// SCReLU² dot product using Leorik's trick:
// Instead of (a * a) * w (overflow since 255²=65025 > int16_max)
// Compute (a * w) * a, then use madd_epi16 for efficiency
// This works when weights are in reasonable range (typically [-127..127] after quantization)
static inline int32_t screlu_dot(const int16_t* acc, const int16_t* weights, int size) {
#if defined(NNUE_AVX512)
    const __m512i zero = _mm512_setzero_si512();
    const __m512i qa = _mm512_set1_epi16(NNUE_QA);
    __m512i sum = _mm512_setzero_si512();

    for (int i = 0; i < size; i += 32) {
        __m512i a = _mm512_load_si512((const __m512i*)(acc + i));
        a = _mm512_max_epi16(a, zero);
        a = _mm512_min_epi16(a, qa);

        __m512i w = _mm512_loadu_si512((const __m512i*)(weights + i));

        // (a * w) * a using madd - same trick as AVX2
        __m512i aw = _mm512_mullo_epi16(a, w);
        sum = _mm512_add_epi32(sum, _mm512_madd_epi16(aw, a));
    }

    return _mm512_reduce_add_epi32(sum);

#elif defined(NNUE_AVX2)
    const __m256i zero = _mm256_setzero_si256();
    const __m256i qa = _mm256_set1_epi16(NNUE_QA);
    __m256i sum = _mm256_setzero_si256();

    for (int i = 0; i < size; i += 16) {
        __m256i a = _mm256_load_si256((const __m256i*)(acc + i));
        a = _mm256_max_epi16(a, zero);
        a = _mm256_min_epi16(a, qa);

        __m256i w = _mm256_loadu_si256((const __m256i*)(weights + i));

        // (a * w) * a using madd
        __m256i aw = _mm256_mullo_epi16(a, w);
        sum = _mm256_add_epi32(sum, _mm256_madd_epi16(aw, a));
    }

    // Horizontal sum
    __m128i sum128 = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, _MM_SHUFFLE(1, 0, 3, 2)));
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_cvtsi128_si32(sum128);

#else
    int32_t output = 0;
    for (int i = 0; i < size; i++) {
        int32_t clamped = acc[i];
        clamped = clamped < 0 ? 0 : (clamped > NNUE_QA ? NNUE_QA : clamped);
        output += clamped * clamped * weights[i];
    }
    return output;
#endif
}

#if NNUE_IS_DEEP
// ---------------------------------------------------------------------------------------------
// Deep architecture forward pass, after pawnocchio's src/nnue/outputs/multilayer.zig (the
// engine bullet's examples/advanced comes from). Integer scales (see training/training_deep.rs):
//   FT accumulator          x 255                  -> a = clamp(acc, 0, 255), b = min(acc, 255)
//   pairwise (a*b) >> 9     x 255^2/512 (~127)     -> u8 in [0, 127], negative products -> 0
//   L1 out                  x 2^14  (i8 weights x 128/(255/256)^2 -> 255^2/512 * that = 2^14,
//                                    = the bias scale), shifted up to 2^20 for the activation
//   L2 in                   x Q^2 = 2^12: [clamp(z, 0, 1), min(z^2, 1)], rounded
//   L2 out                  x Q^3, crelu
//   L3 out                  x Q^4  -> eval = out * NNUE_SCALE / Q^4
// The activation fits 7 bits so that the maddubs fallback (no VNNI) cannot saturate:
// 2 * 127 * 127 < 2^15.
// ---------------------------------------------------------------------------------------------
#define NNUE_FT_HALF     (NNUE_HIDDEN_SIZE / 2)
#define NNUE_PAIR_SHIFT  9
#define NNUE_L1_SHIFT    20
#define NNUE_L1_ONE      (1 << NNUE_L1_SHIFT)
#define NNUE_Q3          (NNUE_Q * NNUE_Q * NNUE_Q)
#define NNUE_Q4          ((int64_t)NNUE_Q3 * NNUE_Q)
// 4-byte groups of u8 activations (the unit of the sparse L1)
#define NNUE_L1_GROUPS   (NNUE_L1_IN / 4)

// Reads 4 bytes of an u8/i16 buffer as one i32 without breaking strict aliasing
// (a plain int32_t* read lets GCC move it before the stores of the buffer)
static inline int32_t load_i32(const void* p) {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

#if defined(NNUE_AVX2) || defined(NNUE_AVX512)
static alignas(16) uint16_t nnz_lut[256][8];

static void deep_init_nnz_lut(void) {
    for (int m = 0; m < 256; m++) {
        int n = 0;
        for (int j = 0; j < 8; j++) {
            if (m & (1 << j)) nnz_lut[m][n++] = (uint16_t)j;
        }
    }
}
#endif

// Pairwise CReLU of one perspective: out[i] = clamp(acc[i], 0, 255) * min(acc[i + HALF], 255) >> 9,
// negative products saturate to 0. With SIMD the indices of the nonzero 4-byte groups are
// collected on the fly: appended to idx[count..] offset by group_base, new count returned.
static inline int deep_ft_activate(const int16_t* acc, const int16_t* tp, uint8_t* out, uint16_t* idx, int count,
                                   int group_base) {
#if NNUE_ARCH != NNUE_ARCH_THREATS
    (void)tp;
#endif
#if defined(NNUE_AVX2) || defined(NNUE_AVX512)
    const __m256i zero = _mm256_setzero_si256();
    const __m256i qa = _mm256_set1_epi16(NNUE_QA);
    const __m128i step = _mm_set1_epi16(8);
    __m128i base = _mm_set1_epi16((short)group_base);
    for (int i = 0; i < NNUE_FT_HALF; i += 32) {
        __m256i a0 = _mm256_load_si256((const __m256i*)(acc + i));
        __m256i a1 = _mm256_load_si256((const __m256i*)(acc + i + 16));
        __m256i b0 = _mm256_load_si256((const __m256i*)(acc + NNUE_FT_HALF + i));
        __m256i b1 = _mm256_load_si256((const __m256i*)(acc + NNUE_FT_HALF + i + 16));
#if NNUE_ARCH == NNUE_ARCH_THREATS
        // PSQT + threats; saturation cannot change the result, everything is clamped to [0, 255] below
        a0 = _mm256_adds_epi16(a0, _mm256_load_si256((const __m256i*)(tp + i)));
        a1 = _mm256_adds_epi16(a1, _mm256_load_si256((const __m256i*)(tp + i + 16)));
        b0 = _mm256_adds_epi16(b0, _mm256_load_si256((const __m256i*)(tp + NNUE_FT_HALF + i)));
        b1 = _mm256_adds_epi16(b1, _mm256_load_si256((const __m256i*)(tp + NNUE_FT_HALF + i + 16)));
#endif
        a0 = _mm256_min_epi16(_mm256_max_epi16(a0, zero), qa);
        a1 = _mm256_min_epi16(_mm256_max_epi16(a1, zero), qa);
        b0 = _mm256_min_epi16(b0, qa);
        b1 = _mm256_min_epi16(b1, qa);
        // (a << 7) * b >> 16 = a*b >> 9 (a << 7 <= 32640 fits i16)
        __m256i p0 = _mm256_mulhi_epi16(_mm256_slli_epi16(a0, 16 - NNUE_PAIR_SHIFT), b0);
        __m256i p1 = _mm256_mulhi_epi16(_mm256_slli_epi16(a1, 16 - NNUE_PAIR_SHIFT), b1);
        // packus saturates negatives to 0 and interleaves 128-bit lanes; permute restores order
        __m256i u = _mm256_permute4x64_epi64(_mm256_packus_epi16(p0, p1), _MM_SHUFFLE(3, 1, 2, 0));
        _mm256_store_si256((__m256i*)(out + i), u);

        unsigned mask = 0xFFu & ~(unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(u, zero)));
        __m128i offsets = _mm_load_si128((const __m128i*)nnz_lut[mask]);
        _mm_storeu_si128((__m128i*)(idx + count), _mm_add_epi16(base, offsets));
        count += __builtin_popcount(mask);
        base = _mm_add_epi16(base, step);
    }
    return count;
#else
    (void)idx;
    (void)group_base;
    for (int i = 0; i < NNUE_FT_HALF; i++) {
#if NNUE_ARCH == NNUE_ARCH_THREATS
        const int32_t va = acc[i] + tp[i], vb = acc[i + NNUE_FT_HALF] + tp[i + NNUE_FT_HALF];
#else
        const int32_t va = acc[i], vb = acc[i + NNUE_FT_HALF];
#endif
        int32_t a = va < 0 ? 0 : (va > NNUE_QA ? NNUE_QA : va);
        int32_t b = vb > NNUE_QA ? NNUE_QA : vb;
        int32_t p = (a * b) >> NNUE_PAIR_SHIFT;  // arithmetic shift = floor, like mulhi
        out[i] = (uint8_t)(p < 0 ? 0 : p);
    }
    return count;
#endif
}

#if defined(NNUE_AVX2) || defined(NNUE_AVX512)
// acc + sum over 4-byte groups of u8 x i8 (VNNI dpbusd; maddubs fallback cannot saturate, see above)
static inline __m256i dpbusd256(__m256i acc, __m256i u, __m256i w) {
#if defined(__AVXVNNI__)
    return _mm256_dpbusd_avx_epi32(acc, u, w);
#elif defined(__AVX512VNNI__) && defined(__AVX512VL__)
    return _mm256_dpbusd_epi32(acc, u, w);
#else
    return _mm256_add_epi32(acc, _mm256_madd_epi16(_mm256_maddubs_epi16(u, w), _mm256_set1_epi16(1)));
#endif
}

// acc + madd_epi16(a, b), fused into one instruction with AVX-VNNI (bit-identical)
static inline __m256i dpwssd256(__m256i acc, __m256i a, __m256i b) {
#if defined(__AVXVNNI__)
    return _mm256_dpwssd_avx_epi32(acc, a, b);
#elif defined(__AVX512VNNI__) && defined(__AVX512VL__)
    return _mm256_dpwssd_epi32(acc, a, b);
#else
    return _mm256_add_epi32(acc, _mm256_madd_epi16(a, b));
#endif
}
#endif

// Sparse L1: each nonzero 4-byte group g of activations adds its weight column
// l1_weights_sparse[bucket][g] (64 bytes) to all 16 outputs with two dpbusd. z at scale 2^14.
static inline void deep_l1_forward(const uint8_t* ft_out, const uint16_t* idx, int count,
                                   const NNUENetwork* net, int bucket, int32_t* z) {
    const int8_t* w = &net->l1_weights_sparse[bucket][0][0][0];
#if defined(NNUE_AVX2) || defined(NNUE_AVX512)
    // four independent accumulator pairs for ILP
    __m256i lo[4], hi[4];
    for (int j = 0; j < 4; j++) lo[j] = hi[j] = _mm256_setzero_si256();
    int k = 0;
    for (; k + 4 <= count; k += 4) {
        for (int j = 0; j < 4; j++) {
            const int g = idx[k + j];
            const __m256i x = _mm256_set1_epi32(load_i32(ft_out + 4 * g));
            const __m256i* wg = (const __m256i*)(w + g * 4 * NNUE_L2_SIZE);
            lo[j] = dpbusd256(lo[j], x, _mm256_load_si256(wg));
            hi[j] = dpbusd256(hi[j], x, _mm256_load_si256(wg + 1));
        }
    }
    for (; k < count; k++) {
        const int g = idx[k];
        const __m256i x = _mm256_set1_epi32(load_i32(ft_out + 4 * g));
        const __m256i* wg = (const __m256i*)(w + g * 4 * NNUE_L2_SIZE);
        lo[0] = dpbusd256(lo[0], x, _mm256_load_si256(wg));
        hi[0] = dpbusd256(hi[0], x, _mm256_load_si256(wg + 1));
    }
    lo[0] = _mm256_add_epi32(_mm256_add_epi32(lo[0], lo[1]), _mm256_add_epi32(lo[2], lo[3]));
    hi[0] = _mm256_add_epi32(_mm256_add_epi32(hi[0], hi[1]), _mm256_add_epi32(hi[2], hi[3]));
    _mm256_storeu_si256((__m256i*)z, lo[0]);
    _mm256_storeu_si256((__m256i*)(z + 8), hi[0]);
#else
    (void)idx;
    (void)count;
    for (int o = 0; o < NNUE_L2_SIZE; o++) z[o] = 0;
    for (int g = 0; g < NNUE_L1_GROUPS; g++) {
        const uint8_t* x = ft_out + 4 * g;
        if ((x[0] | x[1] | x[2] | x[3]) == 0) continue;
        const int8_t* wg = w + g * 4 * NNUE_L2_SIZE;
        for (int o = 0; o < NNUE_L2_SIZE; o++) {
            z[o] += x[0] * wg[4 * o] + x[1] * wg[4 * o + 1] + x[2] * wg[4 * o + 2] + x[3] * wg[4 * o + 3];
        }
    }
#endif
}

// L2: in = 32 i16 values (16 pairs), out = crelu(sum + bias) clamped to Q^3, 32 i32.
// Pairs of i16 inputs with two-wide i16 weight columns (madd scheme), zero pairs skipped.
static inline void deep_l2_forward(const int16_t* in, const NNUENetwork* net, int bucket, int32_t* out) {
    const int16_t* w = &net->l2_weights_sparse[bucket][0][0][0];
    const int32_t* bias = net->l2_biases[bucket];
#if defined(NNUE_AVX2) || defined(NNUE_AVX512)
    __m256i s[4];
    for (int j = 0; j < 4; j++) s[j] = _mm256_loadu_si256((const __m256i*)(bias + 8 * j));
    for (int p = 0; p < NNUE_L2_SIZE; p++) {
        const int32_t pair = load_i32(in + 2 * p);
        if (pair == 0) continue;
        const __m256i x = _mm256_set1_epi32(pair);
        const __m256i* wp = (const __m256i*)(w + p * 2 * NNUE_L3_SIZE);
        for (int j = 0; j < 4; j++) s[j] = dpwssd256(s[j], x, _mm256_load_si256(wp + j));
    }
    const __m256i zero = _mm256_setzero_si256();
    const __m256i q3 = _mm256_set1_epi32(NNUE_Q3);
    for (int j = 0; j < 4; j++) {
        _mm256_storeu_si256((__m256i*)(out + 8 * j), _mm256_min_epi32(_mm256_max_epi32(s[j], zero), q3));
    }
#else
    for (int o = 0; o < NNUE_L3_SIZE; o++) out[o] = bias[o];
    for (int p = 0; p < NNUE_L2_SIZE; p++) {
        const int32_t lo = in[2 * p], hi = in[2 * p + 1];
        if ((lo | hi) == 0) continue;
        const int16_t* wp = w + p * 2 * NNUE_L3_SIZE;
        for (int o = 0; o < NNUE_L3_SIZE; o++) out[o] += lo * wp[2 * o] + hi * wp[2 * o + 1];
    }
    for (int o = 0; o < NNUE_L3_SIZE; o++) out[o] = out[o] < 0 ? 0 : (out[o] > NNUE_Q3 ? NNUE_Q3 : out[o]);
#endif
}

#if NNUE_ARCH == NNUE_ARCH_THREATS
// ---------------------------------------------------------------------------------------------
// Threat + pawn-pair accumulator, after pawnocchio (src/nnue/inputs/psq_threats.zig): each move's
// threat/pawn-pair changes are recorded as raw tuples (nnue_threats.c) - unlike pawnocchio not in
// applyMove itself but lazily, once the frame is evaluated or searched further. On eval the pending
// frames are materialised one by one from their parent (indices for the perspective, then one
// pass over the accumulator); if the king of a perspective changed its mirror state the half is
// rebuilt from the board instead.
// ---------------------------------------------------------------------------------------------
#define NNUE_TP_MAX_ACTIVE (NNUE_TP_MAX_THREATS + NNUE_TP_MAX_PAIRS)
#define NNUE_TP_MAX_ROWS   (NNUE_TP_MAX_DELTA + NNUE_TP_MAX_PAIR_DELTA)

// dst = (src, or 0 if NULL) + sum of the add rows - sum of the sub rows; the i8 rows are widened
// to i16.
static void tp_apply(int16_t* dst, const int16_t* src, const NNUENetwork* net,
                     const uint16_t* add, int n_add, const uint16_t* sub, int n_sub) {
#if defined(NNUE_AVX2) || defined(NNUE_AVX512)
    // Rows are interleaved per 32-byte block (tp_interleave_weights): a 16-bit lane holds a value of
    // the first 16 as its low byte, biased to unsigned (lo = w + 128), and one of the next 16 as its
    // signed high byte, so lane = lo + 256 * hi (mod 2^16). Per row only the lanes themselves (s1)
    // and the high bytes (s2) are summed, three uops per 32 values:
    //   sum of the high values = s2
    //   sum of the low values  = s1 - 256 * s2 - 128 * (rows added - rows subtracted)
    // Everything is exact modulo 2^16, like the i16 accumulator itself. Tiled so that the sums stay
    // in registers over all rows.
    enum { VECS = 6, TILE = VECS * 32 };
    const __m256i bias = _mm256_set1_epi16((int16_t)(128 * (n_add - n_sub)));
    for (int t = 0; t < NNUE_HIDDEN_SIZE; t += TILE) {
        __m256i s1[VECS], s2[VECS];
        for (int j = 0; j < VECS; j++) s1[j] = s2[j] = _mm256_setzero_si256();
        for (int k = 0; k < n_add; k++) {
            const int8_t* w = net->tp_weights[add[k]] + t;
            for (int j = 0; j < VECS; j++) {
                const __m256i x = _mm256_load_si256((const __m256i*)(w + 32 * j));
                s1[j] = _mm256_add_epi16(s1[j], x);
                s2[j] = _mm256_add_epi16(s2[j], _mm256_srai_epi16(x, 8));
            }
        }
        for (int k = 0; k < n_sub; k++) {
            const int8_t* w = net->tp_weights[sub[k]] + t;
            for (int j = 0; j < VECS; j++) {
                const __m256i x = _mm256_load_si256((const __m256i*)(w + 32 * j));
                s1[j] = _mm256_sub_epi16(s1[j], x);
                s2[j] = _mm256_sub_epi16(s2[j], _mm256_srai_epi16(x, 8));
            }
        }
        for (int j = 0; j < VECS; j++) {
            __m256i lo = _mm256_sub_epi16(_mm256_sub_epi16(s1[j], _mm256_slli_epi16(s2[j], 8)), bias);
            __m256i hi = s2[j];
            if (src) {
                lo = _mm256_add_epi16(lo, _mm256_load_si256((const __m256i*)(src + t + 32 * j)));
                hi = _mm256_add_epi16(hi, _mm256_load_si256((const __m256i*)(src + t + 32 * j + 16)));
            }
            _mm256_store_si256((__m256i*)(dst + t + 32 * j), lo);
            _mm256_store_si256((__m256i*)(dst + t + 32 * j + 16), hi);
        }
    }
#else
    for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) dst[i] = src ? src[i] : 0;
    for (int k = 0; k < n_add; k++)
        for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) dst[i] += net->tp_weights[add[k]][i];
    for (int k = 0; k < n_sub; k++)
        for (int i = 0; i < NNUE_HIDDEN_SIZE; i++) dst[i] -= net->tp_weights[sub[k]][i];
#endif
}

void nnue_note_threat_move(NNUEAccumulator* acc, int from, int to, int moving_piece, int captured_piece,
                           int capture_sq, bool castling) {
    if (acc == NULL) return;
    acc->tp_computed[0] = acc->tp_computed[1] = false;
    // a frame reused without nnue_prepare_child_accumulator (search at max ply) holds no usable delta
    if (acc->tp_state != NNUE_TP_STATE_PREPARED) {
        acc->tp_state = NNUE_TP_STATE_NONE;
        return;
    }
    acc->tp_move_from = (uint8_t)from;
    acc->tp_move_to = (uint8_t)to;
    acc->tp_move_piece = (uint8_t)moving_piece;
    acc->tp_move_captured = (uint8_t)captured_piece;
    acc->tp_move_capture_sq = (uint8_t)capture_sq;
    acc->tp_move_castling = castling;
    acc->tp_state = NNUE_TP_STATE_PENDING;
}

static void tp_build_half(const Board* board, NNUEAccumulator* acc, int p, const NNUENetwork* net);

// `board` must hold the frame's position (the board after the noted move)
void nnue_record_pending_threat_move(NNUEAccumulator* frame, const Board* board, const NNUENetwork* net) {
    frame->tp_mask[0] = (int8_t)nnue_tp_square_mask(board, 0);
    frame->tp_mask[1] = (int8_t)nnue_tp_square_mask(board, 1);
    // A king move across the d/e files changes the mirroring of its own perspective, that half cannot
    // be updated from the parent. Build it now while the board is at hand: later only the boards of
    // descendants are known, and each of them would have to be built from scratch instead.
    const int piece = frame->tp_move_piece;
    if (PIECE_TYPE_OF(piece) == KING && ((frame->tp_move_from ^ frame->tp_move_to) & 4)) {
        tp_build_half(board, frame, PIECE_COLOR_OF(piece), net);
    }
    const bool ok = nnue_tp_record_move(board, frame->tp_move_from, frame->tp_move_to, frame->tp_move_piece,
                                        frame->tp_move_captured, frame->tp_move_capture_sq,
                                        frame->tp_move_castling, &frame->tp_delta);
    frame->tp_state = ok ? NNUE_TP_STATE_RECORDED : NNUE_TP_STATE_NONE;
}

// pawnocchio buildThreatAccumulator: one half from scratch
static void tp_build_half(const Board* board, NNUEAccumulator* acc, int p, const NNUENetwork* net) {
    const int mask = nnue_tp_square_mask(board, p);
    uint32_t raw[NNUE_TP_MAX_THREATS];
    uint16_t raw_pairs[NNUE_TP_MAX_PAIRS];
    uint16_t idx[NNUE_TP_MAX_ACTIVE];
    const int n_raw = nnue_tp_raw_threats(board, raw);
    const int n_raw_pairs = nnue_tp_raw_pairs(board, raw_pairs);
    int n = nnue_tp_threat_indices(raw, n_raw, p, mask, idx);
    n += nnue_tp_pair_indices(raw_pairs, n_raw_pairs, p, mask, idx + n);
    tp_apply(acc->tp[p], NULL, net, idx, n, NULL, 0);
    acc->tp_mask[p] = (int8_t)mask;
    acc->tp_computed[p] = true;
}

// pawnocchio materialiseThreatHalf: frame = parent + the frame's recorded changes
static void tp_materialise_half(NNUEAccumulator* frame, const NNUEAccumulator* parent, int p,
                                const NNUENetwork* net) {
    const NNUETPDelta* d = &frame->tp_delta;
    const int mask = frame->tp_mask[p];
    uint16_t add[NNUE_TP_MAX_ROWS], sub[NNUE_TP_MAX_ROWS];
    int n_add = nnue_tp_threat_indices(d->add, d->n_add, p, mask, add);
    int n_sub = nnue_tp_threat_indices(d->sub, d->n_sub, p, mask, sub);
    n_add += nnue_tp_pair_indices(d->pair_add, d->n_pair_add, p, mask, add + n_add);
    n_sub += nnue_tp_pair_indices(d->pair_sub, d->n_pair_sub, p, mask, sub + n_sub);
    tp_apply(frame->tp[p], parent->tp[p], net, add, n_add, sub, n_sub);
    frame->tp_computed[p] = true;
}

// pawnocchio resolvePending, per perspective. Only the current board is known here (make/unmake),
// so a mirror change inside the pending chain can only be rebuilt for the frame being evaluated;
// the frames from that change on stay pending for this perspective.
static void tp_ensure(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net) {
    // the frame's own move is still pending if nothing was searched from it yet; its ancestors were
    // recorded when the next move was made from them
    if (acc->tp_state == NNUE_TP_STATE_PENDING) nnue_record_pending_threat_move(acc, board, net);
    for (int p = 0; p < 2; p++) {
        if (acc->tp_computed[p]) continue;

        NNUEAccumulator* chain[256];
        int n = 0;
        NNUEAccumulator* cursor = acc;
        while (n < 256 && cursor != NULL && !cursor->tp_computed[p] && cursor->tp_state == NNUE_TP_STATE_RECORDED) {
            chain[n++] = cursor;
            cursor = cursor->previous;
        }
        if (cursor == NULL || !cursor->tp_computed[p]) {
            tp_build_half(board, acc, p, net);
            continue;
        }

        const NNUEAccumulator* parent = cursor;
        int i = n - 1;
        for (; i >= 0; i--) {
            if (chain[i]->tp_mask[p] != parent->tp_mask[p]) break;
            tp_materialise_half(chain[i], parent, p, net);
            parent = chain[i];
        }
        if (i >= 0) tp_build_half(board, acc, p, net);
    }

#ifdef DEBUG_NNUE_EVAL
    // pawnocchio's oracle check: incremental result against a build from scratch
    for (int p = 0; p < 2; p++) {
        alignas(64) int16_t incremental[NNUE_HIDDEN_SIZE];
        memcpy(incremental, acc->tp[p], sizeof(incremental));
        tp_build_half(board, acc, p, net);
        if (memcmp(incremental, acc->tp[p], sizeof(incremental)) != 0) {
            printf("info string THREAT ACC MISMATCH perspective %d\n", p);
        }
    }
#endif
}
#endif

int nnue_evaluate(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net) {
    if (acc == NULL || net == NULL) return 0;

    if (!acc->computed) {
        nnue_materialize_accumulator(board, acc, net);
    }
#if NNUE_ARCH == NNUE_ARCH_THREATS
    if (!acc->tp_computed[0] || !acc->tp_computed[1]) tp_ensure(board, acc, net);
    const int16_t* us_tp = board->whiteToMove ? acc->tp[WHITE] : acc->tp[BLACK];
    const int16_t* them_tp = board->whiteToMove ? acc->tp[BLACK] : acc->tp[WHITE];
#else
    const int16_t* us_tp = NULL;
    const int16_t* them_tp = NULL;
#endif

    const int bucket = nnue_get_output_bucket(board);
    const int16_t* us_acc = board->whiteToMove ? acc->white : acc->black;
    const int16_t* them_acc = board->whiteToMove ? acc->black : acc->white;

    alignas(64) uint8_t ft_out[NNUE_L1_IN];
    alignas(32) uint16_t idx[NNUE_L1_GROUPS + 8];  // 8 spare slots for the last 8-wide store
    int count = deep_ft_activate(us_acc, us_tp, ft_out, idx, 0, 0);
    count = deep_ft_activate(them_acc, them_tp, ft_out + NNUE_FT_HALF, idx, count, NNUE_FT_HALF / 4);

    alignas(32) int32_t z1[NNUE_L2_SIZE];
    deep_l1_forward(ft_out, idx, count, net, bucket, z1);

    alignas(32) int16_t l2_in[2 * NNUE_L2_SIZE];
    for (int o = 0; o < NNUE_L2_SIZE; o++) {
        // 2^14 -> 2^20 (|z| <= 768*127*127 + |bias|, x 64 < 2^31)
        int32_t z = (z1[o] + net->l1_biases[bucket][o]) * (1 << (NNUE_L1_SHIFT - 14));
        int64_t lin = z < 0 ? 0 : (z > NNUE_L1_ONE ? NNUE_L1_ONE : z);
        int64_t zc = z < -NNUE_L1_ONE ? -NNUE_L1_ONE : (z > NNUE_L1_ONE ? NNUE_L1_ONE : z);
        // 2^20 -> Q^2 = 2^12 and 2^40 -> 2^12, both rounded (<= 4096, fits i16)
        l2_in[o] = (int16_t)((lin + (1 << 7)) >> 8);
        l2_in[NNUE_L2_SIZE + o] = (int16_t)((zc * zc + (INT64_C(1) << 27)) >> 28);
    }

    alignas(32) int32_t l3_in[NNUE_L3_SIZE];
    deep_l2_forward(l2_in, net, bucket, l3_in);

    int64_t out = net->l3_biases[bucket];
    for (int i = 0; i < NNUE_L3_SIZE; i++) out += (int64_t)l3_in[i] * net->l3_weights[bucket][i];

    int eval = (int)(out * NNUE_SCALE / NNUE_Q4);
    return board->whiteToMove ? eval : -eval;
}

#else
// Evaluate position using NNUE
int nnue_evaluate(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net) {
    if (acc == NULL || net == NULL) return 0;

    if (!acc->computed) {
        nnue_materialize_accumulator(board, acc, net);
    }

    int output_bucket = nnue_get_output_bucket(board);
    int16_t* us_acc = board->whiteToMove ? acc->white : acc->black;
    int16_t* them_acc = board->whiteToMove ? acc->black : acc->white;
    const int16_t* us_weights = net->output_weights[output_bucket];
    const int16_t* them_weights = net->output_weights[output_bucket] + NNUE_HIDDEN_SIZE;

    // SCReLU values are multiplied by QA * QA * QB, but output bias is only QA * QB
    int32_t output = screlu_dot(us_acc, us_weights, NNUE_HIDDEN_SIZE)
                   + screlu_dot(them_acc, them_weights, NNUE_HIDDEN_SIZE);

    output /= NNUE_QA;
    output += net->output_biases[output_bucket];

    int eval = (output * NNUE_SCALE) / (NNUE_QA * NNUE_QB);
    return board->whiteToMove ? eval : -eval;
}

#endif

// Combined sub-add for move updates (dst = dst - src_sub + src_add)
static inline void vec_sub_add(int16_t* restrict dst, const int16_t* restrict src_sub,
                                const int16_t* restrict src_add, int size) {
#if defined(NNUE_AVX512)
    for (int i = 0; i < size; i += 32) {
        __m512i d = _mm512_load_si512((const __m512i*)(dst + i));
        __m512i sub = _mm512_loadu_si512((const __m512i*)(src_sub + i));
        __m512i add = _mm512_loadu_si512((const __m512i*)(src_add + i));
        d = _mm512_sub_epi16(d, sub);
        d = _mm512_add_epi16(d, add);
        _mm512_store_si512((__m512i*)(dst + i), d);
    }
#elif defined(NNUE_AVX2)
    for (int i = 0; i < size; i += 16) {
        __m256i d = _mm256_load_si256((const __m256i*)(dst + i));
        __m256i sub = _mm256_loadu_si256((const __m256i*)(src_sub + i));
        __m256i add = _mm256_loadu_si256((const __m256i*)(src_add + i));
        d = _mm256_sub_epi16(d, sub);
        d = _mm256_add_epi16(d, add);
        _mm256_store_si256((__m256i*)(dst + i), d);
    }
#else
    for (int i = 0; i < size; i++) {
        dst[i] = dst[i] - src_sub[i] + src_add[i];
    }
#endif
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

    const int16_t* w_from = net->ft_weights[white_from_bucket][white_from_input];
    const int16_t* w_to = net->ft_weights[white_to_bucket][white_to_input];
    const int16_t* b_from = net->ft_weights[black_from_bucket][black_from_input];
    const int16_t* b_to = net->ft_weights[black_to_bucket][black_to_input];

    if (apply) {
        // apply: dst = dst - from + to
        vec_sub_add(acc->white, w_from, w_to, NNUE_HIDDEN_SIZE);
        vec_sub_add(acc->black, b_from, b_to, NNUE_HIDDEN_SIZE);
    } else {
        // undo: dst = dst + from - to (same as dst = dst - to + from)
        vec_sub_add(acc->white, w_to, w_from, NNUE_HIDDEN_SIZE);
        vec_sub_add(acc->black, b_to, b_from, NNUE_HIDDEN_SIZE);
    }
}

static bool nnue_apply_lazy_delta(NNUEAccumulator* acc, const NNUENetwork* net) {
    if (acc == NULL || net == NULL) return false;
    if (acc->requires_refresh || acc->piece_type < 0 ||
        acc->white_king_sq < 0 || acc->black_king_sq < 0) {
        return false;
    }

    nnue_update_piece_move_with_kings(acc, net, acc->from_sq, acc->to_sq,
                                      acc->piece_type, acc->moving_color,
                                      acc->white_king_sq, acc->black_king_sq, true);

    if (acc->captured_piece_type >= 0) {
        KingBucket white_bucket = get_king_bucket(acc->white_king_sq, 0);
        KingBucket black_bucket = get_king_bucket(acc->black_king_sq, 1);

        int captured_color = acc->moving_color ^ 1;
        int white_cap_idx = get_feature_index(0, acc->captured_piece_type, captured_color,
                                              acc->capture_sq, white_bucket);
        int black_cap_idx = get_feature_index(1, acc->captured_piece_type, captured_color,
                                              acc->capture_sq, black_bucket);

        int white_bucket_idx = white_cap_idx / NNUE_INPUT_SIZE;
        int white_cap_input = white_cap_idx % NNUE_INPUT_SIZE;
        int black_bucket_idx = black_cap_idx / NNUE_INPUT_SIZE;
        int black_cap_input = black_cap_idx % NNUE_INPUT_SIZE;

        vec_sub(acc->white, net->ft_weights[white_bucket_idx][white_cap_input], NNUE_HIDDEN_SIZE);
        vec_sub(acc->black, net->ft_weights[black_bucket_idx][black_cap_input], NNUE_HIDDEN_SIZE);
    }

    return true;
}

void nnue_materialize_accumulator(const Board* board, NNUEAccumulator* acc, const NNUENetwork* net) {
    if (acc == NULL || net == NULL) return;
    if (acc->computed) return;

    NNUEAccumulator* chain[256];
    int count = 0;
    NNUEAccumulator* cursor = acc;

    while (cursor != NULL && !cursor->computed) {
        if (cursor->requires_refresh || count >= (int)(sizeof(chain) / sizeof(chain[0]))) {
            psqt_refresh_in_chain(board, acc, net);
            return;
        }
        chain[count++] = cursor;
        cursor = cursor->previous;
    }

    if (cursor == NULL || !cursor->computed) {
        psqt_refresh_in_chain(board, acc, net);
        return;
    }

    for (int i = count - 1; i >= 0; i--) {
        NNUEAccumulator* frame = chain[i];
        vec_copy(frame->white, cursor->white, NNUE_HIDDEN_SIZE);
        vec_copy(frame->black, cursor->black, NNUE_HIDDEN_SIZE);

        if (!nnue_apply_lazy_delta(frame, net)) {
            psqt_refresh_in_chain(board, acc, net);
            return;
        }

        frame->computed = true;
        frame->dirty = false;
        cursor = frame;
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
#if NNUE_ARCH == NNUE_ARCH_THREATS
    acc->tp_computed[0] = acc->tp_computed[1] = false;  // no parent frame: rebuilt on the next eval
    acc->tp_state = NNUE_TP_STATE_NONE;
#endif
    
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

            vec_sub(acc->white, net->ft_weights[white_bucket_idx][white_cap_input], NNUE_HIDDEN_SIZE);
            vec_sub(acc->black, net->ft_weights[black_bucket_idx][black_cap_input], NNUE_HIDDEN_SIZE);
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
#if NNUE_ARCH == NNUE_ARCH_THREATS
    acc->tp_computed[0] = acc->tp_computed[1] = false;  // no parent frame: rebuilt on the next eval
    acc->tp_state = NNUE_TP_STATE_NONE;
#endif
    
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

            vec_add(acc->white, net->ft_weights[white_bucket_idx][white_cap_input], NNUE_HIDDEN_SIZE);
            vec_add(acc->black, net->ft_weights[black_bucket_idx][black_cap_input], NNUE_HIDDEN_SIZE);
        }
    }
    // acc->computed bleibt true (war schon true, sonst hätten wir oben returned)
}
