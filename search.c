#include "search.h"
#include "evaluation.h" // Will be created later
#include "move_generator.h"
#include "board_modifiers.h"
#include "tt.h" // Will be created later
#include "board_io.h"
#include <stdio.h>
#include <limits.h> // Required for INT_MIN, INT_MAX
#include <stdlib.h> // For qsort if move ordering is implemented
#include <time.h>   // For clock()

#define MAX_PLY 64 // Maximum search depth

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
        printf("info depth %d score cp %d nodes %d time %ld pv %s\n",
               current_depth,
               best_score_overall, // This is the score for white, adjust if UCI needs perspective
               info->nodesSearched, // This is cumulative nodes for the entire IDS search
               time_spent_ms,
               move_str_uci);

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

    // TODO: Add Transposition Table Lookup here
    // HashEntry* tt_entry = tt_probe(board_hash, depth, alpha, beta);
    // if (tt_entry != NULL) return tt_entry->score;

    if (depth == 0) {
        return quiescence_search(board, alpha, beta, maximizingPlayer, info, ply);
    }

    MoveList move_list;
    generateMoves(board, &move_list);

    if (move_list.count == 0) {
        // TODO: Check for checkmate or stalemate
        // For now, return 0 for stalemate, -/+ MATE_SCORE for checkmate
        // This needs a function like is_square_attacked
        return 0; // Simplified: could be stalemate or checkmate
    }

    // TODO: Implement move ordering (MVV-LVA, killer moves, history heuristic)

    Move best_move_this_node = 0;

    if (maximizingPlayer) {
        int max_eval = INT_MIN + 1;
        for (int i = 0; i < move_list.count; ++i) {
            Move current_move = move_list.moves[i];
            MoveUndoInfo undo_info;
            applyMove(board, current_move, &undo_info);
            int eval = alpha_beta_search(board, depth - 1, alpha, beta, false, info, ply + 1);
            printf("%s", outputFEN(board)); // For debugging, print the board state after applying the move
            undoMove(board, current_move, &undo_info);
            printf("DEBUG: Completed move %d/%d: %u with eval %d at depth %d, ply %d, nodes: %i\n",
                   i + 1, move_list.count, current_move, eval, depth, ply, info->nodesSearched);


            // If search was stopped by a deeper call, and we are at a non-root node, propagate a neutral score.
            // At the root (ply == 0), we want to continue to update bestMoveThisIteration with the current eval,
            // and then the outer loop in iterative_deepening_search will handle the stop.
            if (info->stopSearch && ply > 0) {
                return 0; 
            }

            if (eval > max_eval) {
                max_eval = eval;
                if (ply == 0) { // Root node
                    info->bestMoveThisIteration = current_move;
                }
                // best_move_this_node = current_move; // For TT
            }
            alpha = (alpha > eval) ? alpha : eval; // max(alpha, eval)
            
            if (beta <= alpha) {
                // TODO: Store in Transposition Table (type EXACT or LOWERBOUND)
                break; // Beta cut-off
            }

            // If at the root and search is stopped, break from iterating more root moves.
            // The best move found so far (if any) is already in info->bestMoveThisIteration.
            if (ply == 0 && info->stopSearch) {
                break;
            }
        }
        // TODO: Store in Transposition Table if not a beta cutoff
        return max_eval;
    } else { // minimizingPlayer
        int min_eval = INT_MAX;
        for (int i = 0; i < move_list.count; ++i) {
            Move current_move = move_list.moves[i];
            MoveUndoInfo undo_info;
            applyMove(board, current_move, &undo_info);
            int eval = alpha_beta_search(board, depth - 1, alpha, beta, true, info, ply + 1);
            undoMove(board, current_move, &undo_info);

            if (info->stopSearch && ply > 0) {
                return 0; 
            }

            if (eval < min_eval) {
                min_eval = eval;
                 if (ply == 0) { // Root node
                    info->bestMoveThisIteration = current_move;
                }
                // best_move_this_node = current_move; // For TT
            }
            beta = (beta < eval) ? beta : eval; // min(beta, eval)

            if (beta <= alpha) {
                // TODO: Store in Transposition Table (type EXACT or UPPERBOUND)
                break; // Alpha cut-off
            }
            
            if (ply == 0 && info->stopSearch) {
                break;
            }
        }
        // TODO: Store in Transposition Table if not an alpha cutoff
        return min_eval;
    }
}

int quiescence_search(Board* board, int alpha, int beta, bool maximizingPlayer, SearchInfo* info, int ply) {
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
    // TODO: Add Transposition Table Lookup here for quiescence if desired

    int stand_pat_score = evaluate(board); // Evaluate the current position "as is"

    if (maximizingPlayer) {
        if (stand_pat_score >= beta) {
            return beta; // Fail-high
        }
        if (stand_pat_score > alpha) {
            alpha = stand_pat_score;
        }
    } else {
        if (stand_pat_score <= alpha) {
            return alpha; // Fail-low
        }
        if (stand_pat_score < beta) {
            beta = stand_pat_score;
        }
    }

    MoveList capture_moves;
    generateMoves(board, &capture_moves); // In a real engine, this would be generate_captures_promotions

    // TODO: Order capture moves (e.g., MVV-LVA: Most Valuable Victim - Least Valuable Aggressor)

    for (int i = 0; i < capture_moves.count; ++i) {
        Move current_move = capture_moves.moves[i];
        // Only consider captures and promotions in quiescence search
        if (!MOVE_IS_CAPTURE(current_move) && !MOVE_IS_PROMOTION(current_move)) {
            continue;
        }

        MoveUndoInfo undo_info;
        applyMove(board, current_move, &undo_info);
        int score = quiescence_search(board, alpha, beta, !maximizingPlayer, info, ply + 1);
        undoMove(board, current_move, &undo_info);
        
        if (info->stopSearch) return 0;

        if (maximizingPlayer) {
            if (score >= beta) {
                // TODO: Store in TT (UPPERBOUND)
                return beta; // Fail-high
            }
            if (score > alpha) {
                alpha = score;
            }
        } else { // Minimizing player
            if (score <= alpha) {
                // TODO: Store in TT (LOWERBOUND)
                return alpha; // Fail-low
            }
            if (score < beta) {
                beta = score;
            }
        }
    }

    // If maximizing, alpha is the best score found. If minimizing, beta is the best score found.
    // However, the function should return from the perspective of the side to move.
    // The stand_pat score was already compared, so if no captures improve it, that's the score.
    return maximizingPlayer ? alpha : beta;
}

