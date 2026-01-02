
#include "board_modifiers.h"
#include "zobrist.h"
#include <stdio.h> // For NULL if not in other headers
#include <stdlib.h>



// --- Helper Function Implementations ---

PieceTypeToken getPieceTypeAtSquare(const Board* board, Square sq, bool* pieceIsWhite) {
    Bitboard s = 1ULL << sq;
    if (board->whitePawns & s) { *pieceIsWhite = true; return PAWN_T; }
    if (board->whiteKnights & s) { *pieceIsWhite = true; return KNIGHT_T; }
    if (board->whiteBishops & s) { *pieceIsWhite = true; return BISHOP_T; }
    if (board->whiteRooks & s) { *pieceIsWhite = true; return ROOK_T; }
    if (board->whiteQueens & s) { *pieceIsWhite = true; return QUEEN_T; }
    if (board->whiteKings & s) { *pieceIsWhite = true; return KING_T; }

    if (board->blackPawns & s) { *pieceIsWhite = false; return PAWN_T; }
    if (board->blackKnights & s) { *pieceIsWhite = false; return KNIGHT_T; }
    if (board->blackBishops & s) { *pieceIsWhite = false; return BISHOP_T; }
    if (board->blackRooks & s) { *pieceIsWhite = false; return ROOK_T; }
    if (board->blackQueens & s) { *pieceIsWhite = false; return QUEEN_T; }
    if (board->blackKings & s) { *pieceIsWhite = false; return KING_T; }
    
    *pieceIsWhite = false; // Should not happen on a valid board if sq has a piece
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

PieceTypeToken getPieceTypeFromPromotionFlag(int promoFlag) {
    switch (promoFlag) {
        case PROMOTION_N: return KNIGHT_T;
        case PROMOTION_B: return BISHOP_T;
        case PROMOTION_R: return ROOK_T;
        case PROMOTION_Q: return QUEEN_T;
        default: return NO_PIECE_TYPE; 
    }
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


// --- Public Function Implementations ---

void applyMove(Board* board, Move move, MoveUndoInfo* undoInfo) {
    // 0. Store current state for undo
    undoInfo->previousBoard = *board; // Store the entire board state

    Square from = MOVE_FROM(move);
    Square to = MOVE_TO(move);
    bool movingPlayerIsWhite = board->whiteToMove;
    
    bool pieceIsWhiteTemp; 
    PieceTypeToken movingPieceType = getPieceTypeAtSquare(board, from, &pieceIsWhiteTemp);

    bool isPawnMove = (movingPieceType == PAWN_T);
    bool isCapture = MOVE_IS_CAPTURE(move);
    PieceTypeToken capturedPieceType = NO_PIECE_TYPE; // Default

    // 1. Handle captures & determine captured piece type
    if (isCapture) {
        Square capturedPawnSq = SQ_NONE; // Only relevant for en passant
        if (MOVE_IS_EN_PASSANT(move)) {
            capturedPieceType = PAWN_T; // En passant always captures a pawn
            capturedPawnSq = movingPlayerIsWhite ? (to - 8) : (to + 8);
            removePieceFromBoard(board, capturedPawnSq, PAWN_T, !movingPlayerIsWhite);
        } else {
            bool capturedPieceIsWhite; // Temporary holder for color
            capturedPieceType = getPieceTypeAtSquare(board, to, &capturedPieceIsWhite);
            // No need to remove the captured piece explicitly here if we are clearing the 'to' square anyway
            // before adding the moving piece. However, clearCaptureSquareOnAllBitboards is more robust.
            clearCaptureSquareOnAllBitboards(board, to); 
        }
    }

    // 2. Move the piece: Remove from 'from', add to 'to'
    removePieceFromBoard(board, from, movingPieceType, movingPlayerIsWhite);

    if (MOVE_IS_PROMOTION(move)) {
        PieceTypeToken promotionType = getPieceTypeFromPromotionFlag(MOVE_PROMOTION(move));
        addPieceToBoard(board, to, promotionType, movingPlayerIsWhite);
    } else {
        addPieceToBoard(board, to, movingPieceType, movingPlayerIsWhite);
    }

    // 3. Update castling rights
    uint8_t currentCastlingRights = undoInfo->previousBoard.castlingRights; // Use rights from before this move

    if (movingPieceType == KING_T) {
        if (movingPlayerIsWhite) board->castlingRights &= ~(WHITE_KINGSIDE_CASTLE | WHITE_QUEENSIDE_CASTLE);
        else board->castlingRights &= ~(BLACK_KINGSIDE_CASTLE | BLACK_QUEENSIDE_CASTLE);
    }
    // If a rook moves from its starting square
    if (movingPieceType == ROOK_T) {
        if (movingPlayerIsWhite) {
            if (from == SQ_A1) board->castlingRights &= ~WHITE_QUEENSIDE_CASTLE;
            else if (from == SQ_H1) board->castlingRights &= ~WHITE_KINGSIDE_CASTLE;
        } else {
            if (from == SQ_A8) board->castlingRights &= ~BLACK_QUEENSIDE_CASTLE;
            else if (from == SQ_H8) board->castlingRights &= ~BLACK_KINGSIDE_CASTLE;
        }
    }
    // If a rook is captured on its starting square (check original rights)
    if (isCapture && capturedPieceType == ROOK_T) {
        if (!movingPlayerIsWhite) { // Opponent's (White) rook was captured
            if (to == SQ_A1 && (currentCastlingRights & WHITE_QUEENSIDE_CASTLE)) board->castlingRights &= ~WHITE_QUEENSIDE_CASTLE;
            else if (to == SQ_H1 && (currentCastlingRights & WHITE_KINGSIDE_CASTLE)) board->castlingRights &= ~WHITE_KINGSIDE_CASTLE;
        } else { // Opponent's (Black) rook was captured
            if (to == SQ_A8 && (currentCastlingRights & BLACK_QUEENSIDE_CASTLE)) board->castlingRights &= ~BLACK_QUEENSIDE_CASTLE;
            else if (to == SQ_H8 && (currentCastlingRights & BLACK_KINGSIDE_CASTLE)) board->castlingRights &= ~BLACK_KINGSIDE_CASTLE;
        }
    }
    
    // 4. Handle castling: move the rook (king already moved by general logic)
    if (MOVE_IS_CASTLING(move)) {
        if (movingPlayerIsWhite) {
            if (to == SQ_G1) { // Kingside
                removePieceFromBoard(board, SQ_H1, ROOK_T, true);
                addPieceToBoard(board, SQ_F1, ROOK_T, true);
            } else { // Queenside (to == SQ_C1)
                removePieceFromBoard(board, SQ_A1, ROOK_T, true);
                addPieceToBoard(board, SQ_D1, ROOK_T, true);
            }
        } else { // Black castling
            if (to == SQ_G8) { // Kingside
                removePieceFromBoard(board, SQ_H8, ROOK_T, false);
                addPieceToBoard(board, SQ_F8, ROOK_T, false);
            } else { // Queenside (to == SQ_C8)
                removePieceFromBoard(board, SQ_A8, ROOK_T, false);
                addPieceToBoard(board, SQ_D8, ROOK_T, false);
            }
        }
    }

    // 5. Update en passant square
    if (MOVE_IS_DOUBLE_PAWN_PUSH(move)) {
        board->enPassantSquare = movingPlayerIsWhite ? (from + 8) : (from - 8);
    } else {
        board->enPassantSquare = SQ_NONE; 
    }

    // 6. Update halfmove clock
    if (isPawnMove || isCapture) {
        board->halfMoveClock = 0;
    } else {
        board->halfMoveClock++;
    }

    // 7. Update fullmove number
    if (!movingPlayerIsWhite) { // If black just moved
        board->fullMoveNumber++;
    }

    // 8. Switch side to move
    board->whiteToMove = !movingPlayerIsWhite;



    // 9. Update history
    if (board->historyIndex < 1000) { // Ensure we don't overflow history
        board->history[board->historyIndex++] = board->zobristKey;
    } else {
        // exit
        fprintf(stderr, "History overflow: too many moves made.\n");
        exit(1);
    }

        // 10. Update Zobrist key
    board->zobristKey = calculate_zobrist_key(board);

}

void undoMove(Board* board, Move move, const MoveUndoInfo* undoInfo) {
    *board = undoInfo->previousBoard; // Restore the entire board state
}
