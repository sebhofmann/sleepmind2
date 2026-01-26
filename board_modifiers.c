
#include "board_modifiers.h"
#include "zobrist.h"
#include <stdio.h>
#include <stdlib.h>

// =============================================================================
// Lookup tables for branchless operations
// =============================================================================

// Promotion flag (1-4) to piece type (0-5) - direct lookup
static const int PROMO_TO_TYPE[5] = {-1, KNIGHT, BISHOP, ROOK, QUEEN};

// =============================================================================
// MACRO-BASED BRANCHLESS applyMove TEMPLATE
// =============================================================================
// Generates specialized variants eliminating all runtime branches.
// Parameters are compile-time constants, allowing dead code elimination.
// =============================================================================

#define APPLY_MOVE_TEMPLATE(COLOR, MOVING_TYPE, IS_CAPTURE, IS_EN_PASSANT, IS_CASTLING, IS_PROMO, WITH_NNUE) \
static void applyMove_##COLOR##_##MOVING_TYPE##_cap##IS_CAPTURE##_ep##IS_EN_PASSANT##_castle##IS_CASTLING##_promo##IS_PROMO##_nnue##WITH_NNUE( \
    Board* board, Move move, MoveUndoInfo* undoInfo, \
    NNUEAccumulator* nnue_acc, const NNUENetwork* nnue_net) \
{ \
    const int us = COLOR; \
    const int them = 1 - us; \
    Square from = MOVE_FROM(move); \
    Square to = MOVE_TO(move); \
    \
    uint8_t movingPiece = board->piece[from]; \
    const int movingType = MOVING_TYPE; \
    PieceTypeToken movingPieceType = (PieceTypeToken)(movingType + 1); \
    \
    Bitboard fromTo = (1ULL << from) | (1ULL << to); \
    Bitboard toBB = 1ULL << to; \
    \
    uint64_t zobrist = board->zobristKey; \
    \
    undoInfo->oldEnPassantSquare = board->enPassantSquare; \
    undoInfo->oldCastlingRights = board->castlingRights; \
    undoInfo->oldHalfMoveClock = board->halfMoveClock; \
    undoInfo->oldZobristKey = board->zobristKey; \
    undoInfo->capturedPieceType = NO_PIECE_TYPE; \
    \
    /* Captured piece info - compile-time specialized */ \
    int capturedType = -1; \
    PieceTypeToken capturedPieceType = NO_PIECE_TYPE; \
    \
    if (IS_CAPTURE) { \
        if (IS_EN_PASSANT) { \
            capturedPieceType = PAWN_T; \
            capturedType = PAWN; \
        } else { \
            uint8_t capturedPiece = board->piece[to]; \
            capturedType = PIECE_TYPE_OF(capturedPiece); \
            capturedPieceType = (PieceTypeToken)(capturedType + 1); \
        } \
        undoInfo->capturedPieceType = capturedPieceType; \
    } \
    \
    /* NNUE update before board modification - compile-time specialized */ \
    if (WITH_NNUE) { \
        if (!IS_CASTLING && !IS_PROMO && (MOVING_TYPE != KING)) { \
            int nnue_piece = movingType; \
            int nnue_captured = capturedType; \
            nnue_apply_move(board, nnue_acc, nnue_net, from, to, nnue_piece, \
                           nnue_captured, us == WHITE, IS_EN_PASSANT); \
        } \
    } \
    \
    /* Zobrist: remove piece from source */ \
    zobrist ^= ZOBRIST_PIECE_KEY(movingPieceType, us, from); \
    \
    /* Handle captures - compile-time specialized */ \
    if (IS_CAPTURE) { \
        if (IS_EN_PASSANT) { \
            Square capturedSq = (us == WHITE) ? (to - 8) : (to + 8); \
            Bitboard capBB = 1ULL << capturedSq; \
            board->piece[capturedSq] = NO_PIECE; \
            board->byTypeBB[them][PAWN] &= ~capBB; \
            zobrist ^= ZOBRIST_PIECE_KEY(PAWN_T, them, capturedSq); \
        } else { \
            board->piece[to] = NO_PIECE; \
            board->byTypeBB[them][capturedType] &= ~toBB; \
            zobrist ^= ZOBRIST_PIECE_KEY(capturedPieceType, them, to); \
        } \
    } \
    \
    /* Move piece */ \
    board->byTypeBB[us][movingType] ^= fromTo; \
    board->piece[from] = NO_PIECE; \
    \
    /* Handle promotion or regular move - compile-time specialized */ \
    if (IS_PROMO) { \
        int promoFlag = MOVE_PROMOTION(move); \
        int promoType = PROMO_TO_TYPE[promoFlag]; \
        uint8_t promoPiece = MAKE_PIECE_NEW(promoType, us); \
        board->byTypeBB[us][movingType] ^= toBB; \
        board->byTypeBB[us][promoType] |= toBB; \
        board->piece[to] = promoPiece; \
        zobrist ^= ZOBRIST_PIECE_KEY((PieceTypeToken)(promoType + 1), us, to); \
    } else { \
        board->piece[to] = movingPiece; \
        zobrist ^= ZOBRIST_PIECE_KEY(movingPieceType, us, to); \
    } \
    \
    /* Update castling rights - compile-time specialized */ \
    uint8_t oldCastling = board->castlingRights; \
    if (MOVING_TYPE == KING) { \
        board->castlingRights &= (us == WHITE) ? \
            ~(WHITE_KINGSIDE_CASTLE | WHITE_QUEENSIDE_CASTLE) : \
            ~(BLACK_KINGSIDE_CASTLE | BLACK_QUEENSIDE_CASTLE); \
    } \
    if (MOVING_TYPE == ROOK) { \
        if (us == WHITE) { \
            if (from == SQ_A1) board->castlingRights &= ~WHITE_QUEENSIDE_CASTLE; \
            else if (from == SQ_H1) board->castlingRights &= ~WHITE_KINGSIDE_CASTLE; \
        } else { \
            if (from == SQ_A8) board->castlingRights &= ~BLACK_QUEENSIDE_CASTLE; \
            else if (from == SQ_H8) board->castlingRights &= ~BLACK_KINGSIDE_CASTLE; \
        } \
    } \
    if (IS_CAPTURE && capturedType == ROOK) { \
        if (to == SQ_A1) board->castlingRights &= ~WHITE_QUEENSIDE_CASTLE; \
        else if (to == SQ_H1) board->castlingRights &= ~WHITE_KINGSIDE_CASTLE; \
        else if (to == SQ_A8) board->castlingRights &= ~BLACK_QUEENSIDE_CASTLE; \
        else if (to == SQ_H8) board->castlingRights &= ~BLACK_KINGSIDE_CASTLE; \
    } \
    if (oldCastling != board->castlingRights) { \
        zobrist ^= zobrist_castling_keys[oldCastling] ^ zobrist_castling_keys[board->castlingRights]; \
    } \
    \
    /* Handle castling rook movement - compile-time specialized */ \
    if (IS_CASTLING) { \
        Square rookFrom, rookTo; \
        if (us == WHITE) { \
            if (to == SQ_G1) { rookFrom = SQ_H1; rookTo = SQ_F1; } \
            else { rookFrom = SQ_A1; rookTo = SQ_D1; } \
        } else { \
            if (to == SQ_G8) { rookFrom = SQ_H8; rookTo = SQ_F8; } \
            else { rookFrom = SQ_A8; rookTo = SQ_D8; } \
        } \
        Bitboard rookFromTo = (1ULL << rookFrom) | (1ULL << rookTo); \
        board->byTypeBB[us][ROOK] ^= rookFromTo; \
        board->piece[rookFrom] = NO_PIECE; \
        board->piece[rookTo] = (us == WHITE) ? W_ROOK : B_ROOK; \
        zobrist ^= ZOBRIST_PIECE_KEY(ROOK_T, us, rookFrom) ^ ZOBRIST_PIECE_KEY(ROOK_T, us, rookTo); \
        if (WITH_NNUE) { \
            nnue_refresh_accumulator(board, nnue_acc, nnue_net); \
        } \
    } \
    \
    /* NNUE refresh for promotions */ \
    if (IS_PROMO && WITH_NNUE) { \
        nnue_refresh_accumulator(board, nnue_acc, nnue_net); \
    } \
    \
    /* NNUE refresh for king moves (non-castling) */ \
    if ((MOVING_TYPE == KING) && !IS_CASTLING && WITH_NNUE) { \
        nnue_refresh_accumulator(board, nnue_acc, nnue_net); \
    } \
    \
    /* En passant square update */ \
    if (undoInfo->oldEnPassantSquare != SQ_NONE) { \
        zobrist ^= zobrist_enpassant_keys[undoInfo->oldEnPassantSquare]; \
    } \
    if ((MOVING_TYPE == PAWN) && MOVE_IS_DOUBLE_PAWN_PUSH(move)) { \
        board->enPassantSquare = (us == WHITE) ? (from + 8) : (from - 8); \
        zobrist ^= zobrist_enpassant_keys[board->enPassantSquare]; \
    } else { \
        board->enPassantSquare = SQ_NONE; \
    } \
    \
    /* Halfmove clock */ \
    board->halfMoveClock = ((MOVING_TYPE == PAWN) || IS_CAPTURE) ? 0 : board->halfMoveClock + 1; \
    \
    /* Fullmove number */ \
    board->fullMoveNumber += (us == BLACK); \
    \
    /* Switch side */ \
    board->whiteToMove = !board->whiteToMove; \
    zobrist ^= zobrist_side_to_move_key; \
    \
    board->zobristKey = zobrist; \
    board->history[board->historyIndex++] = zobrist; \
}

// =============================================================================
// Generate all specialized variants
// =============================================================================

// Helper macros for generating variants for each piece type
#define GEN_PIECE_VARIANTS(COLOR, PIECE_TYPE) \
    /* Standard moves (no capture, no special flags) */ \
    APPLY_MOVE_TEMPLATE(COLOR, PIECE_TYPE, 0, 0, 0, 0, 0) \
    APPLY_MOVE_TEMPLATE(COLOR, PIECE_TYPE, 0, 0, 0, 0, 1) \
    /* Captures (non-EP) */ \
    APPLY_MOVE_TEMPLATE(COLOR, PIECE_TYPE, 1, 0, 0, 0, 0) \
    APPLY_MOVE_TEMPLATE(COLOR, PIECE_TYPE, 1, 0, 0, 0, 1)

// Pawn-specific variants (includes en passant and promotion)
#define GEN_PAWN_VARIANTS(COLOR) \
    /* Standard pawn moves */ \
    APPLY_MOVE_TEMPLATE(COLOR, PAWN, 0, 0, 0, 0, 0) \
    APPLY_MOVE_TEMPLATE(COLOR, PAWN, 0, 0, 0, 0, 1) \
    /* Pawn captures (non-EP) */ \
    APPLY_MOVE_TEMPLATE(COLOR, PAWN, 1, 0, 0, 0, 0) \
    APPLY_MOVE_TEMPLATE(COLOR, PAWN, 1, 0, 0, 0, 1) \
    /* En passant */ \
    APPLY_MOVE_TEMPLATE(COLOR, PAWN, 1, 1, 0, 0, 0) \
    APPLY_MOVE_TEMPLATE(COLOR, PAWN, 1, 1, 0, 0, 1) \
    /* Promotions (non-capture) */ \
    APPLY_MOVE_TEMPLATE(COLOR, PAWN, 0, 0, 0, 1, 0) \
    APPLY_MOVE_TEMPLATE(COLOR, PAWN, 0, 0, 0, 1, 1) \
    /* Promotion captures */ \
    APPLY_MOVE_TEMPLATE(COLOR, PAWN, 1, 0, 0, 1, 0) \
    APPLY_MOVE_TEMPLATE(COLOR, PAWN, 1, 0, 0, 1, 1)

// King-specific variants (includes castling)
#define GEN_KING_VARIANTS(COLOR) \
    /* Standard king moves */ \
    APPLY_MOVE_TEMPLATE(COLOR, KING, 0, 0, 0, 0, 0) \
    APPLY_MOVE_TEMPLATE(COLOR, KING, 0, 0, 0, 0, 1) \
    /* King captures */ \
    APPLY_MOVE_TEMPLATE(COLOR, KING, 1, 0, 0, 0, 0) \
    APPLY_MOVE_TEMPLATE(COLOR, KING, 1, 0, 0, 0, 1) \
    /* Castling */ \
    APPLY_MOVE_TEMPLATE(COLOR, KING, 0, 0, 1, 0, 0) \
    APPLY_MOVE_TEMPLATE(COLOR, KING, 0, 0, 1, 0, 1)

// Generate all WHITE variants
GEN_PAWN_VARIANTS(WHITE)
GEN_PIECE_VARIANTS(WHITE, KNIGHT)
GEN_PIECE_VARIANTS(WHITE, BISHOP)
GEN_PIECE_VARIANTS(WHITE, ROOK)
GEN_PIECE_VARIANTS(WHITE, QUEEN)
GEN_KING_VARIANTS(WHITE)

// Generate all BLACK variants  
GEN_PAWN_VARIANTS(BLACK)
GEN_PIECE_VARIANTS(BLACK, KNIGHT)
GEN_PIECE_VARIANTS(BLACK, BISHOP)
GEN_PIECE_VARIANTS(BLACK, ROOK)
GEN_PIECE_VARIANTS(BLACK, QUEEN)
GEN_KING_VARIANTS(BLACK)

// =============================================================================
// Dispatch function pointer tables for O(1) variant selection
// =============================================================================

typedef void (*ApplyMoveFn)(Board*, Move, MoveUndoInfo*, NNUEAccumulator*, const NNUENetwork*);

// Index: [color][pieceType][isCapture][isEnPassant][isCastling][isPromo][withNNUE]
// But we flatten this for practical lookup

// For regular pieces (Knight, Bishop, Rook, Queen): [capture][nnue]
#define DISPATCH_REGULAR(COLOR, PIECE_TYPE) { \
    { applyMove_##COLOR##_##PIECE_TYPE##_cap0_ep0_castle0_promo0_nnue0, \
      applyMove_##COLOR##_##PIECE_TYPE##_cap0_ep0_castle0_promo0_nnue1 }, \
    { applyMove_##COLOR##_##PIECE_TYPE##_cap1_ep0_castle0_promo0_nnue0, \
      applyMove_##COLOR##_##PIECE_TYPE##_cap1_ep0_castle0_promo0_nnue1 } \
}

// Pawn dispatch table: [moveSubType][nnue]
// moveSubType: 0=quiet, 1=capture, 2=ep, 3=promo_quiet, 4=promo_capture
static const ApplyMoveFn pawnDispatch[2][5][2] = {
    // WHITE
    {
        { applyMove_WHITE_PAWN_cap0_ep0_castle0_promo0_nnue0, applyMove_WHITE_PAWN_cap0_ep0_castle0_promo0_nnue1 }, // quiet
        { applyMove_WHITE_PAWN_cap1_ep0_castle0_promo0_nnue0, applyMove_WHITE_PAWN_cap1_ep0_castle0_promo0_nnue1 }, // capture
        { applyMove_WHITE_PAWN_cap1_ep1_castle0_promo0_nnue0, applyMove_WHITE_PAWN_cap1_ep1_castle0_promo0_nnue1 }, // en passant
        { applyMove_WHITE_PAWN_cap0_ep0_castle0_promo1_nnue0, applyMove_WHITE_PAWN_cap0_ep0_castle0_promo1_nnue1 }, // promo quiet
        { applyMove_WHITE_PAWN_cap1_ep0_castle0_promo1_nnue0, applyMove_WHITE_PAWN_cap1_ep0_castle0_promo1_nnue1 }  // promo capture
    },
    // BLACK
    {
        { applyMove_BLACK_PAWN_cap0_ep0_castle0_promo0_nnue0, applyMove_BLACK_PAWN_cap0_ep0_castle0_promo0_nnue1 },
        { applyMove_BLACK_PAWN_cap1_ep0_castle0_promo0_nnue0, applyMove_BLACK_PAWN_cap1_ep0_castle0_promo0_nnue1 },
        { applyMove_BLACK_PAWN_cap1_ep1_castle0_promo0_nnue0, applyMove_BLACK_PAWN_cap1_ep1_castle0_promo0_nnue1 },
        { applyMove_BLACK_PAWN_cap0_ep0_castle0_promo1_nnue0, applyMove_BLACK_PAWN_cap0_ep0_castle0_promo1_nnue1 },
        { applyMove_BLACK_PAWN_cap1_ep0_castle0_promo1_nnue0, applyMove_BLACK_PAWN_cap1_ep0_castle0_promo1_nnue1 }
    }
};

// Regular pieces dispatch: [color][pieceType-1][capture][nnue]  (pieceType 1-4 for N,B,R,Q)
static const ApplyMoveFn regularDispatch[2][4][2][2] = {
    // WHITE
    {
        DISPATCH_REGULAR(WHITE, KNIGHT),
        DISPATCH_REGULAR(WHITE, BISHOP),
        DISPATCH_REGULAR(WHITE, ROOK),
        DISPATCH_REGULAR(WHITE, QUEEN)
    },
    // BLACK
    {
        DISPATCH_REGULAR(BLACK, KNIGHT),
        DISPATCH_REGULAR(BLACK, BISHOP),
        DISPATCH_REGULAR(BLACK, ROOK),
        DISPATCH_REGULAR(BLACK, QUEEN)
    }
};

// King dispatch: [color][moveSubType][nnue]
// moveSubType: 0=quiet, 1=capture, 2=castling
static const ApplyMoveFn kingDispatch[2][3][2] = {
    // WHITE
    {
        { applyMove_WHITE_KING_cap0_ep0_castle0_promo0_nnue0, applyMove_WHITE_KING_cap0_ep0_castle0_promo0_nnue1 },
        { applyMove_WHITE_KING_cap1_ep0_castle0_promo0_nnue0, applyMove_WHITE_KING_cap1_ep0_castle0_promo0_nnue1 },
        { applyMove_WHITE_KING_cap0_ep0_castle1_promo0_nnue0, applyMove_WHITE_KING_cap0_ep0_castle1_promo0_nnue1 }
    },
    // BLACK
    {
        { applyMove_BLACK_KING_cap0_ep0_castle0_promo0_nnue0, applyMove_BLACK_KING_cap0_ep0_castle0_promo0_nnue1 },
        { applyMove_BLACK_KING_cap1_ep0_castle0_promo0_nnue0, applyMove_BLACK_KING_cap1_ep0_castle0_promo0_nnue1 },
        { applyMove_BLACK_KING_cap0_ep0_castle1_promo0_nnue0, applyMove_BLACK_KING_cap0_ep0_castle1_promo0_nnue1 }
    }
};

// =============================================================================
// MACRO-BASED BRANCHLESS undoMove TEMPLATE
// =============================================================================

#define UNDO_MOVE_TEMPLATE(COLOR, MOVING_TYPE, IS_CAPTURE, IS_EN_PASSANT, IS_CASTLING, IS_PROMO, WITH_NNUE) \
static void undoMove_##COLOR##_##MOVING_TYPE##_cap##IS_CAPTURE##_ep##IS_EN_PASSANT##_castle##IS_CASTLING##_promo##IS_PROMO##_nnue##WITH_NNUE( \
    Board* board, Move move, const MoveUndoInfo* undoInfo, \
    NNUEAccumulator* nnue_acc, const NNUENetwork* nnue_net) \
{ \
    const int us = COLOR; \
    const int them = 1 - us; \
    Square from = MOVE_FROM(move); \
    Square to = MOVE_TO(move); \
    \
    /* Revert side to move first */ \
    board->whiteToMove = !board->whiteToMove; \
    \
    /* Revert fullmove number - compile-time specialized */ \
    if (us == BLACK) { \
        board->fullMoveNumber--; \
    } \
    \
    /* Revert other state */ \
    board->halfMoveClock = undoInfo->oldHalfMoveClock; \
    board->enPassantSquare = undoInfo->oldEnPassantSquare; \
    board->castlingRights = undoInfo->oldCastlingRights; \
    board->historyIndex--; \
    \
    Bitboard fromBB = 1ULL << from; \
    Bitboard toBB = 1ULL << to; \
    Bitboard fromTo = fromBB | toBB; \
    \
    /* Undo piece movement - compile-time specialized */ \
    if (IS_PROMO) { \
        int promoFlag = MOVE_PROMOTION(move); \
        int promoType = PROMO_TO_TYPE[promoFlag]; \
        \
        board->byTypeBB[us][promoType] &= ~toBB; \
        board->piece[to] = NO_PIECE; \
        \
        board->byTypeBB[us][PAWN] |= fromBB; \
        board->piece[from] = (us == WHITE) ? W_PAWN : B_PAWN; \
    } else { \
        uint8_t currentPiece = board->piece[to]; \
        int currentType = PIECE_TYPE_OF(currentPiece); \
        \
        board->byTypeBB[us][currentType] ^= fromTo; \
        board->piece[to] = NO_PIECE; \
        board->piece[from] = currentPiece; \
    } \
    \
    /* Restore captured piece - compile-time specialized */ \
    if (IS_CAPTURE) { \
        PieceTypeToken capturedType = undoInfo->capturedPieceType; \
        int capType = capturedType - 1; \
        uint8_t capPiece = MAKE_PIECE(capturedType, them == WHITE); \
        \
        if (IS_EN_PASSANT) { \
            Square capturedSq = (us == WHITE) ? (to - 8) : (to + 8); \
            Bitboard capBB = 1ULL << capturedSq; \
            \
            board->byTypeBB[them][PAWN] |= capBB; \
            board->piece[capturedSq] = (them == WHITE) ? W_PAWN : B_PAWN; \
        } else { \
            board->byTypeBB[them][capType] |= toBB; \
            board->piece[to] = capPiece; \
        } \
    } \
    \
    /* Revert castling rook move - compile-time specialized */ \
    if (IS_CASTLING) { \
        Square rookFrom, rookTo; \
        if (us == WHITE) { \
            if (to == SQ_G1) { rookFrom = SQ_H1; rookTo = SQ_F1; } \
            else { rookFrom = SQ_A1; rookTo = SQ_D1; } \
        } else { \
            if (to == SQ_G8) { rookFrom = SQ_H8; rookTo = SQ_F8; } \
            else { rookFrom = SQ_A8; rookTo = SQ_D8; } \
        } \
        \
        Bitboard rookFromTo = (1ULL << rookFrom) | (1ULL << rookTo); \
        board->byTypeBB[us][ROOK] ^= rookFromTo; \
        board->piece[rookTo] = NO_PIECE; \
        board->piece[rookFrom] = (us == WHITE) ? W_ROOK : B_ROOK; \
        \
        if (WITH_NNUE) { \
            nnue_refresh_accumulator(board, nnue_acc, nnue_net); \
        } \
    } else if (IS_PROMO) { \
        if (WITH_NNUE) { \
            nnue_refresh_accumulator(board, nnue_acc, nnue_net); \
        } \
    } else if (MOVING_TYPE == KING) { \
        if (WITH_NNUE) { \
            nnue_refresh_accumulator(board, nnue_acc, nnue_net); \
        } \
    } else { \
        if (WITH_NNUE) { \
            int nnue_piece = MOVING_TYPE; \
            int nnue_captured = IS_CAPTURE ? (undoInfo->capturedPieceType - 1) : -1; \
            nnue_undo_move(board, nnue_acc, nnue_net, from, to, nnue_piece, nnue_captured, \
                           us == WHITE, IS_EN_PASSANT); \
        } \
    } \
    \
    /* Restore Zobrist key */ \
    board->zobristKey = undoInfo->oldZobristKey; \
}

// =============================================================================
// Generate all specialized undoMove variants
// =============================================================================

#define GEN_UNDO_PIECE_VARIANTS(COLOR, PIECE_TYPE) \
    UNDO_MOVE_TEMPLATE(COLOR, PIECE_TYPE, 0, 0, 0, 0, 0) \
    UNDO_MOVE_TEMPLATE(COLOR, PIECE_TYPE, 0, 0, 0, 0, 1) \
    UNDO_MOVE_TEMPLATE(COLOR, PIECE_TYPE, 1, 0, 0, 0, 0) \
    UNDO_MOVE_TEMPLATE(COLOR, PIECE_TYPE, 1, 0, 0, 0, 1)

#define GEN_UNDO_PAWN_VARIANTS(COLOR) \
    UNDO_MOVE_TEMPLATE(COLOR, PAWN, 0, 0, 0, 0, 0) \
    UNDO_MOVE_TEMPLATE(COLOR, PAWN, 0, 0, 0, 0, 1) \
    UNDO_MOVE_TEMPLATE(COLOR, PAWN, 1, 0, 0, 0, 0) \
    UNDO_MOVE_TEMPLATE(COLOR, PAWN, 1, 0, 0, 0, 1) \
    UNDO_MOVE_TEMPLATE(COLOR, PAWN, 1, 1, 0, 0, 0) \
    UNDO_MOVE_TEMPLATE(COLOR, PAWN, 1, 1, 0, 0, 1) \
    UNDO_MOVE_TEMPLATE(COLOR, PAWN, 0, 0, 0, 1, 0) \
    UNDO_MOVE_TEMPLATE(COLOR, PAWN, 0, 0, 0, 1, 1) \
    UNDO_MOVE_TEMPLATE(COLOR, PAWN, 1, 0, 0, 1, 0) \
    UNDO_MOVE_TEMPLATE(COLOR, PAWN, 1, 0, 0, 1, 1)

#define GEN_UNDO_KING_VARIANTS(COLOR) \
    UNDO_MOVE_TEMPLATE(COLOR, KING, 0, 0, 0, 0, 0) \
    UNDO_MOVE_TEMPLATE(COLOR, KING, 0, 0, 0, 0, 1) \
    UNDO_MOVE_TEMPLATE(COLOR, KING, 1, 0, 0, 0, 0) \
    UNDO_MOVE_TEMPLATE(COLOR, KING, 1, 0, 0, 0, 1) \
    UNDO_MOVE_TEMPLATE(COLOR, KING, 0, 0, 1, 0, 0) \
    UNDO_MOVE_TEMPLATE(COLOR, KING, 0, 0, 1, 0, 1)

// Generate all WHITE undoMove variants
GEN_UNDO_PAWN_VARIANTS(WHITE)
GEN_UNDO_PIECE_VARIANTS(WHITE, KNIGHT)
GEN_UNDO_PIECE_VARIANTS(WHITE, BISHOP)
GEN_UNDO_PIECE_VARIANTS(WHITE, ROOK)
GEN_UNDO_PIECE_VARIANTS(WHITE, QUEEN)
GEN_UNDO_KING_VARIANTS(WHITE)

// Generate all BLACK undoMove variants
GEN_UNDO_PAWN_VARIANTS(BLACK)
GEN_UNDO_PIECE_VARIANTS(BLACK, KNIGHT)
GEN_UNDO_PIECE_VARIANTS(BLACK, BISHOP)
GEN_UNDO_PIECE_VARIANTS(BLACK, ROOK)
GEN_UNDO_PIECE_VARIANTS(BLACK, QUEEN)
GEN_UNDO_KING_VARIANTS(BLACK)

// =============================================================================
// undoMove Dispatch function pointer tables
// =============================================================================

typedef void (*UndoMoveFn)(Board*, Move, const MoveUndoInfo*, NNUEAccumulator*, const NNUENetwork*);

#define UNDO_DISPATCH_REGULAR(COLOR, PIECE_TYPE) { \
    { undoMove_##COLOR##_##PIECE_TYPE##_cap0_ep0_castle0_promo0_nnue0, \
      undoMove_##COLOR##_##PIECE_TYPE##_cap0_ep0_castle0_promo0_nnue1 }, \
    { undoMove_##COLOR##_##PIECE_TYPE##_cap1_ep0_castle0_promo0_nnue0, \
      undoMove_##COLOR##_##PIECE_TYPE##_cap1_ep0_castle0_promo0_nnue1 } \
}

// Pawn undo dispatch table: [color][moveSubType][nnue]
static const UndoMoveFn undoPawnDispatch[2][5][2] = {
    // WHITE
    {
        { undoMove_WHITE_PAWN_cap0_ep0_castle0_promo0_nnue0, undoMove_WHITE_PAWN_cap0_ep0_castle0_promo0_nnue1 },
        { undoMove_WHITE_PAWN_cap1_ep0_castle0_promo0_nnue0, undoMove_WHITE_PAWN_cap1_ep0_castle0_promo0_nnue1 },
        { undoMove_WHITE_PAWN_cap1_ep1_castle0_promo0_nnue0, undoMove_WHITE_PAWN_cap1_ep1_castle0_promo0_nnue1 },
        { undoMove_WHITE_PAWN_cap0_ep0_castle0_promo1_nnue0, undoMove_WHITE_PAWN_cap0_ep0_castle0_promo1_nnue1 },
        { undoMove_WHITE_PAWN_cap1_ep0_castle0_promo1_nnue0, undoMove_WHITE_PAWN_cap1_ep0_castle0_promo1_nnue1 }
    },
    // BLACK
    {
        { undoMove_BLACK_PAWN_cap0_ep0_castle0_promo0_nnue0, undoMove_BLACK_PAWN_cap0_ep0_castle0_promo0_nnue1 },
        { undoMove_BLACK_PAWN_cap1_ep0_castle0_promo0_nnue0, undoMove_BLACK_PAWN_cap1_ep0_castle0_promo0_nnue1 },
        { undoMove_BLACK_PAWN_cap1_ep1_castle0_promo0_nnue0, undoMove_BLACK_PAWN_cap1_ep1_castle0_promo0_nnue1 },
        { undoMove_BLACK_PAWN_cap0_ep0_castle0_promo1_nnue0, undoMove_BLACK_PAWN_cap0_ep0_castle0_promo1_nnue1 },
        { undoMove_BLACK_PAWN_cap1_ep0_castle0_promo1_nnue0, undoMove_BLACK_PAWN_cap1_ep0_castle0_promo1_nnue1 }
    }
};

// Regular pieces undo dispatch: [color][pieceType-1][capture][nnue]
static const UndoMoveFn undoRegularDispatch[2][4][2][2] = {
    // WHITE
    {
        UNDO_DISPATCH_REGULAR(WHITE, KNIGHT),
        UNDO_DISPATCH_REGULAR(WHITE, BISHOP),
        UNDO_DISPATCH_REGULAR(WHITE, ROOK),
        UNDO_DISPATCH_REGULAR(WHITE, QUEEN)
    },
    // BLACK
    {
        UNDO_DISPATCH_REGULAR(BLACK, KNIGHT),
        UNDO_DISPATCH_REGULAR(BLACK, BISHOP),
        UNDO_DISPATCH_REGULAR(BLACK, ROOK),
        UNDO_DISPATCH_REGULAR(BLACK, QUEEN)
    }
};

// King undo dispatch: [color][moveSubType][nnue]
static const UndoMoveFn undoKingDispatch[2][3][2] = {
    // WHITE
    {
        { undoMove_WHITE_KING_cap0_ep0_castle0_promo0_nnue0, undoMove_WHITE_KING_cap0_ep0_castle0_promo0_nnue1 },
        { undoMove_WHITE_KING_cap1_ep0_castle0_promo0_nnue0, undoMove_WHITE_KING_cap1_ep0_castle0_promo0_nnue1 },
        { undoMove_WHITE_KING_cap0_ep0_castle1_promo0_nnue0, undoMove_WHITE_KING_cap0_ep0_castle1_promo0_nnue1 }
    },
    // BLACK
    {
        { undoMove_BLACK_KING_cap0_ep0_castle0_promo0_nnue0, undoMove_BLACK_KING_cap0_ep0_castle0_promo0_nnue1 },
        { undoMove_BLACK_KING_cap1_ep0_castle0_promo0_nnue0, undoMove_BLACK_KING_cap1_ep0_castle0_promo0_nnue1 },
        { undoMove_BLACK_KING_cap0_ep0_castle1_promo0_nnue0, undoMove_BLACK_KING_cap0_ep0_castle1_promo0_nnue1 }
    }
};

// =============================================================================
// Helper functions
// =============================================================================

// Convert PieceTypeToken to NNUE piece type (PAWN_T=1 -> NNUE_PAWN=0, NO_PIECE_TYPE=0 -> -1)
static inline int pieceTypeToNNUE(PieceTypeToken pt) {
    return pt - 1;
}

// Legacy compatibility - uses piece array now
PieceTypeToken getPieceTypeAtSquareForColor(const Board* board, Square sq, bool isWhite) {
    uint8_t p = board->piece[sq];
    if (p == NO_PIECE) return NO_PIECE_TYPE;
    bool pieceIsWhite = PIECE_IS_WHITE(p);
    if (pieceIsWhite != isWhite) return NO_PIECE_TYPE;
    return (PieceTypeToken)PIECE_TYPE(p);
}

PieceTypeToken getPieceTypeAtSquare(const Board* board, Square sq, bool* pieceIsWhite) {
    uint8_t p = board->piece[sq];
    if (p == NO_PIECE) {
        *pieceIsWhite = false;
        return NO_PIECE_TYPE;
    }
    *pieceIsWhite = PIECE_IS_WHITE(p);
    return (PieceTypeToken)PIECE_TYPE(p);
}

// Branchless add piece - updates piece array and indexed bitboards
void addPieceToBoard(Board* board, Square sq, PieceTypeToken pieceType, bool isWhite) {
    uint8_t p = MAKE_PIECE(pieceType, isWhite);
    int color = isWhite ? WHITE : BLACK;
    int type = pieceType - 1;  // PAWN_T(1) -> PAWN(0)
    
    board->piece[sq] = p;
    board->byTypeBB[color][type] |= (1ULL << sq);
}

// Branchless remove piece
void removePieceFromBoard(Board* board, Square sq, PieceTypeToken pieceType, bool isWhite) {
    int color = isWhite ? WHITE : BLACK;
    int type = pieceType - 1;
    
    board->piece[sq] = NO_PIECE;
    board->byTypeBB[color][type] &= ~(1ULL << sq);
}

PieceTypeToken getPieceTypeFromPromotionFlag(int promoFlag) {
    static const PieceTypeToken promo_table[5] = {NO_PIECE_TYPE, KNIGHT_T, BISHOP_T, ROOK_T, QUEEN_T};
    return (promoFlag >= 0 && promoFlag <= 4) ? promo_table[promoFlag] : NO_PIECE_TYPE;
}

// Legacy compatibility - now uses indexed lookup
Bitboard* getMutablePieceBitboardPointer(Board* board, Square sq, bool isPieceWhite) {
    uint8_t p = board->piece[sq];
    if (p == NO_PIECE) return NULL;
    int color = isPieceWhite ? WHITE : BLACK;
    int type = PIECE_TYPE_OF(p);
    return &board->byTypeBB[color][type];
}

void clearCaptureSquareOnAllBitboards(Board* board, Square sq) {
    uint8_t p = board->piece[sq];
    if (p == NO_PIECE) return;
    int color = PIECE_COLOR_OF(p);
    int type = PIECE_TYPE_OF(p);
    
    board->piece[sq] = NO_PIECE;
    board->byTypeBB[color][type] &= ~(1ULL << sq);
}

// =============================================================================
// BRANCHLESS applyMove - Dispatches to specialized variant
// =============================================================================
// Single entry point that selects the appropriate specialized function based
// on move flags. All branches happen here once, then execution is branch-free.
// =============================================================================

void applyMove(Board* board, Move move, MoveUndoInfo* undoInfo, NNUEAccumulator* nnue_acc, const NNUENetwork* nnue_net) {
    Square from = MOVE_FROM(move);
    int us = board->whiteToMove ? WHITE : BLACK;
    int withNNUE = (nnue_acc != NULL && nnue_net != NULL) ? 1 : 0;
    
    // O(1) piece lookup
    uint8_t movingPiece = board->piece[from];
    int movingType = PIECE_TYPE_OF(movingPiece);
    
    // Extract move flags (computed once)
    int isCapture = MOVE_IS_CAPTURE(move);
    int isEnPassant = MOVE_IS_EN_PASSANT(move);
    int isCastling = MOVE_IS_CASTLING(move);
    int isPromo = MOVE_PROMOTION(move) != 0;
    
    // Dispatch based on piece type
    ApplyMoveFn fn;
    
    if (movingType == PAWN) {
        // Pawn: determine subtype index
        int subType;
        if (isEnPassant) {
            subType = 2;  // en passant
        } else if (isPromo) {
            subType = isCapture ? 4 : 3;  // promo capture or promo quiet
        } else {
            subType = isCapture ? 1 : 0;  // capture or quiet
        }
        fn = pawnDispatch[us][subType][withNNUE];
    } else if (movingType == KING) {
        // King: determine subtype index
        int subType;
        if (isCastling) {
            subType = 2;
        } else if (isCapture) {
            subType = 1;
        } else {
            subType = 0;
        }
        fn = kingDispatch[us][subType][withNNUE];
    } else {
        // Regular pieces (Knight, Bishop, Rook, Queen): index is movingType - 1
        fn = regularDispatch[us][movingType - 1][isCapture][withNNUE];
    }
    
    // Call the specialized branch-free function
    fn(board, move, undoInfo, nnue_acc, nnue_net);
}

// =============================================================================
// BRANCHLESS undoMove - Dispatches to specialized variant
// =============================================================================

void undoMove(Board* board, Move move, const MoveUndoInfo* undoInfo, NNUEAccumulator* nnue_acc, const NNUENetwork* nnue_net) {
    Square to = MOVE_TO(move);
    
    // Note: we need to determine the original moving side BEFORE reverting
    // Since board->whiteToMove is currently the opponent's turn, the original mover is !whiteToMove
    int us = board->whiteToMove ? BLACK : WHITE;
    int withNNUE = (nnue_acc != NULL && nnue_net != NULL) ? 1 : 0;
    
    // For undo, we need to look at what's currently on 'to' square (or deduce piece type)
    // For promotions, the piece on 'to' is the promoted piece, but we moved a pawn
    int isPromo = MOVE_PROMOTION(move) != 0;
    int isCapture = MOVE_IS_CAPTURE(move);
    int isEnPassant = MOVE_IS_EN_PASSANT(move);
    int isCastling = MOVE_IS_CASTLING(move);
    
    // Determine original piece type
    int movingType;
    if (isPromo) {
        movingType = PAWN;  // Promotions were pawn moves
    } else {
        uint8_t currentPiece = board->piece[to];
        movingType = PIECE_TYPE_OF(currentPiece);
    }
    
    // Dispatch based on piece type
    UndoMoveFn fn;
    
    if (movingType == PAWN) {
        int subType;
        if (isEnPassant) {
            subType = 2;
        } else if (isPromo) {
            subType = isCapture ? 4 : 3;
        } else {
            subType = isCapture ? 1 : 0;
        }
        fn = undoPawnDispatch[us][subType][withNNUE];
    } else if (movingType == KING) {
        int subType;
        if (isCastling) {
            subType = 2;
        } else if (isCapture) {
            subType = 1;
        } else {
            subType = 0;
        }
        fn = undoKingDispatch[us][subType][withNNUE];
    } else {
        fn = undoRegularDispatch[us][movingType - 1][isCapture][withNNUE];
    }
    
    // Call the specialized branch-free function
    fn(board, move, undoInfo, nnue_acc, nnue_net);
}
