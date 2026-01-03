#include "search.h"
#include "evaluation.h"
#include "move_generator.h"
#include "board_modifiers.h"
#include "tt.h"
#include "board_io.h"
#include "board.h"
#include "move.h"
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

// =============================================================================
// Constants
// =============================================================================

// Late Move Reduction parameters (conservative for weaker eval)
static const int LMR_FULL_DEPTH_MOVES = 6;  // Search more moves at full depth
static const int LMR_REDUCTION_LIMIT = 4;   // Higher minimum depth for LMR

// Null Move Pruning parameters (conservative)
static const int NULL_MOVE_REDUCTION = 2;   // Smaller R value for safety
static const int NULL_MOVE_MIN_DEPTH = 4;   // Higher minimum depth

// Futility pruning margins (disabled for now - eval not reliable enough)
// Set to 0 to effectively disable futility pruning
static const int FUTILITY_MARGIN = 0;
static const int EXTENDED_FUTILITY_MARGIN = 0;

// Delta pruning margin for quiescence (larger margin = less aggressive)
static const int DELTA_MARGIN = 400;

// Aspiration window
static const int ASPIRATION_WINDOW = 50;

// =============================================================================
// Piece values for move ordering
// =============================================================================

static int get_piece_value(PieceTypeToken piece_type) {
    piece_type &= 0x7;
    switch (piece_type) {
        case PAWN_T:   return 100;
        case KNIGHT_T: return 320;
        case BISHOP_T: return 330;
        case ROOK_T:   return 500;
        case QUEEN_T:  return 900;
        case KING_T:   return 20000;
        default:       return 0;
    }
}

static int get_promotion_value(int promo) {
    switch (promo) {
        case PROMOTION_N: return 320;
        case PROMOTION_B: return 330;
        case PROMOTION_R: return 500;
        case PROMOTION_Q: return 900;
        default:          return 0;
    }
}

// =============================================================================
// Move ordering structures
// =============================================================================

typedef struct {
    Move move;
    int score;
} ScoredMove;

static int compare_moves(const void* a, const void* b) {
    return ((ScoredMove*)b)->score - ((ScoredMove*)a)->score;
}

// =============================================================================
// Move scoring for ordering
// =============================================================================

static void score_moves(Board* board, MoveList* moves, ScoredMove* scored, 
                       Move tt_move, SearchInfo* info, int ply) {
    int side = board->whiteToMove ? 0 : 1;
    
    for (int i = 0; i < moves->count; i++) {
        Move m = moves->moves[i];
        scored[i].move = m;
        scored[i].score = 0;
        
        int from = MOVE_FROM(m);
        int to = MOVE_TO(m);
        
        // 1. TT move gets highest priority
        if (m == tt_move) {
            scored[i].score = 10000000;
            continue;
        }
        
        // 2. Winning/equal captures (MVV-LVA)
        if (MOVE_IS_CAPTURE(m)) {
            bool isWhite = board->whiteToMove;
            bool isBlack = !isWhite;
            PieceTypeToken victim = getPieceTypeAtSquare(board, to, &isBlack);
            PieceTypeToken attacker = getPieceTypeAtSquare(board, from, &isWhite);
            int victim_val = get_piece_value(victim);
            int attacker_val = get_piece_value(attacker);
            
            // MVV-LVA: prioritize capturing high value pieces with low value pieces
            int mvv_lva = victim_val * 10 - attacker_val;
            
            // Good captures get high priority, bad captures get low priority
            if (victim_val >= attacker_val) {
                scored[i].score = 8000000 + mvv_lva;  // Winning/equal capture
            } else {
                scored[i].score = 2000000 + mvv_lva;  // Losing capture (but still interesting)
            }
            continue;
        }
        
        // 3. Queen promotions
        if (MOVE_IS_PROMOTION(m)) {
            int promo = MOVE_PROMOTION(m);
            if (promo == PROMOTION_Q) {
                scored[i].score = 9000000;
            } else {
                scored[i].score = 7000000 + get_promotion_value(promo);
            }
            continue;
        }
        
        // 4. Killer moves
        if (ply < MAX_PLY) {
            if (m == info->killers[ply][0]) {
                scored[i].score = 6000000;
                continue;
            }
            if (m == info->killers[ply][1]) {
                scored[i].score = 5900000;
                continue;
            }
        }
        
        // 5. History heuristic for quiet moves
        scored[i].score = info->history[side][from][to];
    }
    
    qsort(scored, moves->count, sizeof(ScoredMove), compare_moves);
}

// Score moves for quiescence (simpler, only captures matter)
static void score_captures(Board* board, MoveList* moves, ScoredMove* scored) {
    for (int i = 0; i < moves->count; i++) {
        Move m = moves->moves[i];
        scored[i].move = m;
        scored[i].score = 0;
        
        if (MOVE_IS_CAPTURE(m)) {
            bool isWhite = board->whiteToMove;
            bool isBlack = !isWhite;
            PieceTypeToken victim = getPieceTypeAtSquare(board, MOVE_TO(m), &isBlack);
            PieceTypeToken attacker = getPieceTypeAtSquare(board, MOVE_FROM(m), &isWhite);
            int victim_val = get_piece_value(victim);
            int attacker_val = get_piece_value(attacker);
            scored[i].score = victim_val * 10 - attacker_val + 1000000;
        } else if (MOVE_IS_PROMOTION(m)) {
            scored[i].score = get_promotion_value(MOVE_PROMOTION(m)) + 500000;
        }
    }
    
    qsort(scored, moves->count, sizeof(ScoredMove), compare_moves);
}

// =============================================================================
// Helper functions
// =============================================================================

// Update killer moves
static void update_killers(SearchInfo* info, Move m, int ply) {
    if (ply >= MAX_PLY) return;
    
    // Don't add if already first killer
    if (info->killers[ply][0] == m) return;
    
    // Shift killers down and add new one
    info->killers[ply][1] = info->killers[ply][0];
    info->killers[ply][0] = m;
}

// Update history heuristic
static void update_history(SearchInfo* info, Board* board, Move m, int depth) {
    int side = board->whiteToMove ? 0 : 1;
    int from = MOVE_FROM(m);
    int to = MOVE_TO(m);
    
    // Bonus proportional to depth^2
    int bonus = depth * depth;
    
    // Aging: scale down and add bonus
    info->history[side][from][to] += bonus;
    
    // Cap history values to prevent overflow
    if (info->history[side][from][to] > 1000000) {
        // Age all history
        for (int s = 0; s < 2; s++) {
            for (int f = 0; f < 64; f++) {
                for (int t = 0; t < 64; t++) {
                    info->history[s][f][t] /= 2;
                }
            }
        }
    }
}

// Clear search heuristics
void clear_search_history(SearchInfo* info) {
    memset(info->killers, 0, sizeof(info->killers));
    memset(info->history, 0, sizeof(info->history));
    memset(info->counter_moves, 0, sizeof(info->counter_moves));
}

// Forward declaration
int evaluate(const Board* board);

// Check time limit (hard limit - sofortiger Abbruch)
static bool check_time(SearchInfo* info) {
    if (info->hardTimeLimit > 0) {
        long elapsed = (long)((clock() - info->startTime) * 1000.0 / CLOCKS_PER_SEC);
        if (elapsed >= info->hardTimeLimit) {
            info->stopSearch = true;
            return true;
        }
    }
    return false;
}

// Hilfsfunktion: Verstrichene Zeit in ms
static long get_elapsed_time(SearchInfo* info) {
    return (long)((clock() - info->startTime) * 1000.0 / CLOCKS_PER_SEC);
}

// Check if position is likely a draw
static bool is_draw(Board* board, int ply) {
    // Draw by 50-move rule
    if (board->halfMoveClock >= 100) {
        return true;
    }
    
    // Draw by repetition
    if (ply > 0) {
        int rep_count = 0;
        for (int i = 0; i < board->historyIndex; i++) {
            if (board->history[i] == board->zobristKey) {
                rep_count++;
                if (rep_count >= 2) {
                    return true;
                }
            }
        }
    }
    
    return false;
}

// Simple check if we can do null move (not in check, have pieces besides pawns)
static bool can_do_null_move(Board* board) {
    if (board->whiteToMove) {
        // Check if white has non-pawn material
        return (board->whiteKnights | board->whiteBishops | 
                board->whiteRooks | board->whiteQueens) != 0;
    } else {
        return (board->blackKnights | board->blackBishops | 
                board->blackRooks | board->blackQueens) != 0;
    }
}

// =============================================================================
// Quiescence Search
// =============================================================================

static int quiescence(Board* board, int alpha, int beta, SearchInfo* info, int ply) {
    info->nodesSearched++;
    
    // Update selective depth
    if (ply > info->seldepth) {
        info->seldepth = ply;
    }
    
    // Check time periodically
    if ((info->nodesSearched & 2047) == 0 && check_time(info)) {
        return 0;
    }
    if (info->stopSearch) return 0;
    
    // Max ply check
    if (ply >= MAX_PLY) {
        int eval = evaluate(board);
        return board->whiteToMove ? eval : -eval;
    }
    
    // Check if we're in check
    bool in_check = isKingAttacked(board, board->whiteToMove);
    
    // Generate all moves
    MoveList all_moves;
    generateMoves(board, &all_moves);
    
    // If no legal moves: checkmate or stalemate
    if (all_moves.count == 0) {
        if (in_check) {
            return -MATE_SCORE + ply;
        }
        return 0;
    }
    
    // If in check, we must search all moves (not just captures)
    if (in_check) {
        ScoredMove scored[256];
        score_captures(board, &all_moves, scored);
        
        for (int i = 0; i < all_moves.count; i++) {
            Move m = scored[i].move;
            
            MoveUndoInfo undo;
            applyMove(board, m, &undo);
            int score = -quiescence(board, -beta, -alpha, info, ply + 1);
            undoMove(board, m, &undo);
            
            if (info->stopSearch) return 0;
            
            if (score >= beta) {
                return beta;
            }
            if (score > alpha) {
                alpha = score;
            }
        }
        return alpha;
    }
    
    // Not in check: use stand pat
    int stand_pat = evaluate(board);
    if (!board->whiteToMove) {
        stand_pat = -stand_pat;
    }
    
    if (stand_pat >= beta) {
        return beta;
    }
    if (stand_pat > alpha) {
        alpha = stand_pat;
    }
    
    // Delta pruning: if we're so far behind that even capturing a queen won't help
    if (stand_pat + 900 + DELTA_MARGIN < alpha) {
        return alpha;
    }
    
    // Score and sort captures
    ScoredMove scored[256];
    score_captures(board, &all_moves, scored);
    
    for (int i = 0; i < all_moves.count; i++) {
        Move m = scored[i].move;
        
        // Only search captures and promotions
        if (!MOVE_IS_CAPTURE(m) && !MOVE_IS_PROMOTION(m)) {
            continue;
        }
        
        // Delta pruning: skip captures that can't possibly raise alpha
        if (!MOVE_IS_PROMOTION(m)) {
            bool isBlack = !board->whiteToMove;
            PieceTypeToken victim = getPieceTypeAtSquare(board, MOVE_TO(m), &isBlack);
            int gain = get_piece_value(victim);
            
            if (stand_pat + gain + DELTA_MARGIN < alpha) {
                continue;
            }
        }
        
        MoveUndoInfo undo;
        applyMove(board, m, &undo);
        int score = -quiescence(board, -beta, -alpha, info, ply + 1);
        undoMove(board, m, &undo);
        
        if (info->stopSearch) return 0;
        
        if (score >= beta) {
            return beta;
        }
        if (score > alpha) {
            alpha = score;
        }
    }
    
    return alpha;
}

// =============================================================================
// Negamax with Alpha-Beta Pruning
// =============================================================================

static int negamax(Board* board, int depth, int alpha, int beta, SearchInfo* info, 
                   int ply, bool do_null) {
    info->nodesSearched++;
    info->pv_length[ply] = 0;
    
    bool is_pv = (beta - alpha) > 1;  // Are we in a PV node?
    int original_alpha = alpha;
    
    // Check time periodically
    if (ply > 0 && (info->nodesSearched & 2047) == 0 && check_time(info)) {
        return 0;
    }
    if (info->stopSearch) return 0;
    
    // Draw detection
    if (ply > 0 && is_draw(board, ply)) {
        return 0;
    }
    
    // Max ply check
    if (ply >= MAX_PLY) {
        int eval = evaluate(board);
        return board->whiteToMove ? eval : -eval;
    }
    
    // Check if in check (needed for various extensions/reductions)
    bool in_check = isKingAttacked(board, board->whiteToMove);
    
    // Check extension
    if (in_check) {
        depth++;
    }
    
    // TT Probe
    Move tt_move = 0;
    TTEntry* tt_entry = tt_probe(board->zobristKey);
    if (tt_entry != NULL) {
        tt_move = tt_entry->bestMove;
        
        // Only use TT cutoff in non-PV nodes
        if (!is_pv && tt_entry->depth >= depth && ply > 0) {
            int tt_score = tt_entry->score;
            
            // Adjust mate scores
            if (tt_score > MATE_SCORE - 100) {
                tt_score -= ply;
            } else if (tt_score < -MATE_SCORE + 100) {
                tt_score += ply;
            }
            
            if (tt_entry->flag == TT_EXACT) {
                return tt_score;
            }
            if (tt_entry->flag == TT_LOWERBOUND && tt_score >= beta) {
                return tt_score;
            }
            if (tt_entry->flag == TT_UPPERBOUND && tt_score <= alpha) {
                return tt_score;
            }
        }
    }
    
    // Quiescence search at leaf nodes
    if (depth <= 0) {
        return quiescence(board, alpha, beta, info, ply);
    }
    
    // Static evaluation for pruning decisions
    int static_eval = evaluate(board);
    if (!board->whiteToMove) {
        static_eval = -static_eval;
    }
    
    // ==========================================================================
    // Null Move Pruning
    // ==========================================================================
    if (do_null && !in_check && !is_pv && depth >= NULL_MOVE_MIN_DEPTH && 
        can_do_null_move(board)) {
        
        // Make null move
        board->whiteToMove = !board->whiteToMove;
        int old_ep = board->enPassantSquare;
        board->enPassantSquare = SQ_NONE;
        
        // Search with reduced depth
        int null_score = -negamax(board, depth - 1 - NULL_MOVE_REDUCTION, -beta, -beta + 1, 
                                  info, ply + 1, false);
        
        // Unmake null move
        board->whiteToMove = !board->whiteToMove;
        board->enPassantSquare = old_ep;
        
        if (info->stopSearch) return 0;
        
        if (null_score >= beta) {
            // Always do verification search for safety (eval not yet reliable)
            int verify = negamax(board, depth - NULL_MOVE_REDUCTION - 1, beta - 1, beta, 
                                info, ply, false);
            if (verify >= beta) {
                return beta;
            }
        }
    }
    
    // ==========================================================================
    // Futility Pruning (disabled until eval is more reliable)
    // ==========================================================================
    bool futility_pruning = false;
    // Futility pruning requires a reliable evaluation function.
    // Enable this when NNUE or better eval is implemented:
    // if (!is_pv && !in_check && depth <= 2 && FUTILITY_MARGIN > 0 &&
    //     abs(alpha) < MATE_SCORE - 100 && abs(beta) < MATE_SCORE - 100) {
    //     int margin = (depth == 1) ? FUTILITY_MARGIN : EXTENDED_FUTILITY_MARGIN;
    //     if (static_eval + margin < alpha) {
    //         futility_pruning = true;
    //     }
    // }
    (void)static_eval; // Suppress unused warning when futility is disabled
    
    // Generate moves
    MoveList moves;
    generateMoves(board, &moves);
    
    // No legal moves: checkmate or stalemate
    if (moves.count == 0) {
        if (in_check) {
            return -MATE_SCORE + ply;
        }
        return 0;
    }
    
    // Score and sort moves
    ScoredMove scored[256];
    score_moves(board, &moves, scored, tt_move, info, ply);
    
    Move best_move = 0;
    int moves_searched = 0;
    
    for (int i = 0; i < moves.count; i++) {
        Move m = scored[i].move;
        bool is_capture = MOVE_IS_CAPTURE(m);
        bool is_promotion = MOVE_IS_PROMOTION(m);
        bool is_tactical = is_capture || is_promotion;
        
        // =======================================================================
        // Futility Pruning: skip quiet moves that can't improve alpha
        // =======================================================================
        if (futility_pruning && moves_searched > 0 && !is_tactical) {
            continue;
        }
        
        // Prefetch TT entry for likely next position
        // (This is a simple optimization that can help cache performance)
        
        MoveUndoInfo undo;
        applyMove(board, m, &undo);
        
        // Prefetch next position's TT entry
        tt_prefetch(board->zobristKey);
        
        int score;
        
        // =======================================================================
        // Principal Variation Search (PVS) with Late Move Reductions (LMR)
        // =======================================================================
        if (moves_searched == 0) {
            // First move: full window search
            score = -negamax(board, depth - 1, -beta, -alpha, info, ply + 1, true);
        } else {
            // Late Move Reductions
            int reduction = 0;
            
            if (!in_check && !is_tactical && depth >= LMR_REDUCTION_LIMIT && 
                moves_searched >= LMR_FULL_DEPTH_MOVES) {
// Base reduction (conservative)
            reduction = 1;
            
            // Increase reduction for later moves (less aggressive)
            if (moves_searched > 10) {
                reduction++;
            }
            // Removed extra reduction for very late moves
                
                // Reduce reduction for killer moves
                if (ply < MAX_PLY && (m == info->killers[ply][0] || m == info->killers[ply][1])) {
                    reduction--;
                }
                
                // Don't reduce below 1
                if (depth - 1 - reduction < 1) {
                    reduction = depth - 2;
                }
                if (reduction < 0) reduction = 0;
            }
            
            // Null window search with possible reduction
            score = -negamax(board, depth - 1 - reduction, -alpha - 1, -alpha, 
                            info, ply + 1, true);
            
            // Re-search if LMR failed high
            if (reduction > 0 && score > alpha) {
                score = -negamax(board, depth - 1, -alpha - 1, -alpha, info, ply + 1, true);
            }
            
            // Re-search with full window if null window failed high
            if (score > alpha && score < beta) {
                score = -negamax(board, depth - 1, -beta, -alpha, info, ply + 1, true);
            }
        }
        
        undoMove(board, m, &undo);
        moves_searched++;
        
        if (info->stopSearch) return 0;
        
        if (score > alpha) {
            alpha = score;
            best_move = m;
            
            // Update PV
            info->pv_table[ply][0] = m;
            for (int j = 0; j < info->pv_length[ply + 1]; j++) {
                info->pv_table[ply][j + 1] = info->pv_table[ply + 1][j];
            }
            info->pv_length[ply] = info->pv_length[ply + 1] + 1;
            
            if (ply == 0) {
                info->bestMoveThisIteration = m;
            }
        }
        
        if (alpha >= beta) {
            // Beta cutoff - update killers and history for quiet moves
            if (!is_capture) {
                update_killers(info, m, ply);
                update_history(info, board, m, depth);
            }
            break;
        }
    }
    
    // TT Store
    if (!info->stopSearch) {
        uint8_t tt_flag;
        if (alpha <= original_alpha) {
            tt_flag = TT_UPPERBOUND;
        } else if (alpha >= beta) {
            tt_flag = TT_LOWERBOUND;
        } else {
            tt_flag = TT_EXACT;
        }
        
        // Adjust mate scores for storage
        int store_score = alpha;
        if (store_score > MATE_SCORE - 100) {
            store_score += ply;
        } else if (store_score < -MATE_SCORE + 100) {
            store_score -= ply;
        }
        
        tt_store(board->zobristKey, depth, store_score, tt_flag, best_move);
    }
    
    return alpha;
}

// =============================================================================
// Iterative Deepening with Aspiration Windows
// =============================================================================

Move iterative_deepening_search(Board* board, SearchInfo* info) {
    printf("DEBUG: Starting search. White to move: %s\n", board->whiteToMove ? "true" : "false");
    printf("info string Time limits: soft=%ld ms, hard=%ld ms\n", info->softTimeLimit, info->hardTimeLimit);
    
    Move best_move = 0;
    int best_score = 0;
    
    info->nodesSearched = 0;
    info->lastIterationTime = 0;
    info->seldepth = 0;
    
    // Initialize TT for new search
    tt_new_search();
    
    // Track longest meaningful iteration time
    long max_meaningful_iteration_time = 0;
    
    for (int i = 0; i < MAX_PLY; i++) {
        info->pv_length[i] = 0;
    }
    
    // Aspiration window variables
    int alpha = -INT_MAX + 1;
    int beta = INT_MAX - 1;
    int prev_score = 0;
    
    for (int depth = 1; depth <= MAX_PLY; depth++) {
        long iteration_start = get_elapsed_time(info);
        int nodes_before = info->nodesSearched;
        
        info->bestMoveThisIteration = 0;
        info->seldepth = 0;
        
        int score;
        
        // Use aspiration windows after depth 4
        if (depth >= 5) {
            alpha = prev_score - ASPIRATION_WINDOW;
            beta = prev_score + ASPIRATION_WINDOW;
            
            while (true) {
                score = negamax(board, depth, alpha, beta, info, 0, true);
                
                if (info->stopSearch) break;
                
                // Failed low - widen alpha
                if (score <= alpha) {
                    alpha = -INT_MAX + 1;
                }
                // Failed high - widen beta  
                else if (score >= beta) {
                    beta = INT_MAX - 1;
                }
                // Within window
                else {
                    break;
                }
            }
        } else {
            score = negamax(board, depth, alpha, beta, info, 0, true);
        }
        
        long iteration_end = get_elapsed_time(info);
        info->lastIterationTime = iteration_end - iteration_start;
        int nodes_this_iteration = info->nodesSearched - nodes_before;
        
        // Update max meaningful iteration time
        if (info->lastIterationTime >= 10 && nodes_this_iteration >= 1000) {
            max_meaningful_iteration_time = info->lastIterationTime;
        }
        
        if (info->stopSearch) {
            printf("info string Search stopped at depth %d (hard limit reached)\n", depth);
            if (info->bestMoveThisIteration != 0) {
                best_move = info->bestMoveThisIteration;
                best_score = score;
            }
            break;
        }
        
        // Update best move from completed iteration
        if (info->bestMoveThisIteration != 0) {
            best_move = info->bestMoveThisIteration;
            best_score = score;
            prev_score = score;
        }
        
        // UCI output
        long time_ms = get_elapsed_time(info);
        int nps = time_ms > 0 ? (int)(info->nodesSearched * 1000L / time_ms) : 0;
        int hashfull = tt_hashfull();
        
        // Convert score to white's perspective for UCI output
        int uci_score = board->whiteToMove ? score : -score;
        
        printf("info depth %d seldepth %d score cp %d nodes %d nps %d time %ld hashfull %d pv",
               depth, info->seldepth, uci_score, info->nodesSearched, nps, time_ms, hashfull);
        
        for (int i = 0; i < info->pv_length[0]; i++) {
            if (info->pv_table[0][i] == 0) break;
            char move_str[10];
            moveToString(info->pv_table[0][i], move_str);
            printf(" %s", move_str);
        }
        printf("\n");
        fflush(stdout);
        
        // Stop if mate found
        if (abs(score) > MATE_SCORE - 100) {
            printf("info string Mate found, stopping search\n");
            break;
        }
        
        // Time management
        if (info->softTimeLimit > 0) {
            long remaining = info->softTimeLimit - time_ms;
            
            long time_for_estimate = max_meaningful_iteration_time > 0 ? 
                                     max_meaningful_iteration_time : info->lastIterationTime;
            long estimated_next = time_for_estimate * 3;
            
            if (time_ms >= info->softTimeLimit) {
                printf("info string Soft time limit reached after depth %d\n", depth);
                break;
            }
            
            bool enough_time_for_next = (estimated_next <= remaining);
            bool still_early = (time_ms < (info->softTimeLimit * 60) / 100);
            
            if (!enough_time_for_next && !still_early) {
                printf("info string Stopping before depth %d (estimated: %ld ms, remaining: %ld ms)\n",
                       depth + 1, estimated_next, remaining);
                break;
            }
        }
    }
    
    (void)best_score;
    printf("DEBUG: Best move: %u, Total time: %ld ms\n", best_move, get_elapsed_time(info));
    return best_move;
}

// Compatibility functions
int alpha_beta_search(Board* board, int depth, int alpha, int beta, bool maximizingPlayer, 
                      SearchInfo* info, int ply) {
    (void)maximizingPlayer;
    return negamax(board, depth, alpha, beta, info, ply, true);
}

int quiescence_search(Board* board, int alpha, int beta, bool maximizingPlayer, 
                      SearchInfo* info, int ply) {
    (void)maximizingPlayer;
    return quiescence(board, alpha, beta, info, ply);
}
