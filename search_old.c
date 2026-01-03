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

// Piece values for move ordering
static int get_piece_value(PieceTypeToken piece_type) {
    piece_type &= 0x7;
    switch (piece_type) {
        case PAWN_T:   return 100;
        case KNIGHT_T: return 320;
        case BISHOP_T: return 330;
        case ROOK_T:   return 500;
        case QUEEN_T:  return 900;
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

// Structure for move ordering
typedef struct {
    Move move;
    int score;
} ScoredMove;

static int compare_moves(const void* a, const void* b) {
    return ((ScoredMove*)b)->score - ((ScoredMove*)a)->score;
}

// Score moves for ordering
static void score_moves(Board* board, MoveList* moves, ScoredMove* scored, Move tt_move) {
    for (int i = 0; i < moves->count; i++) {
        Move m = moves->moves[i];
        scored[i].move = m;
        scored[i].score = 0;
        
        // TT move gets highest priority
        if (m == tt_move) {
            scored[i].score = 2000000;
            continue;
        }
        
        // Promotions
        if (MOVE_IS_PROMOTION(m)) {
            int promo = MOVE_PROMOTION(m);
            if (promo == PROMOTION_Q) {
                scored[i].score = 1900000;
            } else {
                scored[i].score = 1800000 + get_promotion_value(promo);
            }
            continue;
        }
        
        // Captures: MVV-LVA
        if (MOVE_IS_CAPTURE(m)) {
            bool isWhite = board->whiteToMove;
            bool isBlack = !isWhite;
            PieceTypeToken victim = getPieceTypeAtSquare(board, MOVE_TO(m), &isBlack);
            PieceTypeToken attacker = getPieceTypeAtSquare(board, MOVE_FROM(m), &isWhite);
            int victim_val = get_piece_value(victim);
            int attacker_val = get_piece_value(attacker);
            scored[i].score = 1000000 + victim_val * 10 - attacker_val;
        }
    }
    
    qsort(scored, moves->count, sizeof(ScoredMove), compare_moves);
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

// Quiescence search - only look at captures to avoid horizon effect
static int quiescence(Board* board, int alpha, int beta, SearchInfo* info, int ply) {
    info->nodesSearched++;
    
    // Check time periodically
    if ((info->nodesSearched & 2047) == 0 && check_time(info)) {
        return 0;
    }
    if (info->stopSearch) return 0;
    
    // Check if we're in check
    bool in_check = isKingAttacked(board, board->whiteToMove);
    
    // Generate all moves
    MoveList all_moves;
    generateMoves(board, &all_moves);
    
    // If no legal moves: checkmate or stalemate
    if (all_moves.count == 0) {
        if (in_check) {
            return -MATE_SCORE + ply; // Checkmate
        }
        return 0; // Stalemate
    }
    
    // If in check, we must search all moves (not just captures)
    if (in_check) {
        // Score and sort moves
        ScoredMove scored[256];
        score_moves(board, &all_moves, scored, 0);
        
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
        stand_pat = -stand_pat; // Flip for black's perspective
    }
    
    // Stand pat: if current eval is already good enough, we can stop
    if (stand_pat >= beta) {
        return beta;
    }
    if (stand_pat > alpha) {
        alpha = stand_pat;
    }
    
    // Score and sort moves
    ScoredMove scored[256];
    score_moves(board, &all_moves, scored, 0);
    
    for (int i = 0; i < all_moves.count; i++) {
        Move m = scored[i].move;
        
        // Only search captures and promotions in quiescence (when not in check)
        if (!MOVE_IS_CAPTURE(m) && !MOVE_IS_PROMOTION(m)) {
            continue;
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

// Negamax with alpha-beta pruning
static int negamax(Board* board, int depth, int alpha, int beta, SearchInfo* info, int ply) {
    info->nodesSearched++;
    info->pv_length[ply] = 0;
    
    int original_alpha = alpha;
    
    // Check time periodically
    if (ply > 0 && (info->nodesSearched & 2047) == 0 && check_time(info)) {
        return 0;
    }
    if (info->stopSearch) return 0;
    
    // Draw detection: repetition (only check at non-root nodes)
    if (ply > 0) {
        int rep_count = 0;
        for (int i = 0; i < board->historyIndex; i++) {
            if (board->history[i] == board->zobristKey) {
                rep_count++;
            }
        }
        if (rep_count >= 2) {
            return 0; // Draw by repetition
        }
        
        // Draw by 50-move rule
        if (board->halfMoveClock >= 100) {
            return 0;
        }
    }
    
    // TT Probe
    Move tt_move = 0;
    TTEntry* tt_entry = tt_probe(board->zobristKey);
    if (tt_entry != NULL) {
        tt_move = tt_entry->bestMove; // Use for move ordering even if depth insufficient
        
        if (tt_entry->depth >= depth && ply > 0) { // Don't return TT score at root
            int tt_score = tt_entry->score;
            
            // Adjust mate scores relative to current ply
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
    
    // Generate moves
    MoveList moves;
    generateMoves(board, &moves);
    
    // No legal moves: checkmate or stalemate
    if (moves.count == 0) {
        if (isKingAttacked(board, board->whiteToMove)) {
            return -MATE_SCORE + ply; // Checkmate (bad for us, so negative)
        }
        return 0; // Stalemate
    }
    
    // Score and sort moves (use TT move for ordering)
    ScoredMove scored[256];
    score_moves(board, &moves, scored, tt_move);
    
    Move best_move = 0;
    
    for (int i = 0; i < moves.count; i++) {
        Move m = scored[i].move;
        
        MoveUndoInfo undo;
        applyMove(board, m, &undo);
        int score = -negamax(board, depth - 1, -beta, -alpha, info, ply + 1);
        undoMove(board, m, &undo);
        
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
            break; // Beta cutoff
        }
    }
    
    // TT Store
    if (!info->stopSearch) {
        uint8_t tt_flag;
        if (alpha <= original_alpha) {
            tt_flag = TT_UPPERBOUND; // Failed low, score is at most alpha
        } else if (alpha >= beta) {
            tt_flag = TT_LOWERBOUND; // Failed high, score is at least beta
        } else {
            tt_flag = TT_EXACT;
        }
        
        // Adjust mate scores for storage (relative to root)
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

// Iterative deepening mit intelligentem Zeitmanagement
Move iterative_deepening_search(Board* board, SearchInfo* info) {
    printf("DEBUG: Starting search. White to move: %s\n", board->whiteToMove ? "true" : "false");
    printf("info string Time limits: soft=%ld ms, hard=%ld ms\n", info->softTimeLimit, info->hardTimeLimit);
    
    Move best_move = 0;
    int best_score = 0;
    
    info->nodesSearched = 0;
    info->lastIterationTime = 0;
    
    // Tracke die längste "echte" Iteration (nicht TT-Hit dominiert)
    long max_meaningful_iteration_time = 0;
    
    for (int i = 0; i < MAX_PLY; i++) {
        info->pv_length[i] = 0;
    }
    
    for (int depth = 1; depth <= MAX_PLY; depth++) {
        long iteration_start = get_elapsed_time(info);
        int nodes_before = info->nodesSearched;
        
        info->bestMoveThisIteration = 0;
        
        int score = negamax(board, depth, -INT_MAX + 1, INT_MAX - 1, info, 0);
        
        long iteration_end = get_elapsed_time(info);
        info->lastIterationTime = iteration_end - iteration_start;
        int nodes_this_iteration = info->nodesSearched - nodes_before;
        
        // Aktualisiere die maximale "echte" Iterationszeit
        // Ignoriere sehr schnelle Iterationen (< 10ms oder < 1000 Knoten) - das sind TT-Hits
        if (info->lastIterationTime >= 10 && nodes_this_iteration >= 1000) {
            max_meaningful_iteration_time = info->lastIterationTime;
        }
        
        // Wenn die Suche abgebrochen wurde, aber wir einen Move haben, benutze den
        if (info->stopSearch) {
            printf("info string Search stopped at depth %d (hard limit reached)\n", depth);
            // Benutze den besten Zug dieser Iteration nur, wenn wir einen haben
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
        }
        
        // UCI output
        long time_ms = get_elapsed_time(info);
        int nps = time_ms > 0 ? (int)(info->nodesSearched * 1000L / time_ms) : 0;
        
        // Convert score to white's perspective for UCI output
        int uci_score = board->whiteToMove ? score : -score;
        
        printf("info depth %d score cp %d nodes %d nps %d time %ld pv",
               depth, uci_score, info->nodesSearched, nps, time_ms);
        
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
        
        // Entscheide, ob wir die nächste Tiefe starten sollen
        if (info->softTimeLimit > 0) {
            long remaining = info->softTimeLimit - time_ms;
            
            // Schätze Zeit für nächste Tiefe basierend auf der längsten echten Iteration
            // Branching-Faktor ~2.5 für gut sortierte Züge mit Alpha-Beta
            long time_for_estimate = max_meaningful_iteration_time > 0 ? 
                                     max_meaningful_iteration_time : info->lastIterationTime;
            long estimated_next = time_for_estimate * 3; // Faktor 3
            
            // Stoppe wenn:
            // 1. Wir schon über dem Soft-Limit sind, ODER
            // 2. Wir mindestens 60% der Zeit genutzt haben UND die nächste Tiefe würde das Hard-Limit überschreiten
            if (time_ms >= info->softTimeLimit) {
                printf("info string Soft time limit reached after depth %d\n", depth);
                break;
            }
            
            // Sei mutiger: Starte die nächste Tiefe, wenn wir noch unter 60% der Soft-Zeit sind
            // oder wenn genug Zeit für die nächste Tiefe übrig ist
            bool enough_time_for_next = (estimated_next <= remaining);
            bool still_early = (time_ms < (info->softTimeLimit * 60) / 100);
            
            if (!enough_time_for_next && !still_early) {
                printf("info string Stopping before depth %d (estimated: %ld ms, remaining: %ld ms, used: %ld/%ld ms)\n",
                       depth + 1, estimated_next, remaining, time_ms, info->softTimeLimit);
                break;
            }
        }
    }
    
    (void)best_score; // Suppress unused warning
    printf("DEBUG: Best move: %u, Total time: %ld ms\n", best_move, get_elapsed_time(info));
    return best_move;
}

// Keep these for compatibility with header
int alpha_beta_search(Board* board, int depth, int alpha, int beta, bool maximizingPlayer, SearchInfo* info, int ply) {
    (void)maximizingPlayer; // Unused in negamax
    return negamax(board, depth, alpha, beta, info, ply);
}

int quiescence_search(Board* board, int alpha, int beta, bool maximizingPlayer, SearchInfo* info, int ply) {
    (void)maximizingPlayer; // Unused in negamax
    return quiescence(board, alpha, beta, info, ply);
}

