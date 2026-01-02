
#include "board_modifiers.h"
#include "zobrist.h"
#include <stdio.h> // For NULL if not in other headers
#include <stdlib.h>

// --- Helper Function Implementations ---

// Optimized: Only checks bitboards for the specified color
PieceTypeToken getPieceTypeAtSquareForColor(const Board* board, Square sq, bool isWhite) {
    Bitboard s = 1ULL << sq;
    if (isWhite) {
        if (board->whitePawns & s) return PAWN_T;
        if (board->whiteKnights & s) return KNIGHT_T;
        if (board->whiteBishops & s) return BISHOP_T;
        if (board->whiteRooks & s) return ROOK_T;
        if (board->whiteQueens & s) return QUEEN_T;
        if (board->whiteKings & s) return KING_T;
    } else {
        if (board->blackPawns & s) return PAWN_T;
        if (board->blackKnights & s) return KNIGHT_T;
        if (board->blackBishops & s) return BISHOP_T;
        if (board->blackRooks & s) return ROOK_T;
        if (board->blackQueens & s) return QUEEN_T;
        if (board->blackKings & s) return KING_T;
    }
    return NO_PIECE_TYPE;
}

PieceTypeToken getPieceTypeAtSquare(const Board* board, Square sq, bool* pieceIsWhite) {
    Bitboard s = 1ULL << sq;
    // Check each bitboard directly - avoid double lookup
    if (board->whitePawns & s)   { *pieceIsWhite = true;  return PAWN_T; }
    if (board->blackPawns & s)   { *pieceIsWhite = false; return PAWN_T; }
    if (board->whiteKnights & s) { *pieceIsWhite = true;  return KNIGHT_T; }
    if (board->blackKnights & s) { *pieceIsWhite = false; return KNIGHT_T; }
    if (board->whiteBishops & s) { *pieceIsWhite = true;  return BISHOP_T; }
    if (board->blackBishops & s) { *pieceIsWhite = false; return BISHOP_T; }
    if (board->whiteRooks & s)   { *pieceIsWhite = true;  return ROOK_T; }
    if (board->blackRooks & s)   { *pieceIsWhite = false; return ROOK_T; }
    if (board->whiteQueens & s)  { *pieceIsWhite = true;  return QUEEN_T; }
    if (board->blackQueens & s)  { *pieceIsWhite = false; return QUEEN_T; }
    if (board->whiteKings & s)   { *pieceIsWhite = true;  return KING_T; }
    if (board->blackKings & s)   { *pieceIsWhite = false; return KING_T; }
    
    *pieceIsWhite = false; 
    return NO_PIECE_TYPE;
}

void addPieceToBoard(Board* board, Square sq, PieceTypeToken pieceType, bool isWhite) {
    Bitboard s = 1ULL << sq;
    if (isWhite) {
        switch (pieceType) {
            case PAWN_T: board->whitePawns |= s; break;
            case KNIGHT_T: board->whiteKnights |= s; break;
            case BISHOP_T: board->whiteBishops |= s; break;
            case ROOK_T: board->whiteRooks |= s; break;
            case QUEEN_T: board->whiteQueens |= s; break;
            case KING_T: board->whiteKings |= s; break;
            case NO_PIECE_TYPE: break; // Should not happen
        }
    } else {
        switch (pieceType) {
            case PAWN_T: board->blackPawns |= s; break;
            case KNIGHT_T: board->blackKnights |= s; break;
            case BISHOP_T: board->blackBishops |= s; break;
            case ROOK_T: board->blackRooks |= s; break;
            case QUEEN_T: board->blackQueens |= s; break;
            case KING_T: board->blackKings |= s; break;
            case NO_PIECE_TYPE: break; // Should not happen
        }
    }
}

void removePieceFromBoard(Board* board, Square sq, PieceTypeToken pieceType, bool isWhite) {
    Bitboard s_clear = ~(1ULL << sq);
    if (isWhite) {
        switch (pieceType) {
            case PAWN_T: board->whitePawns &= s_clear; break;
            case KNIGHT_T: board->whiteKnights &= s_clear; break;
            case BISHOP_T: board->whiteBishops &= s_clear; break;
            case ROOK_T: board->whiteRooks &= s_clear; break;
            case QUEEN_T: board->whiteQueens &= s_clear; break;
            case KING_T: board->whiteKings &= s_clear; break;
            case NO_PIECE_TYPE: break; 
        }
    } else {
        switch (pieceType) {
            case PAWN_T: board->blackPawns &= s_clear; break;
            case KNIGHT_T: board->blackKnights &= s_clear; break;
            case BISHOP_T: board->blackBishops &= s_clear; break;
            case ROOK_T: board->blackRooks &= s_clear; break;
            case QUEEN_T: board->blackQueens &= s_clear; break;
            case KING_T: board->blackKings &= s_clear; break;
            case NO_PIECE_TYPE: break;
        }
    }
}

// Inline helper for promotion type lookup
static inline PieceTypeToken getPieceTypeFromPromotionFlag_inline(int promoFlag) {
    // Use lookup table for speed
    static const PieceTypeToken promo_table[5] = {NO_PIECE_TYPE, KNIGHT_T, BISHOP_T, ROOK_T, QUEEN_T};
    return (promoFlag >= 0 && promoFlag <= 4) ? promo_table[promoFlag] : NO_PIECE_TYPE;
}

PieceTypeToken getPieceTypeFromPromotionFlag(int promoFlag) {
    return getPieceTypeFromPromotionFlag_inline(promoFlag);
}

// Gets a pointer to the bitboard of the piece at a given square.
// Used to remove the piece from its original bitboard.
Bitboard* getMutablePieceBitboardPointer(Board* board, Square sq, bool isPieceWhite) {
    Bitboard s = 1ULL << sq;
    if (isPieceWhite) {
        if (board->whitePawns & s) return &board->whitePawns;
        if (board->whiteKnights & s) return &board->whiteKnights;
        if (board->whiteBishops & s) return &board->whiteBishops;
        if (board->whiteRooks & s) return &board->whiteRooks;
        if (board->whiteQueens & s) return &board->whiteQueens;
        if (board->whiteKings & s) return &board->whiteKings;
    } else {
        if (board->blackPawns & s) return &board->blackPawns;
        if (board->blackKnights & s) return &board->blackKnights;
        if (board->blackBishops & s) return &board->blackBishops;
        if (board->blackRooks & s) return &board->blackRooks;
        if (board->blackQueens & s) return &board->blackQueens;
        if (board->blackKings & s) return &board->blackKings;
    }
    return NULL; 
}

// Removes any piece from a square on all bitboards (used for captures).
void clearCaptureSquareOnAllBitboards(Board* board, Square sq) {
    Bitboard s_clear = ~(1ULL << sq); 
    board->whitePawns &= s_clear; board->whiteKnights &= s_clear; board->whiteBishops &= s_clear;
    board->whiteRooks &= s_clear; board->whiteQueens &= s_clear; board->whiteKings &= s_clear;
    board->blackPawns &= s_clear; board->blackKnights &= s_clear; board->blackBishops &= s_clear;
    board->blackRooks &= s_clear; board->blackQueens &= s_clear; board->blackKings &= s_clear;
}

// --- Get bitboard pointer by piece type (avoid repeated switch) ---
static inline Bitboard* getBitboardForPiece(Board* board, PieceTypeToken pieceType, bool isWhite) {
    if (isWhite) {
        switch (pieceType) {
            case PAWN_T: return &board->whitePawns;
            case KNIGHT_T: return &board->whiteKnights;
            case BISHOP_T: return &board->whiteBishops;
            case ROOK_T: return &board->whiteRooks;
            case QUEEN_T: return &board->whiteQueens;
            case KING_T: return &board->whiteKings;
            default: return NULL;
        }
    } else {
        switch (pieceType) {
            case PAWN_T: return &board->blackPawns;
            case KNIGHT_T: return &board->blackKnights;
            case BISHOP_T: return &board->blackBishops;
            case ROOK_T: return &board->blackRooks;
            case QUEEN_T: return &board->blackQueens;
            case KING_T: return &board->blackKings;
            default: return NULL;
        }
    }
}

// --- Optimized applyMove ---

void applyMove(Board* board, Move move, MoveUndoInfo* undoInfo) {
    Square from = MOVE_FROM(move);
    Square to = MOVE_TO(move);
    bool isWhite = board->whiteToMove;
    int colorIdx = isWhite ? 0 : 1;
    int oppColorIdx = 1 - colorIdx;
    
    // Pre-calculate masks
    Bitboard from_mask = 1ULL << from;
    Bitboard to_mask = 1ULL << to;
    Bitboard from_clear = ~from_mask;
    Bitboard to_clear = ~to_mask;
    
    // Accumulate zobrist changes locally - single write at end
    uint64_t zobrist = board->zobristKey;
    
    // Find moving piece type by checking bitboards directly
    PieceTypeToken movingPieceType;
    Bitboard* movingBB;
    
    if (isWhite) {
        if (board->whitePawns & from_mask) { movingPieceType = PAWN_T; movingBB = &board->whitePawns; }
        else if (board->whiteKnights & from_mask) { movingPieceType = KNIGHT_T; movingBB = &board->whiteKnights; }
        else if (board->whiteBishops & from_mask) { movingPieceType = BISHOP_T; movingBB = &board->whiteBishops; }
        else if (board->whiteRooks & from_mask) { movingPieceType = ROOK_T; movingBB = &board->whiteRooks; }
        else if (board->whiteQueens & from_mask) { movingPieceType = QUEEN_T; movingBB = &board->whiteQueens; }
        else { movingPieceType = KING_T; movingBB = &board->whiteKings; }
    } else {
        if (board->blackPawns & from_mask) { movingPieceType = PAWN_T; movingBB = &board->blackPawns; }
        else if (board->blackKnights & from_mask) { movingPieceType = KNIGHT_T; movingBB = &board->blackKnights; }
        else if (board->blackBishops & from_mask) { movingPieceType = BISHOP_T; movingBB = &board->blackBishops; }
        else if (board->blackRooks & from_mask) { movingPieceType = ROOK_T; movingBB = &board->blackRooks; }
        else if (board->blackQueens & from_mask) { movingPieceType = QUEEN_T; movingBB = &board->blackQueens; }
        else { movingPieceType = KING_T; movingBB = &board->blackKings; }
    }

    // Store undo info
    undoInfo->oldEnPassantSquare = board->enPassantSquare;
    undoInfo->oldCastlingRights = board->castlingRights;
    undoInfo->oldHalfMoveClock = board->halfMoveClock;
    undoInfo->oldZobristKey = board->zobristKey;
    undoInfo->capturedPieceType = NO_PIECE_TYPE;

    // Update zobrist: remove piece from source
    zobrist ^= ZOBRIST_PIECE_KEY(movingPieceType, colorIdx, from);

    // Handle captures
    if (MOVE_IS_CAPTURE(move)) {
        if (MOVE_IS_EN_PASSANT(move)) {
            Square capturedSq = isWhite ? (to - 8) : (to + 8);
            Bitboard cap_clear = ~(1ULL << capturedSq);
            undoInfo->capturedPieceType = PAWN_T;
            
            if (isWhite) {
                board->blackPawns &= cap_clear;
            } else {
                board->whitePawns &= cap_clear;
            }
            zobrist ^= ZOBRIST_PIECE_KEY(PAWN_T, oppColorIdx, capturedSq);
        } else {
            // Find captured piece type
            PieceTypeToken capturedType;
            if (!isWhite) {
                // Capturing white piece
                if (board->whitePawns & to_mask) { capturedType = PAWN_T; board->whitePawns &= to_clear; }
                else if (board->whiteKnights & to_mask) { capturedType = KNIGHT_T; board->whiteKnights &= to_clear; }
                else if (board->whiteBishops & to_mask) { capturedType = BISHOP_T; board->whiteBishops &= to_clear; }
                else if (board->whiteRooks & to_mask) { capturedType = ROOK_T; board->whiteRooks &= to_clear; }
                else { capturedType = QUEEN_T; board->whiteQueens &= to_clear; }
            } else {
                // Capturing black piece
                if (board->blackPawns & to_mask) { capturedType = PAWN_T; board->blackPawns &= to_clear; }
                else if (board->blackKnights & to_mask) { capturedType = KNIGHT_T; board->blackKnights &= to_clear; }
                else if (board->blackBishops & to_mask) { capturedType = BISHOP_T; board->blackBishops &= to_clear; }
                else if (board->blackRooks & to_mask) { capturedType = ROOK_T; board->blackRooks &= to_clear; }
                else { capturedType = QUEEN_T; board->blackQueens &= to_clear; }
            }
            undoInfo->capturedPieceType = capturedType;
            zobrist ^= ZOBRIST_PIECE_KEY(capturedType, oppColorIdx, to);
        }
    }

    // Move piece: remove from source
    *movingBB &= from_clear;
    
    // Handle promotion or regular move
    int promoFlag = MOVE_PROMOTION(move);
    if (promoFlag) {
        PieceTypeToken promoType = getPieceTypeFromPromotionFlag_inline(promoFlag);
        Bitboard* promoBB = getBitboardForPiece(board, promoType, isWhite);
        *promoBB |= to_mask;
        zobrist ^= ZOBRIST_PIECE_KEY(promoType, colorIdx, to);
    } else {
        *movingBB |= to_mask;
        zobrist ^= ZOBRIST_PIECE_KEY(movingPieceType, colorIdx, to);
    }

    // Update castling rights
    uint8_t oldCastling = board->castlingRights;
    
    // King move removes all castling rights for that side
    if (movingPieceType == KING_T) {
        board->castlingRights &= isWhite ? ~(WHITE_KINGSIDE_CASTLE | WHITE_QUEENSIDE_CASTLE) 
                                          : ~(BLACK_KINGSIDE_CASTLE | BLACK_QUEENSIDE_CASTLE);
    }
    
    // Rook move or capture affects specific castling rights
    if (movingPieceType == ROOK_T) {
        if (from == SQ_A1) board->castlingRights &= ~WHITE_QUEENSIDE_CASTLE;
        else if (from == SQ_H1) board->castlingRights &= ~WHITE_KINGSIDE_CASTLE;
        else if (from == SQ_A8) board->castlingRights &= ~BLACK_QUEENSIDE_CASTLE;
        else if (from == SQ_H8) board->castlingRights &= ~BLACK_KINGSIDE_CASTLE;
    }
    
    if (MOVE_IS_CAPTURE(move) && undoInfo->capturedPieceType == ROOK_T) {
        if (to == SQ_A1) board->castlingRights &= ~WHITE_QUEENSIDE_CASTLE;
        else if (to == SQ_H1) board->castlingRights &= ~WHITE_KINGSIDE_CASTLE;
        else if (to == SQ_A8) board->castlingRights &= ~BLACK_QUEENSIDE_CASTLE;
        else if (to == SQ_H8) board->castlingRights &= ~BLACK_KINGSIDE_CASTLE;
    }
    
    // Only update zobrist for castling if rights changed
    if (oldCastling != board->castlingRights) {
        zobrist ^= zobrist_castling_keys[oldCastling] ^ zobrist_castling_keys[board->castlingRights];
    }

    // Handle castling rook movement
    if (MOVE_IS_CASTLING(move)) {
        if (isWhite) {
            if (to == SQ_G1) {
                board->whiteRooks = (board->whiteRooks & ~(1ULL << SQ_H1)) | (1ULL << SQ_F1);
                zobrist ^= ZOBRIST_PIECE_KEY(ROOK_T, 0, SQ_H1) ^ ZOBRIST_PIECE_KEY(ROOK_T, 0, SQ_F1);
            } else {
                board->whiteRooks = (board->whiteRooks & ~(1ULL << SQ_A1)) | (1ULL << SQ_D1);
                zobrist ^= ZOBRIST_PIECE_KEY(ROOK_T, 0, SQ_A1) ^ ZOBRIST_PIECE_KEY(ROOK_T, 0, SQ_D1);
            }
        } else {
            if (to == SQ_G8) {
                board->blackRooks = (board->blackRooks & ~(1ULL << SQ_H8)) | (1ULL << SQ_F8);
                zobrist ^= ZOBRIST_PIECE_KEY(ROOK_T, 1, SQ_H8) ^ ZOBRIST_PIECE_KEY(ROOK_T, 1, SQ_F8);
            } else {
                board->blackRooks = (board->blackRooks & ~(1ULL << SQ_A8)) | (1ULL << SQ_D8);
                zobrist ^= ZOBRIST_PIECE_KEY(ROOK_T, 1, SQ_A8) ^ ZOBRIST_PIECE_KEY(ROOK_T, 1, SQ_D8);
            }
        }
    }

    // En passant square update - simplified
    int oldEpSq = undoInfo->oldEnPassantSquare;
    if (oldEpSq != SQ_NONE) {
        zobrist ^= zobrist_enpassant_keys[oldEpSq];
    }

    if (MOVE_IS_DOUBLE_PAWN_PUSH(move)) {
        board->enPassantSquare = isWhite ? (from + 8) : (from - 8);
        zobrist ^= zobrist_enpassant_keys[board->enPassantSquare];
    } else {
        board->enPassantSquare = SQ_NONE;
    }

    // Halfmove clock
    board->halfMoveClock = (movingPieceType == PAWN_T || MOVE_IS_CAPTURE(move)) ? 0 : board->halfMoveClock + 1;

    // Fullmove number
    board->fullMoveNumber += !isWhite;

    // Switch side
    board->whiteToMove = !isWhite;
    zobrist ^= zobrist_side_to_move_key;

    // Single write of zobrist key
    board->zobristKey = zobrist;

    // History
    board->history[board->historyIndex++] = zobrist;
}

void undoMove(Board* board, Move move, const MoveUndoInfo* undoInfo) {
    Square from = MOVE_FROM(move);
    Square to = MOVE_TO(move);
    
    // Revert side to move first
    board->whiteToMove = !board->whiteToMove;
    bool movingPlayerIsWhite = board->whiteToMove;

    // Revert fullmove number
    if (!movingPlayerIsWhite) {
        board->fullMoveNumber--;
    }

    // Revert halfmove clock
    board->halfMoveClock = undoInfo->oldHalfMoveClock;

    // Revert en passant square
    board->enPassantSquare = undoInfo->oldEnPassantSquare;

    // Revert castling rights
    board->castlingRights = undoInfo->oldCastlingRights;

    // Revert history
    board->historyIndex--;

    // Revert pieces
    PieceTypeToken movedPieceType;
    if (MOVE_IS_PROMOTION(move)) {
        PieceTypeToken promotedPieceType = getPieceTypeFromPromotionFlag(MOVE_PROMOTION(move));
        removePieceFromBoard(board, to, promotedPieceType, movingPlayerIsWhite);
        addPieceToBoard(board, from, PAWN_T, movingPlayerIsWhite);
        movedPieceType = PAWN_T;
    } else {
        bool dummy;
        movedPieceType = getPieceTypeAtSquare(board, to, &dummy);
        removePieceFromBoard(board, to, movedPieceType, movingPlayerIsWhite);
        addPieceToBoard(board, from, movedPieceType, movingPlayerIsWhite);
    }

    // 2. Restore captured piece
    if (MOVE_IS_CAPTURE(move)) {
        if (MOVE_IS_EN_PASSANT(move)) {
            Square capturedSq = movingPlayerIsWhite ? (to - 8) : (to + 8);
            addPieceToBoard(board, capturedSq, PAWN_T, !movingPlayerIsWhite);
        } else {
            addPieceToBoard(board, to, undoInfo->capturedPieceType, !movingPlayerIsWhite);
        }
    }

    // 3. Revert castling rook move
    if (MOVE_IS_CASTLING(move)) {
        if (movingPlayerIsWhite) {
            if (to == SQ_G1) { // Kingside
                removePieceFromBoard(board, SQ_F1, ROOK_T, true);
                addPieceToBoard(board, SQ_H1, ROOK_T, true);
            } else { // Queenside
                removePieceFromBoard(board, SQ_D1, ROOK_T, true);
                addPieceToBoard(board, SQ_A1, ROOK_T, true);
            }
        } else {
            if (to == SQ_G8) { // Kingside
                removePieceFromBoard(board, SQ_F8, ROOK_T, false);
                addPieceToBoard(board, SQ_H8, ROOK_T, false);
            } else { // Queenside
                removePieceFromBoard(board, SQ_D8, ROOK_T, false);
                addPieceToBoard(board, SQ_A8, ROOK_T, false);
            }
        }
    }

    // Restore Zobrist key
    board->zobristKey = undoInfo->oldZobristKey;
}
