#include "search.h"
#include "evaluation.h" // Will be created later
#include "move_generator.h"
#include "board_modifiers.h"
#include "tt.h" // Will be created later
#include "board_io.h"
#include "board.h" // For Piece type and piece constants (PAWN, KNIGHT, etc.)
#include "move.h"  // For GET_MOVE_PROMOTED, GET_MOVE_CAPTURED, GET_MOVE_PIECE etc.
#include <stdio.h>
#include <limits.h> // Required for INT_MIN, INT_MAX
#include <stdlib.h> // For qsort if move ordering is implemented
#include <time.h>   // For clock()

#define MAX_PLY 64 // Maximum search depth

// Pruning constants
#define FUTILITY_MARGIN 150      // Margin for futility pruning (e.g., roughly a pawn and a bit)
#define STATIC_NULL_MOVE_DEPTH 5 // Depth for static null move pruning / futility
#define LMP_DEPTH_THRESHOLD 4    // Minimum depth for LMP
#define LMP_QUIET_MOVE_COUNT 6   // Number of quiet moves to try before considering LMP
#define DELTA_PRUNING_MARGIN 300 // Margin for delta pruning in Q-search (e.g., a rook's value)
#define QUEEN_VALUE_FOR_DELTA 900 // Value of a queen for delta pruning promotions

// Internal Iterative Deepening (IID) constants
#define IID_MIN_DEPTH 5          // Minimum depth to consider IID
#define IID_REDUCTION_DEPTH 2    // Depth reduction for IID search
#define IID_CONDITION_MOVE_INDEX 1 // Apply IID for moves at or after this 0-based index

// Piece values for move ordering (ideally from evaluation.h)
#define ORDER_PAWN_VALUE 100
#define ORDER_KNIGHT_VALUE 300
#define ORDER_BISHOP_VALUE 310
#define ORDER_ROOK_VALUE 500
#define ORDER_QUEEN_VALUE 900

// Helper function to get piece value for ordering
static int get_piece_order_value(PieceTypeToken piece_type) {
    // Remove color from piece type for value lookup
    piece_type &= 0x7; // Mask to get base piece type (PAWN, KNIGHT, etc.)
    switch (piece_type) {
        case PAWN_T: return ORDER_PAWN_VALUE;
        case KNIGHT_T: return ORDER_KNIGHT_VALUE;
        case BISHOP_T: return ORDER_BISHOP_VALUE;
        case ROOK_T: return ORDER_ROOK_VALUE;
        case QUEEN_T: return ORDER_QUEEN_VALUE;
        default: return 0;
    }
}

static int get_promotion_order_value(int piece_type) {
    // Remove color from piece type for value lookup
    piece_type &= 0x7; // Mask to get base piece type (PAWN, KNIGHT, etc.)
    switch (piece_type) {
        case PROMOTION_N: return ORDER_KNIGHT_VALUE;
        case PROMOTION_B: return ORDER_BISHOP_VALUE;
        case PROMOTION_R: return ORDER_ROOK_VALUE;
        case PROMOTION_Q: return ORDER_QUEEN_VALUE;
        default: return 0;
    }
}

// Structure to hold a move and its score for ordering
typedef struct {
    Move move;
    int score;
} ScoredMove;

// Comparison function for qsort to sort moves in descending order of score
static int compare_scored_moves(const void* a, const void* b) {
    ScoredMove* moveA = (ScoredMove*)a;
    ScoredMove* moveB = (ScoredMove*)b;
    return moveB->score - moveA->score; // Descending order
}


// Placeholder for evaluation function
int evaluate(const Board* board);


Move iterative_deepening_search(Board* board, SearchInfo* info) {
    printf("DEBUG: Entered iterative_deepening_search. White to move: %s\n", board->whiteToMove ? "true" : "false");
    Move best_move_overall = 0;
    // Initialize best_score_overall to a very bad score for the current player
    int best_score_overall = board->whiteToMove ? INT_MIN : INT_MAX;
    
    info->nodesSearched = 0;
    // info->stopSearch should be initialized to false by the caller (e.g., UCI)
    // info->startTime should be initialized by the caller
    for (int i = 0; i < MAX_PLY; i++) {
        info->pv_length[i] = 0;
    }

    for (int current_depth = 1; current_depth <= MAX_PLY; ++current_depth) {
        // Ensure info->bestMoveThisIteration is reset or properly handled if alpha_beta doesn't always set it at ply 0 for every call
        // However, the log implies it is being set.
        
        int score_this_iteration = alpha_beta_search(board, current_depth, INT_MIN + 1, INT_MAX - 1, board->whiteToMove, info, 0);

        // If stopSearch was set during alpha_beta (e.g., by time limit check within alpha_beta for root moves, or by an external signal),
        // the results of this iteration might be incomplete or unreliable.
        // In this case, we break and use the results from the *previous* fully completed iteration,
        // which are already stored in best_move_overall and best_score_overall.
        if (info->stopSearch) {
            printf("info string Search stopped during depth %d. Using results from previous depth if available.\n", current_depth);
            break;
        }

        // If we reach here, the search for current_depth completed successfully.
        // Update our overall best move and score with the findings from this iteration.
        // The user's log "Best move this iteration: 772" indicates info->bestMoveThisIteration is set.
        best_move_overall = info->bestMoveThisIteration;
        best_score_overall = score_this_iteration; // Or use info->bestScoreThisIteration if it's more directly tied by alpha_beta

        // UCI output and user's debug log
        char move_str_uci[10]; // e.g., "e2e4q\0"
        moveToString(best_move_overall, move_str_uci);
        
        long time_spent_ms = 0;
        // Ensure info->startTime was set by the caller
        if (info->startTime != 0) {
             time_spent_ms = (long)((clock() - info->startTime) * 1000.0 / CLOCKS_PER_SEC);
        }

        // Standard UCI info output
        printf("info depth %d score cp %d nodes %d time %ld pv",
               current_depth,
               best_score_overall, // This is the score for white, adjust if UCI needs perspective
               info->nodesSearched, // This is cumulative nodes for the entire IDS search
               time_spent_ms);
        for (int i = 0; i < info->pv_length[0]; i++) {
            char move_str_pv[10];
            moveToString(info->pv_table[0][i], move_str_pv);
            printf(" %s", move_str_pv);
        }
        printf("\n");

        // User's specific debug log line, using the confirmed best move and score for this iteration
        printf("DEBUG: iterative_deepening_search: Depth %d finished. Score: %d, Best move this iteration: %u\n",
               current_depth, best_score_overall, best_move_overall);

        // Check for overall time limit for the search to stop further iterations
        if (info->timeLimit > 0 && time_spent_ms >= info->timeLimit) {
            printf("info string Time limit reached after completing depth %d. Stopping search.\n", current_depth);
            info->stopSearch = true; // Set to ensure loop terminates if not already broken
            break; 
        }
        
        // Optional: if a mate score is found, can stop early
        // Adjust KING_VALUE and MAX_PLY if necessary for mate detection logic
        // (KING_VALUE is in evaluation.h, ensure it's accessible or define a local constant)
        // Example: if (abs(best_score_overall) > (KING_VALUE - MAX_PLY * 10)) // Check against a threshold indicating mate
        // {
        //     printf("info string Mate found at depth %d. Stopping search.\n", current_depth);
        //     break;
        // }
    } // End of for loop for current_depth

    printf("DEBUG: Exiting iterative_deepening_search. Overall best move: %u\n", best_move_overall);
    return best_move_overall;
}

int alpha_beta_search(Board* board, int depth, int alpha, int beta, bool maximizingPlayer, SearchInfo* info, int ply) {
    info->nodesSearched++;
    info->pv_length[ply] = ply; // Initialize PV length for this ply

    if (ply > 0) { // Don't check time at root, iterative deepening handles that
        if ((info->nodesSearched & 2047) == 0) { // Check time every 2048 nodes
            if (info->timeLimit > 0 && (long)(((double)(clock() - info->startTime) / CLOCKS_PER_SEC) * 1000.0) > info->timeLimit) {
                info->stopSearch = true;
            }
        }
        if (info->stopSearch) {
            return 0; // Return a neutral score if search is stopped
        }
    }
    
    int repetitionCount = 0; 
    for(int i = 0; i < board->historyIndex; ++i) {
        if (board->history[i] == board->zobristKey) {
            repetitionCount++;
        }
    }
    if(repetitionCount >= 2) {
        return 0; 
    }

    if(ply > 0 && board->halfMoveClock >= 100) {
        return 0; // Draw by 50-move rule
    }

    // Transposition Table Lookup
    // TTEntry* tt_entry = tt_probe(board->zobristKey, depth, &alpha, &beta);
    // if (tt_entry != NULL && tt_entry->depth >= depth) {
        
    //     return tt_entry->score;
    // }

    TTEntry* tt_entry = tt_probe(board->zobristKey);
    if (tt_entry != NULL && tt_entry->depth >= depth) {
        // TODO: Add logic for repetition check and mate/stalemate from the user's example if applicable here
        // For now, directly use the TT entry if conditions are met, similar to the user's logic
        if (tt_entry->flag == TT_EXACT) {
            // If ply is 0, this is the root. We need to ensure bestMoveThisIteration is set.
            // The original code set bestMoveThisIteration *after* the recursive call.
            // If we return early due to TT, we need to consider how bestMoveThisIteration is handled.
            // For now, let's assume that if we have an EXACT score from TT at the root, 
            // the bestMove stored in TT is the one we want for this iteration.
            if (ply == 0 && tt_entry->bestMove != 0) {
                info->bestMoveThisIteration = tt_entry->bestMove;
            }
            return tt_entry->score;
        }
        if (tt_entry->flag == TT_LOWERBOUND && tt_entry->score >= beta) {
            if (ply == 0 && tt_entry->bestMove != 0) {
                info->bestMoveThisIteration = tt_entry->bestMove;
            }
            return tt_entry->score; // Or just beta, as it's a lower bound causing a cutoff
        }
        if (tt_entry->flag == TT_UPPERBOUND && tt_entry->score <= alpha) {
            if (ply == 0 && tt_entry->bestMove != 0) {
                info->bestMoveThisIteration = tt_entry->bestMove;
            }
            return tt_entry->score; // Or just alpha, as it's an upper bound causing a cutoff
        }
        // If the entry didn't cause a cutoff but is valid, its bestMove can still be used for move ordering later.
    }

    if (depth == 0) {
        return quiescence_search(board, alpha, beta, maximizingPlayer, info, ply);
    }

    // --- Futility Pruning (Static) ---
    // Apply only if not in check, at low depths, and not near mate scores
    if (depth <= STATIC_NULL_MOVE_DEPTH &&
        !isKingAttacked(board, board->whiteToMove) &&
        abs(alpha) < (MATE_SCORE - MAX_PLY) && abs(beta) < (MATE_SCORE - MAX_PLY)) {
        int static_eval = evaluate(board);
        if (maximizingPlayer) {
            if (static_eval + FUTILITY_MARGIN * depth <= alpha) { // Multiply margin by depth
                return alpha; // Prune
            }
        } else {
            if (static_eval - FUTILITY_MARGIN * depth >= beta) {
                return beta; // Prune
            }
        }
    }


    MoveList move_list;
    generateMoves(board, &move_list);


    if (move_list.count == 0) {
        // No legal moves available, check for checkmate or stalemate
        if(isKingAttacked(board, board->whiteToMove)) {
            // If the king is attacked and no moves are available, it's checkmate
            return maximizingPlayer ? -MATE_SCORE : MATE_SCORE; // Return negative for white's turn, positive
        }

        // TODO: Check for checkmate or stalemate
        // For now, return 0 for stalemate, -/+ MATE_SCORE for checkmate
        // This needs a function like is_square_attacked
        return 0; // Simplified: could be stalemate or checkmate
    }

    // --- Move Ordering Start ---
    ScoredMove scored_moves[256]; // Max moves based on MoveList array size
    int num_moves = move_list.count;

    for (int i = 0; i < num_moves; ++i) {
        scored_moves[i].move = move_list.moves[i];
        scored_moves[i].score = 0; // Default score for quiet moves

        Move current_gen_move = scored_moves[i].move;

        // 1. Transposition Table Move
        if (tt_entry != NULL && tt_entry->bestMove == current_gen_move) {
            // Give TT move the highest score, regardless of tt_entry->depth for ordering purposes
            // The actual TT hit logic (for cutoff) will check depth.
            scored_moves[i].score = 2000000;
        }
        // 2. Promotions (only if not already scored as TT move)
        if (scored_moves[i].score == 0 && MOVE_IS_PROMOTION(current_gen_move)) {
            int promotionPieceType = MOVE_PROMOTION(current_gen_move);


            if (promotionPieceType == PROMOTION_Q) { // Check base piece type
                scored_moves[i].score = 1900000; // Queen promotion
            } else {
                scored_moves[i].score = 1800000 + get_promotion_order_value(promotionPieceType); // Other promotions
            }
        }
        // 3. Captures (MVV-LVA - Most Valuable Victim, Least Valuable Aggressor)
        // (only if not already scored as TT or promotion)
        if (scored_moves[i].score == 0 && MOVE_IS_CAPTURE(current_gen_move)) {
            bool isWhite = board->whiteToMove;
            bool isBlack = !isWhite;
            PieceTypeToken victim = getPieceTypeAtSquare(board, MOVE_TO(current_gen_move), &isBlack); // Get victim piece type
            PieceTypeToken aggressor_piece_type = getPieceTypeAtSquare(board, MOVE_FROM(current_gen_move), &isWhite); // Piece being moved

            int victim_value = get_piece_order_value(victim);
            int aggressor_value = get_piece_order_value(aggressor_piece_type);

            // Score captures: base value + (victim value * 10 - aggressor value)
            // This prioritizes capturing high-value pieces with low-value pieces.
            scored_moves[i].score = 1000000 + (victim_value * 10 - aggressor_value);
        }
        // TODO: Add Killer Moves, History Heuristic scoring here
    }

    qsort(scored_moves, num_moves, sizeof(ScoredMove), compare_scored_moves);
    // --- Move Ordering End ---

    Move best_move_this_node = 0;
    int moves_searched_count = 0; // For LMP
    // int best_score_this_node = maximizingPlayer ? (INT_MIN +1): INT_MAX; // keep track of best score found at this node

    if (maximizingPlayer) {
        int max_eval = INT_MIN + 1;
        for (int i = 0; i < num_moves; ++i) { // Iterate through sorted moves
            Move current_move = scored_moves[i].move; // Get move from sorted list
            MoveUndoInfo undo_info_for_move;
            int eval;

            // --- Internal Iterative Deepening (IID) ---
            bool apply_iid = (
                depth >= IID_MIN_DEPTH &&
                i >= IID_CONDITION_MOVE_INDEX &&
                !MOVE_IS_CAPTURE(current_move) &&
                !MOVE_IS_PROMOTION(current_move) &&
                !isKingAttacked(board, board->whiteToMove) // Don't do IID if in check
            );

            if (apply_iid) {
                int iid_search_depth = depth - IID_REDUCTION_DEPTH;
                if (iid_search_depth < 1) iid_search_depth = 1; // Ensure positive depth

                applyMove(board, current_move, &undo_info_for_move);
                int iid_eval = alpha_beta_search(board, iid_search_depth, alpha, beta, false, info, ply + 1);
                undoMove(board, current_move, &undo_info_for_move);

                if (info->stopSearch && ply > 0) return 0; // Check after IID recursive call

                if (iid_eval <= alpha) { // IID suggests this move won't raise alpha
                    eval = iid_eval; // Use IID score, skip full-depth search for this move
                } else {
                    // IID was promising or didn't prune, proceed with full-depth search
                    applyMove(board, current_move, &undo_info_for_move);
                    eval = alpha_beta_search(board, depth - 1, alpha, beta, false, info, ply + 1);
                    undoMove(board, current_move, &undo_info_for_move);
                }
            } else {
                // No IID, or conditions not met, proceed with normal full-depth search
                applyMove(board, current_move, &undo_info_for_move);
                eval = alpha_beta_search(board, depth - 1, alpha, beta, false, info, ply + 1);
                undoMove(board, current_move, &undo_info_for_move);
            }

            if (info->stopSearch && ply > 0) { // Check after main recursive call (or IID if it was the only one)
                return 0; 
            }
            moves_searched_count++;

            if (eval > max_eval) {
                max_eval = eval;
                if (ply == 0) { // Root node
                    info->bestMoveThisIteration = current_move;
                }
                best_move_this_node = current_move;
                // Update PV table
                info->pv_table[ply][ply] = current_move;
                for (int next_ply = ply + 1; next_ply < info->pv_length[ply + 1] + ply + 1; next_ply++) {
                    info->pv_table[ply][next_ply] = info->pv_table[ply + 1][next_ply];
                }
                info->pv_length[ply] = info->pv_length[ply + 1] + 1;
            }
            alpha = (alpha > eval) ? alpha : eval; // max(alpha, eval)
            
            if (beta <= alpha) {
                // Speichere in TT
                tt_store(board->zobristKey, depth, max_eval, TT_LOWERBOUND, best_move_this_node);
                break; // Beta cut-off
            }

            // --- Late Move Pruning (LMP) ---
            // Apply if depth is sufficient, not in check (implicitly handled by futility check earlier, or add explicit check if needed)
            // and a certain number of quiet moves have been searched without raising alpha.
            if (depth >= LMP_DEPTH_THRESHOLD &&
                moves_searched_count >= LMP_QUIET_MOVE_COUNT &&
                !MOVE_IS_CAPTURE(current_move) && !MOVE_IS_PROMOTION(current_move) &&
                !isKingAttacked(board, board->whiteToMove) && // Ensure not in check after the move (or before if preferred)
                max_eval < beta - FUTILITY_MARGIN) { // Check if significantly below beta
                // This condition implies that previous quiet moves didn't raise alpha significantly.
                // The actual pruning happens by breaking if conditions are met for subsequent quiet moves.
                // For LMP, often it's about reducing the depth of subsequent quiet moves,
                // or pruning them if many quiet moves have failed to improve alpha.
                // Here, we'll implement a simple version: if many quiet moves didn't improve alpha, break.
                // A more common LMP is to reduce depth for later quiet moves.
                // This implementation is a simplified "break if many quiet moves don't improve alpha"
                if (eval <= alpha) { // If this quiet move also doesn't improve alpha
                    break; // Prune remaining quiet moves at this node
                }
            }


            if (ply == 0 && info->stopSearch) {
                break;
            }
        }
        // Speichere in TT wenn kein Beta-Cutoff
        if (alpha < beta) {
            tt_store(board->zobristKey, depth, max_eval, TT_EXACT, best_move_this_node);
        }
        return max_eval;
    } else { // minimizingPlayer
        int min_eval = INT_MAX;
        for (int i = 0; i < num_moves; ++i) { // Iterate through scored moves
            Move current_move = scored_moves[i].move; // Get move from sorted list
            MoveUndoInfo undo_info_for_move;
            int eval;

            // --- Internal Iterative Deepening (IID) ---
            bool apply_iid = (
                depth >= IID_MIN_DEPTH &&
                i >= IID_CONDITION_MOVE_INDEX &&
                !MOVE_IS_CAPTURE(current_move) &&
                !MOVE_IS_PROMOTION(current_move) &&
                !isKingAttacked(board, board->whiteToMove) // Don't do IID if in check
            );

            if (apply_iid) {
                int iid_search_depth = depth - IID_REDUCTION_DEPTH;
                if (iid_search_depth < 1) iid_search_depth = 1; // Ensure positive depth

                applyMove(board, current_move, &undo_info_for_move);
                int iid_eval = alpha_beta_search(board, iid_search_depth, alpha, beta, true, info, ply + 1);
                undoMove(board, current_move, &undo_info_for_move);

                if (info->stopSearch && ply > 0) return 0; // Check after IID recursive call

                if (iid_eval >= beta) { // IID suggests this move won't lower beta
                    eval = iid_eval; // Use IID score, skip full-depth search for this move
                } else {
                    // IID was promising or didn't prune, proceed with full-depth search
                    applyMove(board, current_move, &undo_info_for_move);
                    eval = alpha_beta_search(board, depth - 1, alpha, beta, true, info, ply + 1);
                    undoMove(board, current_move, &undo_info_for_move);
                }
            } else {
                // No IID, or conditions not met, proceed with normal full-depth search
                applyMove(board, current_move, &undo_info_for_move);
                eval = alpha_beta_search(board, depth - 1, alpha, beta, true, info, ply + 1);
                undoMove(board, current_move, &undo_info_for_move);
            }

            if (info->stopSearch && ply > 0) { // Check after main recursive call (or IID if it was the only one)
                return 0; 
            }
            moves_searched_count++;

            if (eval < min_eval) {
                min_eval = eval;
                if (ply == 0) { // Root node
                    info->bestMoveThisIteration = current_move;
                }
                best_move_this_node = current_move;
                // Update PV table
                info->pv_table[ply][ply] = current_move;
                for (int next_ply = ply + 1; next_ply < info->pv_length[ply + 1] + ply + 1; next_ply++) {
                    info->pv_table[ply][next_ply] = info->pv_table[ply + 1][next_ply];
                }
                info->pv_length[ply] = info->pv_length[ply + 1] + 1;
            }
            beta = (beta < eval) ? beta : eval; // min(beta, eval)

            if (beta <= alpha) {
                // Speichere in TT
                tt_store(board->zobristKey, depth, min_eval, TT_UPPERBOUND, best_move_this_node);
                break; // Alpha cut-off
            }

            // --- Late Move Pruning (LMP) ---
            if (depth >= LMP_DEPTH_THRESHOLD &&
                moves_searched_count >= LMP_QUIET_MOVE_COUNT &&
                !MOVE_IS_CAPTURE(current_move) && !MOVE_IS_PROMOTION(current_move) &&
                !isKingAttacked(board, board->whiteToMove) &&
                min_eval > alpha + FUTILITY_MARGIN) { // Check if significantly above alpha
                 if (eval >= beta) { // If this quiet move also doesn't improve beta
                    break; // Prune remaining quiet moves at this node
                 }
            }
            
            if (ply == 0 && info->stopSearch) {
                break;
            }
        }
        // Speichere in TT wenn kein Alpha-Cutoff
        if (alpha < beta) {
            tt_store(board->zobristKey, depth, min_eval, TT_EXACT, best_move_this_node);
        }
        return min_eval;
    }
}

int quiescence_search(Board* board, int alpha, int beta, bool maximizingPlayer, SearchInfo* info, int ply) {
    int original_alpha = alpha;
    int original_beta = beta;

    info->nodesSearched++;
    
    if ((info->nodesSearched & 2047) == 0) { // Check time every 2048 nodes
        if (info->timeLimit > 0 && (long)(((double)(clock() - info->startTime) / CLOCKS_PER_SEC) * 1000.0) > info->timeLimit) {
            info->stopSearch = true;
        }
    }
    if (info->stopSearch) {
        return 0; 
    }

    int repetitionCount = 0; 
    for(int i = 0; i < board->historyIndex; ++i) {
        if (board->history[i] == board->zobristKey) {
            repetitionCount++;
        }
    }
    if(repetitionCount >= 2) {
        return 0; 
    }

    // --- Transposition Table Probe ---
    // Quiescence search entries are stored with depth 0
    TTEntry* tt_entry = tt_probe(board->zobristKey);
    if (tt_entry != NULL && tt_entry->depth == 0) { // Check for key match and if it's a q-search entry
        if (tt_entry->flag == TT_EXACT) {
            return tt_entry->score;
        }
        if (maximizingPlayer) {
            if (tt_entry->flag == TT_LOWERBOUND && tt_entry->score >= beta) {
                return tt_entry->score; // Using stored score, could also be 'return beta;'
            }
            if (tt_entry->flag == TT_UPPERBOUND && tt_entry->score <= alpha) {
                return tt_entry->score; // Using stored score, could also be 'return alpha;'
            }
        } else { // minimizingPlayer
            if (tt_entry->flag == TT_UPPERBOUND && tt_entry->score <= alpha) {
                return tt_entry->score; // Using stored score, could also be 'return alpha;'
            }
            if (tt_entry->flag == TT_LOWERBOUND && tt_entry->score >= beta) {
                return tt_entry->score; // Using stored score, could also be 'return beta;'
            }
        }
    }


    int stand_pat_score = evaluate(board); // Evaluate the current position "as is"

    if (maximizingPlayer) {
        if (stand_pat_score >= beta) {
            // Store before returning beta (LOWERBOUND)
            tt_store(board->zobristKey, 0, beta, TT_LOWERBOUND, 0);
            return beta;
        }
        if (stand_pat_score > alpha) {
            alpha = stand_pat_score;
        }
    } else { // minimizingPlayer
        if (stand_pat_score <= alpha) {
            // Store before returning alpha (UPPERBOUND)
            tt_store(board->zobristKey, 0, alpha, TT_UPPERBOUND, 0);
            return alpha;
        }
        if (stand_pat_score < beta) {
            beta = stand_pat_score;
        }
    }

    // MoveList capture_moves; // Original line
    // generateMoves(board, &capture_moves); // Original line: In a real engine, this would be generate_captures_promotions

    MoveList forcing_moves; // New list for captures and promotions
    generateCaptureAndPromotionMoves(board, &forcing_moves); // Generate only forcing moves

    // If in check, and no forcing moves, it might be mate.
    // However, stand_pat already handles the checkmate case if generateMoves was called before.
    // Here, with only forcing moves, if in check and no forcing moves, it's likely mate.
    // But evaluate() should ideally return MATE_SCORE if it's checkmate.
    // The main alpha-beta search handles mate if no moves are generated.
    // Quiescence search's primary role is to stabilize noisy evaluations.

    // --- Delta Pruning (applied before move generation for captures) ---
    if (maximizingPlayer) {
        if (stand_pat_score + DELTA_PRUNING_MARGIN < alpha && ply < MAX_PLY -1) { 
            // If even with a large material gain (DELTA_PRUNING_MARGIN, e.g., a rook),
            // we can't raise alpha, then prune.
            // This is a rough check. More precise delta pruning is per-move.
            // For promotions to queen, the margin might need to be queen's value.
            if (stand_pat_score + QUEEN_VALUE_FOR_DELTA < alpha) {
                 // tt_store(board->zobristKey, 0, alpha, TT_UPPERBOUND, 0); // Or stand_pat_score
                 // return alpha; // Or stand_pat_score
            }
        }
    } else { // minimizingPlayer
        if (stand_pat_score - DELTA_PRUNING_MARGIN > beta && ply < MAX_PLY -1) {
            if (stand_pat_score - QUEEN_VALUE_FOR_DELTA > beta) {
                // tt_store(board->zobristKey, 0, beta, TT_LOWERBOUND, 0); // Or stand_pat_score
                // return beta; // Or stand_pat_score
            }
        }
    }


 // --- Move Ordering Start ---
    ScoredMove scored_moves[256]; // Max moves based on MoveList array size
    // int num_moves = capture_moves.count; // Original line
    int num_moves = forcing_moves.count; // Use count of forcing moves

    for (int i = 0; i < num_moves; ++i) {
        // scored_moves[i].move = capture_moves.moves[i]; // Original line
        scored_moves[i].move = forcing_moves.moves[i]; // Use forcing moves
        scored_moves[i].score = 0; // Default score for quiet moves

        Move current_gen_move = scored_moves[i].move;

        // 1. Transposition Table Move
        if (tt_entry != NULL && tt_entry->bestMove == current_gen_move) {
            // Give TT move the highest score, regardless of tt_entry->depth for ordering purposes
            // The actual TT hit logic (for cutoff) will check depth.
            scored_moves[i].score = 2000000;
        }
        // 2. Promotions (only if not already scored as TT move)
        if (scored_moves[i].score == 0 && MOVE_IS_PROMOTION(current_gen_move)) {
            int promotionPieceType = MOVE_PROMOTION(current_gen_move);


            if (promotionPieceType == PROMOTION_Q) { // Check base piece type
                scored_moves[i].score = 1900000; // Queen promotion
            } else {
                scored_moves[i].score = 1800000 + get_promotion_order_value(promotionPieceType); // Other promotions
            }
        }
        // 3. Captures (MVV-LVA - Most Valuable Victim, Least Valuable Aggressor)
        // (only if not already scored as TT or promotion)
        if (scored_moves[i].score == 0 && MOVE_IS_CAPTURE(current_gen_move)) {
            bool isWhite = board->whiteToMove;
            bool isBlack = !isWhite;
            PieceTypeToken victim = getPieceTypeAtSquare(board, MOVE_TO(current_gen_move), &isBlack); // Get victim piece type
            PieceTypeToken aggressor_piece_type = getPieceTypeAtSquare(board, MOVE_FROM(current_gen_move), &isWhite); // Piece being moved

            int victim_value = get_piece_order_value(victim);
            int aggressor_value = get_piece_order_value(aggressor_piece_type);

            // Score captures: base value + (victim value * 10 - aggressor value)
            // This prioritizes capturing high-value pieces with low-value pieces.
            scored_moves[i].score = 1000000 + (victim_value * 10 - aggressor_value);
        }
        // TODO: Add Killer Moves, History Heuristic scoring here
    }

    qsort(scored_moves, num_moves, sizeof(ScoredMove), compare_scored_moves);
    // --- Move Ordering End ---

    // for (int i = 0; i < capture_moves.count; ++i) { // Original line
    for (int i = 0; i < num_moves; ++i) { // Iterate over sorted forcing moves
        ScoredMove sm = scored_moves[i];
        Move current_move = sm.move;
        // The explicit check for capture/promotion is no longer strictly necessary here
        // if generateCaptureAndPromotionMoves() is used, as it only generates those.
        // However, keeping it doesn't harm and adds clarity if generateMoves() was used by mistake.
        // if (!MOVE_IS_CAPTURE(current_move) && !MOVE_IS_PROMOTION(current_move)) {
        //     continue;
        // }

        // --- Per-move Delta Pruning ---
        if (maximizingPlayer) {
            int material_gain = 0;
            if (MOVE_IS_CAPTURE(current_move)) {
                PieceTypeToken victim_type = getPieceTypeAtSquare(board, MOVE_TO(current_move), &(bool){false}); // Opponent's piece
                material_gain = get_piece_order_value(victim_type);
            }
            if (MOVE_IS_PROMOTION(current_move)) {
                // Add promotion piece value, subtract pawn value if it was a capture-promotion
                int promoted_to = MOVE_PROMOTION(current_move);
                material_gain += get_promotion_order_value(promoted_to) - (MOVE_IS_CAPTURE(current_move) ? 0 : ORDER_PAWN_VALUE);
            }
            if (stand_pat_score + material_gain + DELTA_PRUNING_MARGIN < alpha) {
                continue; // Prune this move
            }
        } else { // minimizingPlayer
            int material_loss = 0; // From minimizing player's perspective, gain for opponent is loss for them
            if (MOVE_IS_CAPTURE(current_move)) {
                PieceTypeToken victim_type = getPieceTypeAtSquare(board, MOVE_TO(current_move), &(bool){true}); // Opponent's piece
                material_loss = get_piece_order_value(victim_type);
            }
             if (MOVE_IS_PROMOTION(current_move)) {
                int promoted_to = MOVE_PROMOTION(current_move);
                material_loss += get_promotion_order_value(promoted_to) - (MOVE_IS_CAPTURE(current_move) ? 0 : ORDER_PAWN_VALUE);
            }
            if (stand_pat_score - material_loss - DELTA_PRUNING_MARGIN > beta) {
                continue; // Prune this move
            }
        }


        MoveUndoInfo undo_info;
        applyMove(board, current_move, &undo_info);
        int score = quiescence_search(board, alpha, beta, !maximizingPlayer, info, ply + 1);
        undoMove(board, current_move, &undo_info);
        
        if (info->stopSearch) return 0;

        if (maximizingPlayer) {
            if (score >= beta) {
                // Store before returning beta (LOWERBOUND)
                tt_store(board->zobristKey, 0, beta, TT_LOWERBOUND, 0);
                return beta; // Beta cutoff
            }
            if (score > alpha) {
                alpha = score;
            }
        } else { // Minimizing player
            if (score <= alpha) {
                // Store before returning alpha (UPPERBOUND)
                tt_store(board->zobristKey, 0, alpha, TT_UPPERBOUND, 0);
                return alpha; // Alpha cutoff
            }
            if (score < beta) {
                beta = score;
            }
        }
    }

    // Determine final score and TT flag
    int eval_score = maximizingPlayer ? alpha : beta;
    uint8_t tt_flag;

    if (maximizingPlayer) {
        if (eval_score >= original_beta) {
            tt_flag = TT_LOWERBOUND;
        } else if (eval_score <= original_alpha) { // Alpha was not improved beyond original
            tt_flag = TT_UPPERBOUND;
        } else {
            tt_flag = TT_EXACT;
        }
    } else { // minimizingPlayer
        if (eval_score <= original_alpha) {
            tt_flag = TT_UPPERBOUND;
        } else if (eval_score >= original_beta) { // Beta was not improved beyond original
            tt_flag = TT_LOWERBOUND;
        } else {
            tt_flag = TT_EXACT;
        }
    }
    
    tt_store(board->zobristKey, 0, eval_score, tt_flag, 0); // Depth 0, no best move

    return eval_score;
}

