#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <getopt.h>
#include <signal.h>

#include "board.h"
#include "board_io.h"
#include "move.h"
#include "move_generator.h"
#include "board_modifiers.h"
#include "search.h"
#include "bitboard_utils.h"
#include "nnue.h"
#include "evaluation.h"
#include "training_data.h"
#include "zobrist.h"
#include "tt.h"

// =============================================================================
// Configuration
// =============================================================================

typedef struct {
    char output_file[256];
    int random_moves;           // Number of random moves at start of game
    int random_probability;     // Probability (0-100) for each random move
    int draw_threshold;         // Moves without progress before draw
    int max_game_moves;         // Maximum moves per game
    int num_games;              // Number of games to play
    int search_depth;           // Search depth for moves
    int search_time_ms;         // Search time in milliseconds (0 = use depth)
    uint64_t search_nodes;      // Search node limit (0 = use depth or time)
    int verbose;                // Verbosity level
    int eval_threshold;         // Max eval (in pawns) after random moves, 0 = disabled
    int adjudicate_threshold;   // Adjudicate game as won/lost if eval exceeds this (in pawns), 0 = disabled
    bool filter_tactics;        // Filter out tactical positions (checks, captures)
} TrainingConfig;

static TrainingConfig config = {
    .output_file = "training_data.txt",
    .random_moves = 12,
    .random_probability = 100,  // 100% random for first N moves
    .draw_threshold = 100,      // 50-move rule
    .max_game_moves = 500,
    .num_games = 100,
    .search_depth = 8,
    .search_time_ms = 0,
    .search_nodes = 0,          // 0 = use depth or time
    .verbose = 1,
    .eval_threshold = 1,        // Default: discard games with eval > +/-100cp after random moves
    .adjudicate_threshold = 10, // Default: adjudicate at +/-1000cp
    .filter_tactics = true      // Default: filter tactical positions
};

// Global flag for graceful shutdown
static volatile bool should_stop = false;
static int games_completed = 0;

// Statistics tracking
static int total_positions = 0;
static int filtered_positions = 0;
static int games_discarded = 0;
static time_t start_time = 0;
static time_t last_status_time = 0;

static void print_status(void) {
    time_t now = time(NULL);
    double elapsed = difftime(now, start_time);
    if (elapsed > 0) {
        double pos_per_sec = total_positions / elapsed;
        printf("[Status: %d games, %d discarded, %d positions (%d filtered), %.1f pos/sec, %.0fs elapsed]\n", 
               games_completed, games_discarded, total_positions, filtered_positions, pos_per_sec, elapsed);
        fflush(stdout);
    }
}

static void check_status_output(void) {
    time_t now = time(NULL);
    if (difftime(now, last_status_time) >= 5.0) {
        print_status();
        last_status_time = now;
    }
}

// =============================================================================
// Signal Handler
// =============================================================================

void signal_handler(int sig) {
    (void)sig;
    printf("\nReceived signal, finishing current game and shutting down...\n");
    should_stop = true;
}

// =============================================================================
// Game Result Detection
// =============================================================================

typedef enum {
    GAME_ONGOING,
    GAME_WHITE_WINS,
    GAME_BLACK_WINS,
    GAME_DRAW
} GameResult;

// Check for insufficient material
static bool is_insufficient_material(const Board* board) {
    // Count pieces
    int white_pawns = __builtin_popcountll(board->whitePawns);
    int black_pawns = __builtin_popcountll(board->blackPawns);
    int white_rooks = __builtin_popcountll(board->whiteRooks);
    int black_rooks = __builtin_popcountll(board->blackRooks);
    int white_queens = __builtin_popcountll(board->whiteQueens);
    int black_queens = __builtin_popcountll(board->blackQueens);
    int white_knights = __builtin_popcountll(board->whiteKnights);
    int black_knights = __builtin_popcountll(board->blackKnights);
    int white_bishops = __builtin_popcountll(board->whiteBishops);
    int black_bishops = __builtin_popcountll(board->blackBishops);
    
    // If there are pawns, rooks, or queens, sufficient material
    if (white_pawns || black_pawns || white_rooks || black_rooks || 
        white_queens || black_queens) {
        return false;
    }
    
    int white_minor = white_knights + white_bishops;
    int black_minor = black_knights + black_bishops;
    
    // King vs King
    if (white_minor == 0 && black_minor == 0) return true;
    
    // King + minor vs King
    if ((white_minor == 1 && black_minor == 0) ||
        (white_minor == 0 && black_minor == 1)) return true;
    
    // King + Bishop vs King + Bishop (same color bishops)
    if (white_knights == 0 && black_knights == 0 &&
        white_bishops == 1 && black_bishops == 1) {
        // Check if bishops are on same color
        int white_bishop_sq = __builtin_ctzll(board->whiteBishops);
        int black_bishop_sq = __builtin_ctzll(board->blackBishops);
        int white_color = (white_bishop_sq / 8 + white_bishop_sq % 8) % 2;
        int black_color = (black_bishop_sq / 8 + black_bishop_sq % 8) % 2;
        if (white_color == black_color) return true;
    }
    
    return false;
}

// Check for threefold repetition (simplified - just check last few positions)
static uint64_t position_history[1024];
static int position_count = 0;

static void reset_position_history(void) {
    position_count = 0;
}

static void record_position(uint64_t hash) {
    if (position_count < 1024) {
        position_history[position_count++] = hash;
    }
}

static int count_position_repetitions(uint64_t hash) {
    int count = 0;
    for (int i = 0; i < position_count; i++) {
        if (position_history[i] == hash) count++;
    }
    return count;
}

static GameResult check_game_result(Board* board, int half_move_clock, MoveList* moves) {
    generateMoves(board, moves);
    
    // Count legal moves (pseudo-legal generator requires legality check)
    int legal_move_count = 0;
    for (int i = 0; i < moves->count; i++) {
        MoveUndoInfo undo;
        applyMove(board, moves->moves[i], &undo, NULL, NULL);
        if (!isKingAttacked(board, !board->whiteToMove)) {
            legal_move_count++;
        }
        undoMove(board, moves->moves[i], &undo, NULL, NULL);
    }
    
    // Check for checkmate or stalemate
    if (legal_move_count == 0) {
        if (isKingAttacked(board, board->whiteToMove)) {
            // Checkmate
            return board->whiteToMove ? GAME_BLACK_WINS : GAME_WHITE_WINS;
        } else {
            // Stalemate
            return GAME_DRAW;
        }
    }
    
    // 50-move rule
    if (half_move_clock >= config.draw_threshold * 2) {
        return GAME_DRAW;
    }
    
    // Insufficient material
    if (is_insufficient_material(board)) {
        return GAME_DRAW;
    }
    
    // Threefold repetition
    if (count_position_repetitions(board->zobristKey) >= 3) {
        return GAME_DRAW;
    }
    
    return GAME_ONGOING;
}

// =============================================================================
// Self-Play Game
// =============================================================================

// Returns true if game was valid, false if discarded (eval threshold exceeded)
static bool play_game(int game_num, NNUENetwork* nnue_network) {
    Board board = parseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    NNUEAccumulator nnue_accumulator = {0};
    nnue_reset_accumulator(&board, &nnue_accumulator, nnue_network);
    
    MoveList moves;
    MoveUndoInfo undo_info;
    
    int ply = 0;
    int half_move_clock = 0;
    GameResult result = GAME_ONGOING;
    bool checked_threshold = false;  // Track if we've checked eval after random moves
    
    // Reset position history and training data for this game
    reset_position_history();
    training_data_count = 0;  // Just reset count, keep path from set_training_data_path
    enable_training(true);    // Make sure training is enabled
    
    if (config.verbose >= 1) {
        printf("Game %d/%d starting...\n", game_num, config.num_games);
    }
    
    while (result == GAME_ONGOING && ply < config.max_game_moves && !should_stop) {
        // Record position for repetition detection
        record_position(board.zobristKey);
        
        // Generate moves
        generateMoves(&board, &moves);
        if (moves.count == 0) break;
        
        Move best_move = 0;
        int best_score = 0;
        bool is_random_move = false;
        
        // Random opening moves
        if (ply < config.random_moves) {
            int roll = rand() % 100;
            if (roll < config.random_probability) {
                // Build list of legal moves (pseudo-legal generator requires check)
                Move legal_moves[256];
                int legal_count = 0;
                for (int i = 0; i < moves.count; i++) {
                    MoveUndoInfo test_undo;
                    applyMove(&board, moves.moves[i], &test_undo, NULL, NULL);
                    if (!isKingAttacked(&board, !board.whiteToMove)) {
                        legal_moves[legal_count++] = moves.moves[i];
                    }
                    undoMove(&board, moves.moves[i], &test_undo, NULL, NULL);
                }
                
                if (legal_count > 0) {
                    int idx = rand() % legal_count;
                    best_move = legal_moves[idx];
                    is_random_move = true;
                    
                    if (config.verbose >= 2) {
                        char move_str[6];
                        moveToString(best_move, move_str);
                        printf("  Ply %d: random move %s\n", ply, move_str);
                    }
                }
            }
        }
        
        // Search for best move if not random
        if (!is_random_move) {
            SearchInfo search_info;
            search_info.startTime = clock();
            search_params_init(&search_info.params);  // Initialize search parameters
            
            if (config.search_nodes > 0) {
                // Node-based search: no time or depth limit
                search_info.softTimeLimit = 0;
                search_info.hardTimeLimit = 0;
                search_info.depthLimit = 0;
                search_info.nodeLimit = config.search_nodes;
            } else if (config.search_time_ms > 0) {
                search_info.softTimeLimit = config.search_time_ms;
                search_info.hardTimeLimit = config.search_time_ms;
                search_info.depthLimit = 0;
                search_info.nodeLimit = 0;
            } else {
                search_info.softTimeLimit = 0;
                search_info.hardTimeLimit = 0;
                search_info.depthLimit = config.search_depth;
                search_info.nodeLimit = 0;
            }
            
            search_info.stopSearch = false;
            search_info.lastIterationTime = 0;
            search_info.nnue_acc = &nnue_accumulator;
            search_info.nnue_net = nnue_network;
            search_info.nodesSearched = 0;
            search_info.bestMoveThisIteration = 0;
            search_info.bestScoreThisIteration = 0;
            search_info.seldepth = 0;
            clear_search_history(&search_info);
            
            best_move = iterative_deepening_search(&board, &search_info);
            best_score = search_info.bestScoreThisIteration;
            
            if (config.verbose >= 2) {
                char move_str[6];
                moveToString(best_move, move_str);
                printf("  Ply %d: search move %s (score: %d)\n", ply, move_str, best_score);
            }
            
            // Check eval threshold after random moves (only once)
            if (!checked_threshold && config.eval_threshold > 0 && ply >= config.random_moves) {
                checked_threshold = true;
                int threshold_cp = config.eval_threshold * 100;  // Convert pawns to centipawns
                if (best_score > threshold_cp || best_score < -threshold_cp) {
                    if (config.verbose >= 1) {
                        printf("Game %d discarded: eval %d cp exceeds threshold +/-%d cp\n", 
                               game_num, best_score, threshold_cp);
                    }
                    return false;  // Discard this game
                }
            }
            
            // Check adjudication threshold - stop logging and end game if position is hopeless
            if (config.adjudicate_threshold > 0) {
                int adj_threshold_cp = config.adjudicate_threshold * 100;
                if (best_score > adj_threshold_cp || best_score < -adj_threshold_cp) {
                    // Adjudicate the game
                    if (best_score > adj_threshold_cp) {
                        result = board.whiteToMove ? GAME_WHITE_WINS : GAME_BLACK_WINS;
                    } else {
                        result = board.whiteToMove ? GAME_BLACK_WINS : GAME_WHITE_WINS;
                    }
                    if (config.verbose >= 2) {
                        printf("  Ply %d: adjudicated (eval %d cp)\n", ply, best_score);
                    }
                    break;  // Exit the game loop
                }
            }
            
            // Filter tactical positions if enabled
            bool should_record = true;
            if (config.filter_tactics) {
                // Check if side to move is in check
                bool in_check = isKingAttacked(&board, board.whiteToMove);
                // Check if best move is a capture
                bool is_capture_move = MOVE_IS_CAPTURE(best_move);
                
                if (in_check || is_capture_move) {
                    should_record = false;
                    filtered_positions++;
                    if (config.verbose >= 2) {
                        printf("  Ply %d: filtered (%s)\n", ply, 
                               in_check ? "in check" : "capture move");
                    }
                }
            }
            
            // Record training data (only for non-random, non-tactical moves)
            if (should_record) {
                add_training_entry(&board, best_score, ply);
            }
        }
        
        if (best_move == 0) {
            if (config.verbose >= 1) {
                printf("  No move found at ply %d!\n", ply);
            }
            break;
        }
        
        // Check if this is a pawn move or capture for 50-move rule
        bool is_pawn_move = false;
        bool is_capture = MOVE_IS_CAPTURE(best_move);
        Square from_sq = MOVE_FROM(best_move);
        Bitboard from_bb = 1ULL << from_sq;
        if ((board.whitePawns & from_bb) || (board.blackPawns & from_bb)) {
            is_pawn_move = true;
        }
        
        // Apply the move
        applyMove(&board, best_move, &undo_info, &nnue_accumulator, nnue_network);
        ply++;
        
        // Update half-move clock
        if (is_pawn_move || is_capture) {
            half_move_clock = 0;
        } else {
            half_move_clock++;
        }
        
        // Check for game end
        result = check_game_result(&board, half_move_clock, &moves);
    }
    
    // Determine final result
    int result_value = 0;  // 1 = white wins, 0 = draw, -1 = black wins
    const char* result_str = "draw";
    
    if (result == GAME_WHITE_WINS) {
        result_value = 1;
        result_str = "white wins";
    } else if (result == GAME_BLACK_WINS) {
        result_value = -1;
        result_str = "black wins";
    } else if (result == GAME_DRAW || ply >= config.max_game_moves) {
        result_value = 0;
        result_str = "draw";
    }
    
    // Write training data
    int entries_written = training_data_count;
    total_positions += entries_written;
    write_training_data(result_value);
    
    if (config.verbose >= 1) {
        printf("Game %d finished: %s after %d plies (%d training entries)\n", 
               game_num, result_str, ply, entries_written);
    }
    
    games_completed++;
    return true;  // Game was valid
}

// =============================================================================
// Usage and Argument Parsing
// =============================================================================

static void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("\nOptions:\n");
    printf("  -o, --output FILE       Output file for training data (default: training_data.txt)\n");
    printf("  -n, --num-games N       Number of games to play (default: 100)\n");
    printf("  -r, --random-moves N    Number of random moves at start (default: 12)\n");
    printf("  -p, --random-prob N     Probability (0-100) for random moves (default: 100)\n");
    printf("  -d, --depth N           Search depth (default: 8)\n");
    printf("  -t, --time MS           Search time in milliseconds (overrides depth)\n");
    printf("  -N, --nodes N           Search node limit (overrides depth and time)\n");
    printf("  --draw-threshold N      Moves without progress for draw (default: 100)\n");
    printf("  --max-moves N           Maximum moves per game (default: 500)\n");
    printf("  -e, --eval-threshold N  Max eval in pawns after random moves, discard if exceeded (0=off)\n");
    printf("  -a, --adjudicate N      Adjudicate game if eval exceeds N pawns (default: 10, 0=off)\n");
    printf("  -f, --filter-tactics B  Filter tactical positions: checks/captures (default: 1, 0=off)\n");
    printf("  -v, --verbose LEVEL     Verbosity level 0-2 (default: 1)\n");
    printf("  -h, --help              Show this help message\n");
    printf("\nExample:\n");
    printf("  %s -o data.txt -n 1000 -r 8 -d 6 -e 4 -a 10 -f 1\n", program_name);
    printf("  %s -o data.txt -n 1000 -r 8 -N 10000 -e 4 -a 10 -f 1  # Node-based search\n", program_name);
}

static void parse_arguments(int argc, char* argv[]) {
    static struct option long_options[] = {
        {"output",         required_argument, 0, 'o'},
        {"num-games",      required_argument, 0, 'n'},
        {"random-moves",   required_argument, 0, 'r'},
        {"random-prob",    required_argument, 0, 'p'},
        {"depth",          required_argument, 0, 'd'},
        {"time",           required_argument, 0, 't'},
        {"nodes",          required_argument, 0, 'N'},
        {"draw-threshold", required_argument, 0, 'D'},
        {"max-moves",      required_argument, 0, 'M'},
        {"eval-threshold", required_argument, 0, 'e'},
        {"adjudicate",     required_argument, 0, 'a'},
        {"filter-tactics", required_argument, 0, 'f'},
        {"verbose",        required_argument, 0, 'v'},
        {"help",           no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int c;
    while ((c = getopt_long(argc, argv, "o:n:r:p:d:t:N:e:a:f:v:h", long_options, NULL)) != -1) {
        switch (c) {
            case 'o':
                strncpy(config.output_file, optarg, sizeof(config.output_file) - 1);
                break;
            case 'n':
                config.num_games = atoi(optarg);
                break;
            case 'r':
                config.random_moves = atoi(optarg);
                break;
            case 'p':
                config.random_probability = atoi(optarg);
                if (config.random_probability < 0) config.random_probability = 0;
                if (config.random_probability > 100) config.random_probability = 100;
                break;
            case 'd':
                config.search_depth = atoi(optarg);
                break;
            case 't':
                config.search_time_ms = atoi(optarg);
                break;
            case 'N':
                config.search_nodes = strtoull(optarg, NULL, 10);
                break;
            case 'D':
                config.draw_threshold = atoi(optarg);
                break;
            case 'M':
                config.max_game_moves = atoi(optarg);
                break;
            case 'e':
                config.eval_threshold = atoi(optarg);
                break;
            case 'a':
                config.adjudicate_threshold = atoi(optarg);
                break;
            case 'f':
                config.filter_tactics = atoi(optarg) != 0;
                break;
            case 'v':
                config.verbose = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                exit(1);
        }
    }
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    // Parse command line arguments
    parse_arguments(argc, argv);
    
    // Install signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    
    // Initialize random seed
    srand((unsigned int)time(NULL));
    
    // Initialize engine components
    init_zobrist_keys();
    init_tt(256);  // 256 MB transposition table
    initMoveGenerator();
    
    // Disable search output for training (no "info depth" and "DEBUG" spam)
    set_search_silent(true);
    
    // Allocate and load NNUE network
    NNUENetwork* nnue_network = (NNUENetwork*)calloc(1, sizeof(NNUENetwork));
    if (!nnue_network) {
        fprintf(stderr, "Error: Failed to allocate memory for NNUE network\n");
        return 1;
    }
    eval_init("quantised.bin", nnue_network);
    
    if (!nnue_network->loaded) {
        printf("Warning: NNUE network not loaded, using classical evaluation\n");
    }
    
    // Set up training data output
    set_training_data_path(config.output_file);
    
    // Print configuration
    printf("=== Training Data Generator ===\n");
    printf("Output file:       %s\n", config.output_file);
    printf("Number of games:   %d\n", config.num_games);
    printf("Random moves:      %d\n", config.random_moves);
    printf("Random probability: %d%%\n", config.random_probability);
    if (config.search_nodes > 0) {
        printf("Search nodes:      %llu\n", (unsigned long long)config.search_nodes);
    } else if (config.search_time_ms > 0) {
        printf("Search time:       %d ms\n", config.search_time_ms);
    } else {
        printf("Search depth:      %d\n", config.search_depth);
    }
    printf("Draw threshold:    %d moves\n", config.draw_threshold);
    printf("Max moves/game:    %d\n", config.max_game_moves);
    if (config.eval_threshold > 0) {
        printf("Eval threshold:    +/-%d pawns (%d cp)\n", config.eval_threshold, config.eval_threshold * 100);
    } else {
        printf("Eval threshold:    disabled\n");
    }
    if (config.adjudicate_threshold > 0) {
        printf("Adjudicate at:     +/-%d pawns (%d cp)\n", config.adjudicate_threshold, config.adjudicate_threshold * 100);
    } else {
        printf("Adjudicate at:     disabled\n");
    }
    printf("Filter tactics:    %s\n", config.filter_tactics ? "yes" : "no");
    printf("================================\n\n");
    
    // Initialize timing for statistics
    start_time = time(NULL);
    last_status_time = start_time;
    
    // Play games
    int game = 1;
    while (game <= config.num_games && !should_stop) {
        bool valid = play_game(game, nnue_network);
        if (valid) {
            game++;  // Only advance to next game if this one was valid
        } else {
            games_discarded++;
        }
        check_status_output();
    }
    
    // Final statistics
    time_t end_time = time(NULL);
    double total_elapsed = difftime(end_time, start_time);
    
    // Summary
    printf("\n=== Summary ===\n");
    printf("Games completed: %d/%d\n", games_completed, config.num_games);
    printf("Games discarded: %d\n", games_discarded);
    printf("Total positions: %d\n", total_positions);
    printf("Filtered (tactics): %d\n", filtered_positions);
    if (total_elapsed > 0) {
        printf("Total time:      %.1f seconds\n", total_elapsed);
        printf("Avg pos/sec:     %.1f\n", total_positions / total_elapsed);
    }
    printf("Training data written to: %s.*\n", config.output_file);
    
    // Cleanup
    free(nnue_network);
    
    return 0;
}
