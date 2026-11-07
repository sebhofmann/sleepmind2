// Net file to embed, set by the Makefile per NNUE_ARCH (quantised.bin / quantised_deep.bin)
#ifndef NNUE_EMBED_FILE
#define NNUE_EMBED_FILE "quantised.bin"
#endif

#if defined(__APPLE__)
    .section __DATA,__const
    .balign 64
    .globl _sleepmind_nnue_start
_sleepmind_nnue_start:
    .incbin NNUE_EMBED_FILE
    .globl _sleepmind_nnue_end
_sleepmind_nnue_end:
#elif defined(_WIN32)
    .section .rdata,"dr"
    .balign 64
    .globl sleepmind_nnue_start
sleepmind_nnue_start:
    .incbin NNUE_EMBED_FILE
    .globl sleepmind_nnue_end
sleepmind_nnue_end:
#else
    .section .rodata
    .balign 64
    .globl sleepmind_nnue_start
sleepmind_nnue_start:
    .incbin NNUE_EMBED_FILE
    .globl sleepmind_nnue_end
sleepmind_nnue_end:

    .section .note.GNU-stack,"",@progbits
#endif
