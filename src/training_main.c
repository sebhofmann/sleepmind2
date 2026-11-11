#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>

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
#include "syzygy.h"

// Network embedded into the binary at build time (src/embedded_nnue.S).
extern const unsigned char sleepmind_nnue_start[];
extern const unsigned char sleepmind_nnue_end[];

// =============================================================================
// Configuration
// =============================================================================
//
// Games are recorded whole (viriformat pipeline, see training_data.h). There
// is no eval adjudication and no position filtering here: games only end by
// the rules of chess (or by an exact Syzygy result), and check/tactical/
// eval-based filtering happens in the trainer's data loader.

typedef struct {
    char output_file[256];
    int random_moves;           // Number of random moves at start of game
    int random_probability;     // Probability (0-100) for each random move
    int draw_threshold;         // 50-move rule, in moves (draw after 2*N plies without progress)
    int max_game_moves;         // Safety limit in plies; games reaching it are DISCARDED
    int num_games;              // Number of games to play
    int search_depth;           // Search depth for moves
    int search_time_ms;         // Search time in milliseconds (0 = use depth)
    uint64_t search_nodes;      // Search node limit (0 = use depth or time)
    int verbose;                // Verbosity level
    int eval_threshold;         // Max eval (in pawns) after random moves, 0 = disabled
    char syzygy_path[1024];     // Syzygy tablebase path (empty = disabled)
    int syzygy_probe_limit;     // Max piece count for TB adjudication
    char nnue_file[1024];       // Explicit network file (overrides the embedded net)
} TrainingConfig;

static TrainingConfig config = {
    .output_file = "training_data.games",
    .random_moves = 12,
    .random_probability = 100,  // 100% random for first N moves
    .draw_threshold = 50,       // 50-move rule
    .max_game_moves = 2000,     // plies; only a safety net, rule-based endings come first
    .num_games = 100,
    .search_depth = 8,
    .search_time_ms = 0,
    .search_nodes = 0,          // 0 = use depth or time
    .verbose = 1,
    .eval_threshold = 1,        // Default: discard games with eval > +/-100cp after random moves
    .syzygy_path = "",          // Default: tablebases disabled
    .syzygy_probe_limit = 7,    // Max pieces for TB adjudication (when loaded)
    .nnue_file = ""
};

// Global flag for graceful shutdown
static volatile sig_atomic_t should_stop = 0;

// Statistics tracking
static int games_completed = 0;       // games written (or empty after TB hit at start)
static int games_discarded_eval = 0;  // opening eval outside +/-eval_threshold
static int games_discarded_limit = 0; // reached max_game_moves
static int games_aborted = 0;         // interrupted by a signal / no move found
static long long total_positions = 0;
static long long total_plies = 0;     // plies of completed games (incl. opening)
static int results_white = 0, results_draw = 0, results_black = 0;
static int results_tb = 0;
static time_t start_time = 0;
static time_t last_status_time = 0;

static void print_status(void) {
    time_t now = time(NULL);
    double elapsed = difftime(now, start_time);
    if (elapsed > 0) {
        printf("[Status: %d games, %d discarded (eval), %d discarded (move limit), "
               "%lld positions, %.1f pos/sec, %.0fs elapsed]\n",
               games_completed, games_discarded_eval, games_discarded_limit,
               total_positions, total_positions / elapsed, elapsed);
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

// Only async-signal-safe operations are allowed here: printf/fflush/malloc
// take locks the interrupted main thread may already hold -> deadlock.
// Message and data flush happen in the main loop when the flag is seen.
void signal_handler(int sig) {
    (void)sig;
    should_stop = 1;
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

typedef enum {
    PLAY_COMPLETED,         // game finished by the rules (or TB), record written
    PLAY_DISCARDED_EVAL,    // opening eval outside the threshold
    PLAY_DISCARDED_LIMIT,   // max_game_moves reached
    PLAY_ABORTED            // signal received or no move found
} PlayOutcome;

// Check for insufficient material
static bool is_insufficient_material(const Board* board) {
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

    // King + Bishop vs King + Bishop (same color bishops): no mate possible
    if (white_knights == 0 && black_knights == 0 &&
        white_bishops == 1 && black_bishops == 1) {
        int white_bishop_sq = __builtin_ctzll(board->whiteBishops);
        int black_bishop_sq = __builtin_ctzll(board->blackBishops);
        int white_color = (white_bishop_sq / 8 + white_bishop_sq % 8) % 2;
        int black_color = (black_bishop_sq / 8 + black_bishop_sq % 8) % 2;
        if (white_color == black_color) return true;
    }

    return false;
}

// Threefold repetition: every position of the game is kept (zobrist keys).
static uint64_t position_history[MAX_GAME_RECORD_PLIES + 1];

// board.history must hold a full-length game plus the search stack.
_Static_assert(MAX_GAME_RECORD_PLIES + MAX_PLY + 64 <= BOARD_HISTORY_SIZE,
               "BOARD_HISTORY_SIZE too small for MAX_GAME_RECORD_PLIES");
static int position_count = 0;

static void reset_position_history(void) {
    position_count = 0;
}

static void record_position(uint64_t hash) {
    if (position_count < MAX_GAME_RECORD_PLIES + 1) {
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
    generateLegalMoves(board, moves);

    // Checkmate or stalemate (takes precedence over the 50-move rule)
    if (moves->count == 0) {
        if (isKingAttacked(board, board->whiteToMove)) {
            return board->whiteToMove ? GAME_BLACK_WINS : GAME_WHITE_WINS;
        }
        return GAME_DRAW;
    }

    // 50-move rule
    if (half_move_clock >= config.draw_threshold * 2) {
        return GAME_DRAW;
    }

    // Insufficient material
    if (is_insufficient_material(board)) {
        return GAME_DRAW;
    }

    // Threefold repetition. The position just reached is recorded at the top
    // of the NEXT game-loop iteration, so it is not in position_history yet
    // and counts as one occurrence itself. Without the +1 this adjudicated
    // only on the 4th occurrence - one full repetition cycle too late (a
    // hung search on the 3rd occurrence is what stalled instances for hours).
    if (count_position_repetitions(board->zobristKey) + 1 >= 3) {
        return GAME_DRAW;
    }

    return GAME_ONGOING;
}

// Search score (side to move) -> recorded score (White's view, fits i16).
// Mate scores (and TB scores, should the search ever return one) are mapped
// to a fixed +/-TRAINING_MATE_SCORE so the loader's max_eval filter drops them.
static int record_score(int stm_score, bool white_to_move) {
    int s;
    if (stm_score >= TB_SCORE_MIN) {
        s = TRAINING_MATE_SCORE;
    } else if (stm_score <= -TB_SCORE_MIN) {
        s = -TRAINING_MATE_SCORE;
    } else if (stm_score > TRAINING_MAX_NORMAL_SCORE) {
        s = TRAINING_MAX_NORMAL_SCORE;
    } else if (stm_score < -TRAINING_MAX_NORMAL_SCORE) {
        s = -TRAINING_MAX_NORMAL_SCORE;
    } else {
        s = stm_score;
    }
    return white_to_move ? s : -s;
}

// =============================================================================
// Self-Play Game
// =============================================================================

static PlayOutcome play_game(int game_num, NNUENetwork* nnue_network) {
    Board board = parseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    NNUEAccumulator nnue_accumulator = {0};
    nnue_reset_accumulator(&board, &nnue_accumulator, nnue_network);

    MoveList moves;
    MoveUndoInfo undo_info;

    int ply = 0;
    int half_move_clock = 0;
    GameResult result = GAME_ONGOING;
    bool checked_threshold = false;  // eval checked once after the random phase
    bool tb_adjudicated = false;     // game ended by an exact tablebase result

    reset_position_history();
    clear_tt();
    game_record_discard();

    if (config.verbose >= 1) {
        printf("Game %d/%d starting...\n", game_num, config.num_games);
    }

    while (result == GAME_ONGOING) {
        if (should_stop) {
            game_record_discard();
            return PLAY_ABORTED;
        }
        // Safety net only: games normally end by the rules long before. A game
        // that reaches the limit has no real result, so it is not written.
        if (ply >= config.max_game_moves) {
            if (config.verbose >= 1) {
                printf("Game %d discarded: reached move limit of %d plies\n",
                       game_num, config.max_game_moves);
            }
            game_record_discard();
            return PLAY_DISCARDED_LIMIT;
        }

        // Record position for repetition detection
        record_position(board.zobristKey);

        generateLegalMoves(&board, &moves);
        if (moves.count == 0) break;  // cannot happen: game end is checked after each move

        Move best_move = 0;
        bool is_random_move = false;

        // Random opening moves
        if (ply < config.random_moves) {
            int roll = rand() % 100;
            if (roll < config.random_probability) {
                int idx = rand() % moves.count;
                best_move = moves.moves[idx];
                is_random_move = true;

                if (config.verbose >= 2) {
                    char move_str[6];
                    moveToString(best_move, move_str);
                    printf("  Ply %d: random move %s\n", ply, move_str);
                }
            }
        }

        // The record starts with the first position after the random phase.
        // Opening-phase plies (ply < random_moves) are never recorded: they
        // are only randomized with probability random_probability, the rest
        // would oversample the same few opening positions.
        if (ply == config.random_moves) {
            game_record_begin(&board);
        }

        if (!is_random_move) {
            // --- Tablebase adjudication ---
            // Once the position is within the tablebases the exact outcome is
            // known: end the game immediately and use the TB WDL as the game
            // result. The TB position itself is NOT recorded.
            Move tb_move = 0;
            int tb_wdl = 0, tb_dtz = 0;
            if (syzygy_max_pieces() > 0 && board.castlingRights == NO_CASTLING &&
                !isKingAttacked(&board, !board.whiteToMove)) {
                Bitboard tb_occ =
                    board.byTypeBB[WHITE][PAWN]   | board.byTypeBB[BLACK][PAWN]   |
                    board.byTypeBB[WHITE][KNIGHT] | board.byTypeBB[BLACK][KNIGHT] |
                    board.byTypeBB[WHITE][BISHOP] | board.byTypeBB[BLACK][BISHOP] |
                    board.byTypeBB[WHITE][ROOK]   | board.byTypeBB[BLACK][ROOK]   |
                    board.byTypeBB[WHITE][QUEEN]  | board.byTypeBB[BLACK][QUEEN]  |
                    board.byTypeBB[WHITE][KING]   | board.byTypeBB[BLACK][KING];
                int tb_pieces = POPCOUNT(tb_occ);
                if (tb_pieces <= config.syzygy_probe_limit &&
                    syzygy_available(tb_pieces) &&
                    syzygy_probe_play(&board, &tb_move, &tb_wdl, &tb_dtz) == 1) {
                    int white_wdl = board.whiteToMove ? tb_wdl : -tb_wdl;
                    result = white_wdl > 0 ? GAME_WHITE_WINS
                           : white_wdl < 0 ? GAME_BLACK_WINS : GAME_DRAW;
                    tb_adjudicated = true;
                    if (config.verbose >= 2) {
                        printf("  Ply %d: TB adjudication (wdl %d, dtz %d)\n",
                               ply, tb_wdl, tb_dtz);
                    }
                    break;
                }
            }

            SearchInfo search_info;
            search_info.startTimeMs = search_current_time_ms();
            search_params_init(&search_info.params);
            // Tablebases are not probed inside the search during training
            // data generation (adjudication above handles TB positions).
            search_info.tbProbeLimit = 0;
            search_info.tbHits = 0;
            search_info.tbRootMoveCount = 0;
            search_info.tbRootScore = 0;
            search_info.tbRootMatePlies = -1;
            search_info.tbRootPvLen = 0;

            if (config.search_nodes > 0) {
                // Node-based search: soft node limit between iterations, plus
                // the hard node cap in check_time() (search.c) as safety net.
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
            // Score of the last completed iteration, side-to-move view.
            int best_score = search_info.bestScoreThisIteration;

            if (config.verbose >= 2) {
                char move_str[6];
                moveToString(best_move, move_str);
                printf("  Ply %d: search move %s (score: %d)\n", ply, move_str, best_score);
            }

            // Check eval threshold after random moves (only once)
            if (!checked_threshold && config.eval_threshold > 0 && ply >= config.random_moves) {
                checked_threshold = true;
                int threshold_cp = config.eval_threshold * 100;
                if (best_score > threshold_cp || best_score < -threshold_cp) {
                    if (config.verbose >= 1) {
                        printf("Game %d discarded: eval %d cp exceeds threshold +/-%d cp\n",
                               game_num, best_score, threshold_cp);
                    }
                    game_record_discard();
                    return PLAY_DISCARDED_EVAL;
                }
            }

            if (best_move != 0 && ply >= config.random_moves) {
                game_record_add(best_move, record_score(best_score, board.whiteToMove));
            }
        }

        if (best_move == 0) {
            if (config.verbose >= 1) {
                printf("  No move found at ply %d!\n", ply);
            }
            game_record_discard();
            return PLAY_ABORTED;
        }

        // Pawn move or capture resets the 50-move counter
        bool is_capture = MOVE_IS_CAPTURE(best_move);
        Bitboard from_bb = 1ULL << MOVE_FROM(best_move);
        bool is_pawn_move = (board.whitePawns & from_bb) || (board.blackPawns & from_bb);

        applyMove(&board, best_move, &undo_info, &nnue_accumulator, nnue_network);
        ply++;

        if (is_pawn_move || is_capture) {
            half_move_clock = 0;
        } else {
            half_move_clock++;
        }

        result = check_game_result(&board, half_move_clock, &moves);
    }

    int result_value = 0;  // 1 = White wins, 0 = draw, -1 = Black wins
    const char* result_str = "draw";
    if (result == GAME_WHITE_WINS) {
        result_value = 1;
        result_str = "white wins";
        results_white++;
    } else if (result == GAME_BLACK_WINS) {
        result_value = -1;
        result_str = "black wins";
        results_black++;
    } else {
        results_draw++;
    }
    if (tb_adjudicated) {
        results_tb++;
        result_str = result_value > 0 ? "white wins (TB)"
                   : result_value < 0 ? "black wins (TB)" : "draw (TB)";
    }

    int entries_written = game_record_length();
    total_positions += entries_written;
    total_plies += ply;
    game_record_write(result_value);

    if (config.verbose >= 1) {
        printf("Game %d finished: %s after %d plies (%d recorded positions)\n",
               game_num, result_str, ply, entries_written);
    }

    games_completed++;
    return PLAY_COMPLETED;
}

// =============================================================================
// NNUE loading
// =============================================================================

static unsigned char* read_whole_file(const char* path, size_t* size_out) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char* data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0 && fseek(f, 0, SEEK_SET) == 0) {
            data = malloc((size_t)size);
            if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
                free(data);
                data = NULL;
            }
            if (data) *size_out = (size_t)size;
        }
    }
    fclose(f);
    return data;
}

// Loads a network file into `net`; prints source and hash on success.
static bool load_network_file(const char* path, NNUENetwork* net) {
    size_t size = 0;
    unsigned char* data = read_whole_file(path, &size);
    if (!data) {
        fprintf(stderr, "Error: cannot read NNUE file %s\n", path);
        return false;
    }
    bool ok = nnue_load_memory(data, size, net);
    if (ok) {
        printf("info string NNUE evaluation enabled (source: file %s, size %zu, fnv1a64 %016" PRIx64 ")\n",
               path, size, nnue_hash(data, size));
    }
    free(data);
    return ok;
}

// Embedded network first (like the engine), quantised.bin only as fallback,
// or exactly the file given with --nnue. Without a network the generator
// aborts: data from the classical eval would be worthless.
static bool load_network(NNUENetwork* net) {
    if (config.nnue_file[0]) {
        return load_network_file(config.nnue_file, net);
    }
    size_t embedded_size = (size_t)(sleepmind_nnue_end - sleepmind_nnue_start);
    if (nnue_load_memory(sleepmind_nnue_start, embedded_size, net)) {
        printf("info string NNUE evaluation enabled (source: embedded, size %zu, fnv1a64 %016" PRIx64 ")\n",
               embedded_size, nnue_hash(sleepmind_nnue_start, embedded_size));
        return true;
    }
    // Same net file the engine of this build embeds (quantised.bin / quantised_deep.bin)
    printf("info string Embedded NNUE unavailable, trying " NNUE_EMBED_FILE "\n");
    return load_network_file(NNUE_EMBED_FILE, net);
}

// =============================================================================
// Usage and Argument Parsing
// =============================================================================

static void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("\nWrites whole self-play games (one per line: start FEN | uci:score ... | result)\n");
    printf("for conversion into a viriformat binpack with training/datatools (viri_pack).\n");
    printf("\nOptions:\n");
    printf("  -o, --output FILE       Output file prefix, PID is appended (default: training_data.games)\n");
    printf("  -n, --num-games N       Number of games to play (default: 100)\n");
    printf("  -r, --random-moves N    Number of random moves at start (default: 12)\n");
    printf("  -p, --random-prob N     Probability (0-100) for random moves (default: 100)\n");
    printf("  -d, --depth N           Search depth (default: 8)\n");
    printf("  -t, --time MS           Search time in milliseconds (overrides depth)\n");
    printf("  -N, --nodes N           Search node limit (overrides depth and time)\n");
    printf("  --draw-threshold N      50-move rule in moves (default: 50)\n");
    printf("  --max-moves N           Safety limit in plies, games reaching it are discarded (default: 2000)\n");
    printf("  -e, --eval-threshold N  Max eval in pawns after random moves, discard if exceeded (0=off)\n");
    printf("  -v, --verbose LEVEL     Verbosity level 0-2 (default: 1)\n");
    printf("  -S, --syzygy-path PATH  Syzygy tablebase path for exact endgame adjudication (default: off)\n");
    printf("  -L, --syzygy-probe-limit N  Max piece count for TB adjudication (default: 7)\n");
    printf("      --nnue FILE         Use this network file instead of the embedded net\n");
    printf("  -h, --help              Show this help message\n");
    printf("\nExample:\n");
    printf("  %s -o data.games -n 1000 -r 12 -p 50 -N 5000 -e 4\n", program_name);
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
        {"verbose",        required_argument, 0, 'v'},
        {"syzygy-path",        required_argument, 0, 'S'},
        {"syzygy-probe-limit", required_argument, 0, 'L'},
        {"nnue",           required_argument, 0, 'W'},
        {"help",           no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "o:n:r:p:d:t:N:e:v:S:L:h", long_options, NULL)) != -1) {
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
            case 'v':
                config.verbose = atoi(optarg);
                break;
            case 'S':
                strncpy(config.syzygy_path, optarg, sizeof(config.syzygy_path) - 1);
                config.syzygy_path[sizeof(config.syzygy_path) - 1] = '\0';
                break;
            case 'L':
                config.syzygy_probe_limit = atoi(optarg);
                break;
            case 'W':
                strncpy(config.nnue_file, optarg, sizeof(config.nnue_file) - 1);
                config.nnue_file[sizeof(config.nnue_file) - 1] = '\0';
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                exit(1);
        }
    }

    // The record buffer and repetition history hold at most
    // MAX_GAME_RECORD_PLIES plies.
    if (config.max_game_moves <= 0 || config.max_game_moves > MAX_GAME_RECORD_PLIES) {
        config.max_game_moves = MAX_GAME_RECORD_PLIES;
    }
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    parse_arguments(argc, argv);

    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    init_zobrist_keys();
    init_tt(256);  // 256 MB transposition table
    initMoveGenerator();

    // Initialize random seed AFTER initMoveGenerator (which also calls srand)
    // Using multiplication ensures different PIDs give very different seeds
    srand((unsigned int)(time(NULL) * getpid()));

    // Disable search output for training (no "info depth" and "DEBUG" spam)
    set_search_silent(true);

    NNUENetwork* nnue_network = nnue_alloc_network();
    if (!nnue_network) {
        fprintf(stderr, "Error: Failed to allocate memory for NNUE network\n");
        return 1;
    }
    if (!load_network(nnue_network) || !nnue_network->loaded) {
        fprintf(stderr, "Error: no NNUE network available (embedded net invalid, "
                        NNUE_EMBED_FILE " missing or wrong size) - aborting\n");
        free(nnue_network);
        return 1;
    }

    // Initialize Syzygy tablebases (no-op if path is empty)
    syzygy_init(config.syzygy_path);

    set_training_data_path(config.output_file);

    printf("=== Training Data Generator (viriformat game records) ===\n");
    printf("Output file:       %s\n", get_training_data_path());
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
    printf("50-move rule:      %d moves\n", config.draw_threshold);
    printf("Move limit:        %d plies (games reaching it are discarded)\n", config.max_game_moves);
    if (config.eval_threshold > 0) {
        printf("Eval threshold:    +/-%d pawns (%d cp)\n", config.eval_threshold, config.eval_threshold * 100);
    } else {
        printf("Eval threshold:    disabled\n");
    }
    printf("Eval adjudication: none (games end by the rules only)\n");
    if (syzygy_max_pieces() > 0) {
        printf("Syzygy TB:         %s (adjudicate up to %d pieces, %d loaded)\n",
               config.syzygy_path, config.syzygy_probe_limit, syzygy_max_pieces());
    } else {
        printf("Syzygy TB:         disabled\n");
    }
    printf("================================\n\n");

    start_time = time(NULL);
    last_status_time = start_time;

    int game = 1;
    while (game <= config.num_games && !should_stop) {
        PlayOutcome outcome = play_game(game, nnue_network);
        switch (outcome) {
            case PLAY_COMPLETED:        game++; break;
            case PLAY_DISCARDED_EVAL:   games_discarded_eval++; break;
            // A game hitting the move limit counts towards num_games so a
            // pathological setting cannot loop forever.
            case PLAY_DISCARDED_LIMIT:  games_discarded_limit++; game++; break;
            case PLAY_ABORTED:          games_aborted++; break;
        }
        check_status_output();
    }

    if (should_stop) {
        printf("\nReceived signal, unfinished game dropped.\n");
    }

    double total_elapsed = difftime(time(NULL), start_time);

    printf("\n=== Summary ===\n");
    printf("Games completed:        %d/%d\n", games_completed, config.num_games);
    printf("  White/Draw/Black:     %d/%d/%d (TB adjudicated: %d)\n",
           results_white, results_draw, results_black, results_tb);
    if (games_completed > 0) {
        printf("  Avg game length:      %.1f plies\n", (double)total_plies / games_completed);
    }
    printf("Discarded (eval):       %d\n", games_discarded_eval);
    printf("Discarded (move limit): %d\n", games_discarded_limit);
    printf("Aborted:                %d\n", games_aborted);
    printf("Total positions:        %lld\n", total_positions);
    if (total_elapsed > 0) {
        printf("Total time:             %.1f seconds\n", total_elapsed);
        printf("Avg pos/sec:            %.1f\n", total_positions / total_elapsed);
    }
    printf("Training data written to: %s\n", get_training_data_path());

    close_training_data();
    syzygy_free();
    free(nnue_network);

    return 0;
}
