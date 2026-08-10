#include "nnue.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char sleepmind_nnue_start[];
extern const unsigned char sleepmind_nnue_end[];

int main(void) {
    const size_t embedded_size = (size_t)(sleepmind_nnue_end - sleepmind_nnue_start);
    NNUENetwork* embedded = calloc(1, sizeof(*embedded));
    NNUENetwork* file = calloc(1, sizeof(*file));
    if (!embedded || !file) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    if (((uintptr_t)sleepmind_nnue_start % 64) != 0) {
        fprintf(stderr, "embedded network is not 64-byte aligned\n");
        return 1;
    }
    if (!nnue_load_memory(sleepmind_nnue_start, embedded_size, embedded)) {
        fprintf(stderr, "embedded network failed to load\n");
        return 1;
    }
    if (!nnue_load("quantised.bin", file)) {
        fprintf(stderr, "file network failed to load\n");
        return 1;
    }
    if (memcmp(embedded, file, sizeof(*embedded)) != 0) {
        fprintf(stderr, "embedded and file networks differ\n");
        return 1;
    }

    if (nnue_load_memory(NULL, embedded_size, embedded) ||
        nnue_load_memory(sleepmind_nnue_start, embedded_size - 1, embedded) ||
        nnue_load_memory(sleepmind_nnue_start, embedded_size + 1, embedded)) {
        fprintf(stderr, "invalid memory buffer was accepted\n");
        return 1;
    }

    free(file);
    free(embedded);
    puts("NNUE file/memory loader test passed");
    return 0;
}
