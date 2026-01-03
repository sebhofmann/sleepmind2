#include "search.h"
#include "evaluation.h"
#include "move_generator.h"
#include "board_modifiers.h"
#include "tt.h"
#include "board_io.h"
#include "board.h"
#include "move.h"
#include "bitboard_utils.h"
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

// =============================================================================
// Constants
// =============================================================================

// Late Move Reduction parameters (tuned for NNUE)
static const int LMR_FULL_DEPTH_MOVES = 4;  // Start LMR earlier with reliable eval
static const int LMR_REDUCTION_LIMIT = 3;   // Lower minimum depth for LMR

// Null Move Pruning parameters
static const int NULL_MOVE_REDUCTION = 3;   // Standard R=3 with NNUE
static const int NULL_MOVE_MIN_DEPTH = 3;   // Can be more aggressive

// Futility pruning margins (enabled with NNUE)
static const int FUTILITY_MARGIN = 150;           // Depth 1
static const int EXTENDED_FUTILITY_MARGIN = 300;  // Depth 2
static const int FUTILITY_MARGIN_DEPTH3 = 450;    // Depth 3

// Reverse Futility Pruning (Static Null Move Pruning) margins
static const int RFP_MARGIN = 120;  // Per depth margin
static const int RFP_MAX_DEPTH = 6; // Maximum depth for RFP

// Delta pruning margin for quiescence
static const int DELTA_MARGIN = 200;  // Tighter with reliable eval

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

// SEE piece values (simple array for fast lookup)
static const int SEE_VALUES[7] = {
    0,      // NONE
    100,    // PAWN
    320,    // KNIGHT
    330,    // BISHOP
    500,    // ROOK
    900,    // QUEEN
    20000   // KING
};

// External attack tables from move_generator.c
extern Bitboard getRookAttacks(int square, Bitboard occupancy);
extern Bitboard getBishopAttacks(int square, Bitboard occupancy);

// Precomputed attack tables (declared in move_generator.c, we need access)
// We'll compute attackers dynamically

// Get all attackers to a square
static Bitboard get_all_attackers(const Board* board, int square, Bitboard occupied) {
    Bitboard attackers = 0ULL;
    
    // Pawn attacks (reverse lookup - who can attack this square with a pawn?)
    // White pawns attack diagonally upward, so square must be attacked from below
    Bitboard white_pawn_attackers = 0ULL;
    if (square >= 9 && (square % 8) > 0) white_pawn_attackers |= (1ULL << (square - 9));
    if (square >= 7 && (square % 8) < 7) white_pawn_attackers |= (1ULL << (square - 7));
    attackers |= white_pawn_attackers & board->whitePawns;
    
    // Black pawns attack diagonally downward
    Bitboard black_pawn_attackers = 0ULL;
    if (square <= 54 && (square % 8) < 7) black_pawn_attackers |= (1ULL << (square + 9));
    if (square <= 56 && (square % 8) > 0) black_pawn_attackers |= (1ULL << (square + 7));
    attackers |= black_pawn_attackers & board->blackPawns;
    
    // Knight attacks (symmetric)
    static const int knight_offsets[8] = {-17, -15, -10, -6, 6, 10, 15, 17};
    Bitboard knights = board->whiteKnights | board->blackKnights;
    for (int i = 0; i < 8; i++) {
        int from = square + knight_offsets[i];
        if (from >= 0 && from < 64) {
            int from_file = from % 8;
            int to_file = square % 8;
            if (abs(from_file - to_file) <= 2) {
                attackers |= (1ULL << from) & knights;
            }
        }
    }
    
    // King attacks (symmetric)
    static const int king_offsets[8] = {-9, -8, -7, -1, 1, 7, 8, 9};
    Bitboard kings = board->whiteKings | board->blackKings;
    for (int i = 0; i < 8; i++) {
        int from = square + king_offsets[i];
        if (from >= 0 && from < 64) {
            int from_file = from % 8;
            int to_file = square % 8;
            if (abs(from_file - to_file) <= 1) {
                attackers |= (1ULL << from) & kings;
            }
        }
    }
    
    // Sliding pieces
    Bitboard rook_attacks = getRookAttacks(square, occupied);
    attackers |= rook_attacks & (board->whiteRooks | board->blackRooks | 
                                  board->whiteQueens | board->blackQueens);
    
    Bitboard bishop_attacks = getBishopAttacks(square, occupied);
    attackers |= bishop_attacks & (board->whiteBishops | board->blackBishops | 
                                    board->whiteQueens | board->blackQueens);
    
    return attackers;
}

// Get the least valuable attacker from a set of attackers
static int get_smallest_attacker(const Board* board, Bitboard attackers, bool white, int* piece_square) {
    Bitboard our_pieces;
    
    // Check in order: Pawns, Knights, Bishops, Rooks, Queens, King
    our_pieces = attackers & (white ? board->whitePawns : board->blackPawns);
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[1]; // PAWN
    }
    
    our_pieces = attackers & (white ? board->whiteKnights : board->blackKnights);
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[2]; // KNIGHT
    }
    
    our_pieces = attackers & (white ? board->whiteBishops : board->blackBishops);
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[3]; // BISHOP
    }
    
    our_pieces = attackers & (white ? board->whiteRooks : board->blackRooks);
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[4]; // ROOK
    }
    
    our_pieces = attackers & (white ? board->whiteQueens : board->blackQueens);
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[5]; // QUEEN
    }
    
    our_pieces = attackers & (white ? board->whiteKings : board->blackKings);
    if (our_pieces) {
        *piece_square = BIT_SCAN_FORWARD(our_pieces);
        return SEE_VALUES[6]; // KING
    }
    
    return 0; // No attacker
}

// Static Exchange Evaluation
// Returns the expected material gain/loss from a capture sequence
static int see(const Board* board, Move move) {
    int from = MOVE_FROM(move);
    int to = MOVE_TO(move);
    
    // Get initial attacker value
    bool attacker_white = board->whiteToMove;
    bool attacker_side = attacker_white;
    PieceTypeToken attacker_type = getPieceTypeAtSquare(board, from, &attacker_white);
    int attacker_value = get_piece_value(attacker_type);
    
    // Get victim value
    bool victim_white = !board->whiteToMove;
    PieceTypeToken victim_type = getPieceTypeAtSquare(board, to, &victim_white);
    int victim_value = get_piece_value(victim_type);
    
    // Handle en passant
    if (MOVE_IS_EN_PASSANT(move)) {
        victim_value = SEE_VALUES[1]; // Pawn
    }
    
    // If capturing nothing (shouldn't happen for captures), return 0
    if (victim_value == 0 && !MOVE_IS_EN_PASSANT(move)) {
        return 0;
    }
    
    // Gain array to track material balance at each step
    int gain[32];
    int depth = 0;
    
    // Initial capture
    gain[depth] = victim_value;
    
    // Simulate the capture
    Bitboard occupied = board->whitePawns | board->whiteKnights | board->whiteBishops |
                        board->whiteRooks | board->whiteQueens | board->whiteKings |
                        board->blackPawns | board->blackKnights | board->blackBishops |
                        board->blackRooks | board->blackQueens | board->blackKings;
    
    // Remove the initial attacker from occupancy
    occupied &= ~(1ULL << from);
    
    // Get all attackers to the target square
    Bitboard attackers = get_all_attackers(board, to, occupied);
    
    // Remove the initial attacker
    attackers &= ~(1ULL << from);
    
    // Current piece on the target square (the one that just captured)
    int current_piece_value = attacker_value;
    
    // Alternate sides
    bool side_to_move = !attacker_side;
    
    while (attackers) {
        depth++;
        if (depth >= 32) break;
        
        // Find smallest attacker for current side
        int piece_square;
        int next_attacker_value = get_smallest_attacker(board, attackers, side_to_move, &piece_square);
        
        if (next_attacker_value == 0) break;
        
        // Calculate gain: capture the current piece, but we might lose our attacker
        gain[depth] = current_piece_value - gain[depth - 1];
        
        // Prune if even the best case (opponent doesn't recapture) is bad
        if (-gain[depth - 1] > 0 && -gain[depth] > 0) break;
        
        // Update occupied (remove the attacker)
        occupied &= ~(1ULL << piece_square);
        attackers &= ~(1ULL << piece_square);
        
        // Update attackers (x-ray through the removed piece)
        attackers |= get_all_attackers(board, to, occupied) & occupied;
        
        // Update current piece value
        current_piece_value = next_attacker_value;
        
        // Switch sides
        side_to_move = !side_to_move;
    }
    
    // Negamax the gain array
    while (depth > 0) {
        gain[depth - 1] = -(-gain[depth - 1] > gain[depth] ? -gain[depth - 1] : gain[depth]);
        depth--;
    }
    
    return gain[0];
}

// Quick SEE check: is the capture likely good (>= threshold)?
static bool see_ge(const Board* board, Move move, int threshold) {
    return see(board, move) >= threshold;
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
        
        // 2. Captures - use SEE for accurate ordering
        if (MOVE_IS_CAPTURE(m)) {
            int see_value = see(board, m);
            
            // Also compute MVV-LVA as tiebreaker
            bool isWhite = board->whiteToMove;
            bool isBlack = !isWhite;
            PieceTypeToken victim = getPieceTypeAtSquare(board, to, &isBlack);
            PieceTypeToken attacker = getPieceTypeAtSquare(board, from, &isWhite);
            int victim_val = get_piece_value(victim);
            int attacker_val = get_piece_value(attacker);
            int mvv_lva = victim_val * 10 - attacker_val;
            
            if (see_value >= 0) {
                // Good/equal captures: high priority, sorted by SEE then MVV-LVA
                scored[i].score = 8000000 + see_value * 100 + mvv_lva;
            } else {
                // Bad captures: low priority but still above quiet moves with bad history
                scored[i].score = 2000000 + see_value * 100 + mvv_lva;
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
            // Use SEE for accurate capture ordering in qsearch
            int see_value = see(board, m);
            
            // MVV-LVA as tiebreaker
            bool isWhite = board->whiteToMove;
            bool isBlack = !isWhite;
            PieceTypeToken victim = getPieceTypeAtSquare(board, MOVE_TO(m), &isBlack);
            PieceTypeToken attacker = getPieceTypeAtSquare(board, MOVE_FROM(m), &isWhite);
            int mvv_lva = get_piece_value(victim) * 10 - get_piece_value(attacker);
            
            // Good captures first, then by MVV-LVA
            if (see_value >= 0) {
                scored[i].score = 1000000 + see_value * 100 + mvv_lva;
            } else {
                scored[i].score = see_value * 100 + mvv_lva;  // Bad captures at the end
            }
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

// Update history heuristic with gravity (bonus for good moves, malus for bad)
static void update_history(SearchInfo* info, Board* board, Move m, int depth) {
    int side = board->whiteToMove ? 0 : 1;
    int from = MOVE_FROM(m);
    int to = MOVE_TO(m);
    
    // Bonus proportional to depth^2
    int bonus = depth * depth;
    
    // Gravity formula to prevent runaway values
    int current = info->history[side][from][to];
    int max_history = 16384;
    info->history[side][from][to] += bonus - (current * abs(bonus) / max_history);
}

// Penalize quiet moves that didn't cause a cutoff
static void update_history_malus(SearchInfo* info, Board* board, Move m, int depth) {
    int side = board->whiteToMove ? 0 : 1;
    int from = MOVE_FROM(m);
    int to = MOVE_TO(m);
    
    int malus = depth * depth;
    int current = info->history[side][from][to];
    int max_history = 16384;
    info->history[side][from][to] -= malus - (current * abs(malus) / max_history);
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
    // Reverse Futility Pruning (Static Null Move Pruning)
    // If static eval is way above beta, we can assume a beta cutoff
    // ==========================================================================
    if (!is_pv && !in_check && depth <= RFP_MAX_DEPTH &&
        abs(beta) < MATE_SCORE - 100) {
        int rfp_margin = RFP_MARGIN * depth;
        if (static_eval - rfp_margin >= beta) {
            return static_eval - rfp_margin;
        }
    }
    
    // ==========================================================================
    // Futility Pruning (now enabled with NNUE)
    // ==========================================================================
    bool futility_pruning = false;
    int futility_margin = 0;
    if (!is_pv && !in_check && depth <= 3 &&
        abs(alpha) < MATE_SCORE - 100 && abs(beta) < MATE_SCORE - 100) {
        if (depth == 1) {
            futility_margin = FUTILITY_MARGIN;
        } else if (depth == 2) {
            futility_margin = EXTENDED_FUTILITY_MARGIN;
        } else {
            futility_margin = FUTILITY_MARGIN_DEPTH3;
        }
        if (static_eval + futility_margin <= alpha) {
            futility_pruning = true;
        }
    }
    
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
            // Late Move Reductions (tuned for NNUE)
            int reduction = 0;
            
            if (!in_check && !is_tactical && depth >= LMR_REDUCTION_LIMIT && 
                moves_searched >= LMR_FULL_DEPTH_MOVES) {
                // Log-based reduction formula: more aggressive with reliable eval
                // Base: ln(depth) * ln(moves_searched) / 2
                reduction = 1;
                
                // Depth-based increase
                if (depth >= 6) reduction++;
                if (depth >= 10) reduction++;
                
                // Move count based increase
                if (moves_searched >= 8) reduction++;
                if (moves_searched >= 16) reduction++;
                if (moves_searched >= 32) reduction++;
                
                // Reduce less for PV nodes
                if (is_pv) {
                    reduction--;
                }
                
                // Reduce less for killer moves
                if (ply < MAX_PLY && (m == info->killers[ply][0] || m == info->killers[ply][1])) {
                    reduction--;
                }
                
                // Reduce more for moves with bad history
                int side = board->whiteToMove ? 0 : 1;
                int from = MOVE_FROM(m);
                int to = MOVE_TO(m);
                if (info->history[side][from][to] < 0) {
                    reduction++;
                } else if (info->history[side][from][to] > 5000) {
                    reduction--;  // Good history = less reduction
                }
                
                // Clamp reduction
                if (reduction < 0) reduction = 0;
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
                
                // Apply malus to all previously searched quiet moves
                for (int j = 0; j < i; j++) {
                    Move prev = scored[j].move;
                    if (!MOVE_IS_CAPTURE(prev) && !MOVE_IS_PROMOTION(prev)) {
                        update_history_malus(info, board, prev, depth);
                    }
                }
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
