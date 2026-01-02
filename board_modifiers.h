\
#ifndef BOARD_MODIFIERS_H
#define BOARD_MODIFIERS_H

#include "board.h"
#include "move.h" // For Move, PieceTypeToken, Square, promotion flags

// Structure to hold information needed to undo a move
typedef struct {
    Board previousBoard; // Stores the entire board state before the move
} MoveUndoInfo;

// --- Function Prototypes ---

/**
 * @brief Applies a given move to the board and records undo information.
 * 
 * @param board Pointer to the Board to be modified.
 * @param move The Move to apply.
 * @param undoInfo Pointer to MoveUndoInfo struct to store data for undoing this move.
 */
void applyMove(Board* board, Move move, MoveUndoInfo* undoInfo);

/**
 * @brief Undoes a given move on the board using the recorded undo information.
 * 
 * @param board Pointer to the Board to be modified.
 * @param move The Move to undo.
 * @param undoInfo Pointer to MoveUndoInfo struct containing data from when the move was applied.
 */
void undoMove(Board* board, Move move, const MoveUndoInfo* undoInfo);

PieceTypeToken getPieceTypeAtSquare(const Board* board, Square sq, bool* pieceIsWhite);
void addPieceToBoard(Board* board, Square sq, PieceTypeToken pieceType, bool isWhite);
void removePieceFromBoard(Board* board, Square sq, PieceTypeToken pieceType, bool isWhite);
PieceTypeToken getPieceTypeFromPromotionFlag(int promoFlag);
Bitboard* getMutablePieceBitboardPointer(Board* board, Square sq, bool isPieceWhite);
void clearCaptureSquareOnAllBitboards(Board* board, Square sq);



#endif // BOARD_MODIFIERS_H
