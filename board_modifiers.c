
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
// Helper functions
// =============================================================================

// Convert PieceTypeToken to NNUE piece type (PAWN_T=1 -> NNUE_PAWN=0)
static inline int pieceTypeToNNUE(PieceTypeToken pt) {
    return (pt == NO_PIECE_TYPE) ? -1 : (pt - 1);
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
// OPTIMIZED applyMove - Stockfish-style branchless hotpath
// =============================================================================

void applyMove(Board* board, Move move, MoveUndoInfo* undoInfo, NNUEAccumulator* nnue_acc, const NNUENetwork* nnue_net) {
    Square from = MOVE_FROM(move);
    Square to = MOVE_TO(move);
    int us = board->whiteToMove ? WHITE : BLACK;
    int them = 1 - us;
    
    // O(1) piece lookup - THE key optimization
    uint8_t movingPiece = board->piece[from];
    int movingType = PIECE_TYPE_OF(movingPiece);
    PieceTypeToken movingPieceType = (PieceTypeToken)(movingType + 1);
    
    Bitboard fromTo = (1ULL << from) | (1ULL << to);
    Bitboard fromBB = 1ULL << from;
    Bitboard toBB = 1ULL << to;
    
    // Accumulate zobrist changes locally
    uint64_t zobrist = board->zobristKey;
    
    // Store undo info
    undoInfo->oldEnPassantSquare = board->enPassantSquare;
    undoInfo->oldCastlingRights = board->castlingRights;
    undoInfo->oldHalfMoveClock = board->halfMoveClock;
    undoInfo->oldZobristKey = board->zobristKey;
    undoInfo->capturedPieceType = NO_PIECE_TYPE;
    
    // Determine captured piece (O(1) lookup)
    uint8_t capturedPiece = NO_PIECE;
    int capturedType = -1;
    PieceTypeToken capturedPieceType = NO_PIECE_TYPE;
    
    if (MOVE_IS_CAPTURE(move)) {
        if (MOVE_IS_EN_PASSANT(move)) {
            capturedPieceType = PAWN_T;
            capturedType = PAWN;
        } else {
            capturedPiece = board->piece[to];
            capturedType = PIECE_TYPE_OF(capturedPiece);
            capturedPieceType = (PieceTypeToken)(capturedType + 1);
        }
        undoInfo->capturedPieceType = capturedPieceType;
    }
    
    // NNUE update before board modification
    int promoFlag = MOVE_PROMOTION(move);
    bool isKingMove = (movingType == KING);
    
    if (nnue_acc != NULL && nnue_net != NULL) {
        if (MOVE_IS_CASTLING(move) || promoFlag || isKingMove) {
            // Refresh after board update
        } else {
            int nnue_piece = movingType;
            int nnue_captured = capturedType;
            nnue_apply_move(board, nnue_acc, nnue_net, from, to, nnue_piece, 
                           nnue_captured, us == WHITE, MOVE_IS_EN_PASSANT(move));
        }
    }

    // =========================================================================
    // BOARD UPDATE - Branchless indexed operations
    // =========================================================================
    
    // Zobrist: remove piece from source
    zobrist ^= ZOBRIST_PIECE_KEY(movingPieceType, us, from);

    // Handle captures
    if (MOVE_IS_CAPTURE(move)) {
        if (MOVE_IS_EN_PASSANT(move)) {
            Square capturedSq = (us == WHITE) ? (to - 8) : (to + 8);
            Bitboard capBB = 1ULL << capturedSq;
            
            board->piece[capturedSq] = NO_PIECE;
            board->byTypeBB[them][PAWN] &= ~capBB;
            zobrist ^= ZOBRIST_PIECE_KEY(PAWN_T, them, capturedSq);
        } else {
            // Branchless capture removal
            board->piece[to] = NO_PIECE;
            board->byTypeBB[them][capturedType] &= ~toBB;
            zobrist ^= ZOBRIST_PIECE_KEY(capturedPieceType, them, to);
        }
    }

    // Move piece - branchless XOR for bitboards
    board->byTypeBB[us][movingType] ^= fromTo;
    board->piece[from] = NO_PIECE;
    
    // Handle promotion or regular move
    if (promoFlag) {
        int promoType = PROMO_TO_TYPE[promoFlag];
        uint8_t promoPiece = MAKE_PIECE_NEW(promoType, us);
        
        // Remove pawn from destination (we just moved it there)
        board->byTypeBB[us][movingType] ^= toBB;  // Undo the pawn move to 'to'
        // Add promoted piece
        board->byTypeBB[us][promoType] |= toBB;
        board->piece[to] = promoPiece;
        
        zobrist ^= ZOBRIST_PIECE_KEY((PieceTypeToken)(promoType + 1), us, to);
    } else {
        board->piece[to] = movingPiece;
        zobrist ^= ZOBRIST_PIECE_KEY(movingPieceType, us, to);
    }

    // Update castling rights
    uint8_t oldCastling = board->castlingRights;
    
    if (movingType == KING) {
        board->castlingRights &= (us == WHITE) ? 
            ~(WHITE_KINGSIDE_CASTLE | WHITE_QUEENSIDE_CASTLE) :
            ~(BLACK_KINGSIDE_CASTLE | BLACK_QUEENSIDE_CASTLE);
    }
    
    if (movingType == ROOK) {
        if (from == SQ_A1) board->castlingRights &= ~WHITE_QUEENSIDE_CASTLE;
        else if (from == SQ_H1) board->castlingRights &= ~WHITE_KINGSIDE_CASTLE;
        else if (from == SQ_A8) board->castlingRights &= ~BLACK_QUEENSIDE_CASTLE;
        else if (from == SQ_H8) board->castlingRights &= ~BLACK_KINGSIDE_CASTLE;
    }
    
    if (MOVE_IS_CAPTURE(move) && capturedType == ROOK) {
        if (to == SQ_A1) board->castlingRights &= ~WHITE_QUEENSIDE_CASTLE;
        else if (to == SQ_H1) board->castlingRights &= ~WHITE_KINGSIDE_CASTLE;
        else if (to == SQ_A8) board->castlingRights &= ~BLACK_QUEENSIDE_CASTLE;
        else if (to == SQ_H8) board->castlingRights &= ~BLACK_KINGSIDE_CASTLE;
    }
    
    if (oldCastling != board->castlingRights) {
        zobrist ^= zobrist_castling_keys[oldCastling] ^ zobrist_castling_keys[board->castlingRights];
    }

    // Handle castling rook movement
    if (MOVE_IS_CASTLING(move)) {
        Square rookFrom, rookTo;
        if (us == WHITE) {
            if (to == SQ_G1) { rookFrom = SQ_H1; rookTo = SQ_F1; }
            else { rookFrom = SQ_A1; rookTo = SQ_D1; }
        } else {
            if (to == SQ_G8) { rookFrom = SQ_H8; rookTo = SQ_F8; }
            else { rookFrom = SQ_A8; rookTo = SQ_D8; }
        }
        
        Bitboard rookFromTo = (1ULL << rookFrom) | (1ULL << rookTo);
        board->byTypeBB[us][ROOK] ^= rookFromTo;
        board->piece[rookFrom] = NO_PIECE;
        board->piece[rookTo] = (us == WHITE) ? W_ROOK : B_ROOK;
        
        zobrist ^= ZOBRIST_PIECE_KEY(ROOK_T, us, rookFrom) ^ ZOBRIST_PIECE_KEY(ROOK_T, us, rookTo);
        
        if (nnue_acc != NULL && nnue_net != NULL) {
            nnue_refresh_accumulator(board, nnue_acc, nnue_net);
        }
    }
    
    // NNUE refresh for promotions
    if (promoFlag && nnue_acc != NULL && nnue_net != NULL) {
        nnue_refresh_accumulator(board, nnue_acc, nnue_net);
    }
    
    // NNUE refresh for king moves
    if (isKingMove && !MOVE_IS_CASTLING(move) && nnue_acc != NULL && nnue_net != NULL) {
        nnue_refresh_accumulator(board, nnue_acc, nnue_net);
    }

    // En passant square update
    if (undoInfo->oldEnPassantSquare != SQ_NONE) {
        zobrist ^= zobrist_enpassant_keys[undoInfo->oldEnPassantSquare];
    }

    if (MOVE_IS_DOUBLE_PAWN_PUSH(move)) {
        board->enPassantSquare = (us == WHITE) ? (from + 8) : (from - 8);
        zobrist ^= zobrist_enpassant_keys[board->enPassantSquare];
    } else {
        board->enPassantSquare = SQ_NONE;
    }

    // Halfmove clock
    board->halfMoveClock = (movingType == PAWN || MOVE_IS_CAPTURE(move)) ? 0 : board->halfMoveClock + 1;

    // Fullmove number
    board->fullMoveNumber += (us == BLACK);

    // Switch side
    board->whiteToMove = !board->whiteToMove;
    zobrist ^= zobrist_side_to_move_key;

    // Single write of zobrist key
    board->zobristKey = zobrist;
    board->history[board->historyIndex++] = zobrist;
}

// =============================================================================
// OPTIMIZED undoMove
// =============================================================================

void undoMove(Board* board, Move move, const MoveUndoInfo* undoInfo, NNUEAccumulator* nnue_acc, const NNUENetwork* nnue_net) {
    Square from = MOVE_FROM(move);
    Square to = MOVE_TO(move);
    
    // Revert side to move first
    board->whiteToMove = !board->whiteToMove;
    int us = board->whiteToMove ? WHITE : BLACK;
    int them = 1 - us;

    // Revert fullmove number
    if (us == BLACK) {
        board->fullMoveNumber--;
    }

    // Revert other state
    board->halfMoveClock = undoInfo->oldHalfMoveClock;
    board->enPassantSquare = undoInfo->oldEnPassantSquare;
    board->castlingRights = undoInfo->oldCastlingRights;
    board->historyIndex--;

    // Get moved piece info
    int promoFlag = MOVE_PROMOTION(move);
    uint8_t currentPiece = board->piece[to];
    int currentType = PIECE_TYPE_OF(currentPiece);
    
    Bitboard fromBB = 1ULL << from;
    Bitboard toBB = 1ULL << to;
    Bitboard fromTo = fromBB | toBB;
    
    PieceTypeToken movedPieceType;
    
    if (promoFlag) {
        // Undo promotion: remove promoted piece, add pawn back
        int promoType = PROMO_TO_TYPE[promoFlag];
        
        board->byTypeBB[us][promoType] &= ~toBB;
        board->piece[to] = NO_PIECE;
        
        // Add pawn back to source
        board->byTypeBB[us][PAWN] |= fromBB;
        board->piece[from] = (us == WHITE) ? W_PAWN : B_PAWN;
        
        movedPieceType = PAWN_T;
    } else {
        // Regular move: just move piece back
        board->byTypeBB[us][currentType] ^= fromTo;
        board->piece[to] = NO_PIECE;
        board->piece[from] = currentPiece;
        
        movedPieceType = (PieceTypeToken)(currentType + 1);
    }

    // Restore captured piece
    if (MOVE_IS_CAPTURE(move)) {
        PieceTypeToken capturedType = undoInfo->capturedPieceType;
        int capType = capturedType - 1;
        uint8_t capPiece = MAKE_PIECE(capturedType, them == WHITE);
        
        if (MOVE_IS_EN_PASSANT(move)) {
            Square capturedSq = (us == WHITE) ? (to - 8) : (to + 8);
            Bitboard capBB = 1ULL << capturedSq;
            
            board->byTypeBB[them][PAWN] |= capBB;
            board->piece[capturedSq] = (them == WHITE) ? W_PAWN : B_PAWN;
        } else {
            board->byTypeBB[them][capType] |= toBB;
            board->piece[to] = capPiece;
        }
    }

    // Revert castling rook move
    if (MOVE_IS_CASTLING(move)) {
        Square rookFrom, rookTo;
        if (us == WHITE) {
            if (to == SQ_G1) { rookFrom = SQ_H1; rookTo = SQ_F1; }
            else { rookFrom = SQ_A1; rookTo = SQ_D1; }
        } else {
            if (to == SQ_G8) { rookFrom = SQ_H8; rookTo = SQ_F8; }
            else { rookFrom = SQ_A8; rookTo = SQ_D8; }
        }
        
        Bitboard rookFromTo = (1ULL << rookFrom) | (1ULL << rookTo);
        board->byTypeBB[us][ROOK] ^= rookFromTo;
        board->piece[rookTo] = NO_PIECE;
        board->piece[rookFrom] = (us == WHITE) ? W_ROOK : B_ROOK;
        
        if (nnue_acc != NULL && nnue_net != NULL) {
            nnue_refresh_accumulator(board, nnue_acc, nnue_net);
        }
    } else if (promoFlag) {
        if (nnue_acc != NULL && nnue_net != NULL) {
            nnue_refresh_accumulator(board, nnue_acc, nnue_net);
        }
    } else if (movedPieceType == KING_T) {
        if (nnue_acc != NULL && nnue_net != NULL) {
            nnue_refresh_accumulator(board, nnue_acc, nnue_net);
        }
    } else {
        int nnue_piece = pieceTypeToNNUE(movedPieceType);
        int nnue_captured = pieceTypeToNNUE(undoInfo->capturedPieceType);
        nnue_undo_move(board, nnue_acc, nnue_net, from, to, nnue_piece, nnue_captured, 
                       us == WHITE, MOVE_IS_EN_PASSANT(move));
    }

    // Restore Zobrist key
    board->zobristKey = undoInfo->oldZobristKey;
}
