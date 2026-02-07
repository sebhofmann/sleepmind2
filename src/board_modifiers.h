\
#ifndef BOARD_MODIFIERS_H
#define BOARD_MODIFIERS_H

#include "board.h"
#include "move.h" // For Move, PieceTypeToken, Square, promotion flags
#include "nnue.h" // For NNUEAccumulator

// Structure to hold information needed to undo a move
typedef struct {
    int capturedPieceType; // PieceTypeToken of captured piece (or NO_PIECE_TYPE)
    int oldEnPassantSquare;
    uint8_t oldCastlingRights;
    int oldHalfMoveClock;
    uint64_t oldZobristKey;
} MoveUndoInfo;

// --- Function Prototypes ---

/**
 * @brief Applies a given move to the board and records undo information.
 *        Updates the NNUE accumulator incrementally if provided.
 * 
 * @param board Pointer to the Board to be modified.
 * @param move The Move to apply.
 * @param undoInfo Pointer to MoveUndoInfo struct to store data for undoing this move.
 * @param nnue_acc Pointer to NNUEAccumulator for incremental updates (can be NULL).
 * @param nnue_net Pointer to NNUENetwork for incremental updates (can be NULL).
 */
void applyMove(Board* board, Move move, MoveUndoInfo* undoInfo, NNUEAccumulator* nnue_acc, const NNUENetwork* nnue_net);

/**
 * @brief Undoes a given move on the board using the recorded undo information.
 *        Updates the NNUE accumulator incrementally if provided.
 * 
 * @param board Pointer to the Board to be modified.
 * @param move The Move to undo.
 * @param undoInfo Pointer to MoveUndoInfo struct containing data from when the move was applied.
 * @param nnue_acc Pointer to NNUEAccumulator for incremental updates (can be NULL).
 * @param nnue_net Pointer to NNUENetwork for incremental updates (can be NULL).
 */
void undoMove(Board* board, Move move, const MoveUndoInfo* undoInfo, NNUEAccumulator* nnue_acc, const NNUENetwork* nnue_net);

PieceTypeToken getPieceTypeAtSquare(const Board* board, Square sq, bool* pieceIsWhite);
void addPieceToBoard(Board* board, Square sq, PieceTypeToken pieceType, bool isWhite);
void removePieceFromBoard(Board* board, Square sq, PieceTypeToken pieceType, bool isWhite);
PieceTypeToken getPieceTypeFromPromotionFlag(int promoFlag);
Bitboard* getMutablePieceBitboardPointer(Board* board, Square sq, bool isPieceWhite);
void clearCaptureSquareOnAllBitboards(Board* board, Square sq);



#endif // BOARD_MODIFIERS_H
