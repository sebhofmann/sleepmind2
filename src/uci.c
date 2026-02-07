#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif
#include "uci.h"
#include "board.h"
#include "board_io.h"
#include "move.h"
#include "move_generator.h"
#include "board_modifiers.h"
#include "search.h" // Will be created later
#include "bitboard_utils.h" // ADDED
#include "nnue.h" // For NNUE accumulator management
#include "evaluation.h" // For eval_init
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h> // For time management
#include <stdint.h>
#include <string.h>


#define ENGINE_NAME "SleepMind UCI"
#define ENGINE_AUTHOR "Sebastian Hofmann (and Gemini, Claude and GPT-4)"

static Board current_board;
static MoveList move_list;
static int current_ply = 0;

// Function to parse moves in UCI format (e.g., "e2e4", "e7e8q")

// Function to parse moves in UCI format (e.g., "e2e4", "e7e8q")
Move parse_uci_move(Board* board, const char* move_str) {
    if (!move_str || strlen(move_str) < 4) return 0; // Invalid move string

    Square from_sq = (move_str[0] - 'a') + (move_str[1] - '1') * 8;
    Square to_sq = (move_str[2] - 'a') + (move_str[3] - '1') * 8;

    PieceTypeToken promotion_piece_type_from_str = NO_PIECE_TYPE; // Renamed for clarity
    if (strlen(move_str) == 5) {
        switch (move_str[4]) {
            case 'q': promotion_piece_type_from_str = QUEEN_T; break;
            case 'r': promotion_piece_type_from_str = ROOK_T; break;
            case 'b': promotion_piece_type_from_str = BISHOP_T; break;
            case 'n': promotion_piece_type_from_str = KNIGHT_T; break;
        }
    }

    generateMoves(board, &move_list);
    for (int i = 0; i < move_list.count; ++i) {
        Move m = move_list.moves[i];
        if (MOVE_FROM(m) == from_sq && MOVE_TO(m) == to_sq) {
            // Case 1: Promotion
            if (MOVE_IS_PROMOTION(m)) {
                PieceTypeToken promo_token_in_move;
                switch(MOVE_PROMOTION(m)) { // MOVE_PROMOTION extracts the engine's promotion piece identifier
                    case PROMOTION_N: promo_token_in_move = KNIGHT_T; break;
                    case PROMOTION_B: promo_token_in_move = BISHOP_T; break;
                    case PROMOTION_R: promo_token_in_move = ROOK_T; break;
                    case PROMOTION_Q: promo_token_in_move = QUEEN_T; break;
                    default: promo_token_in_move = NO_PIECE_TYPE; break; // Should not happen for a promotion move
                }
                if (promo_token_in_move == promotion_piece_type_from_str) {
                    return m; // Match: From, To, and Promotion Piece Type
                }
            // Case 2: Castling (Castling is NOT a promotion)
            } else if (MOVE_IS_CASTLING(m)) {
                if (promotion_piece_type_from_str == NO_PIECE_TYPE) { // Castling UCI moves don't have a 5th char
                    return m; // Match: From, To, and it's a castling move
                }
            // Case 3: Regular move (not promotion, not castling explicitly checked here but covered)
            } else if (promotion_piece_type_from_str == NO_PIECE_TYPE) {
                // This will match regular moves if they are not promotions.
                // If it was a promotion, it would have been caught by MOVE_IS_PROMOTION(m) above.
                // If it was castling, it would have been caught by MOVE_IS_CASTLING(m) above.
                return m; // Match: From, To, and no promotion specified in string
            }
        }
    }

    // If we reach here, the move was not found. Add debug info.
    printf("info string DEBUG: parse_uci_move: Move %s not found. Parsed from_sq=%d (%c%c), to_sq=%d (%c%c). Promotion=\'%c\'.\n",
           move_str,
           from_sq, (from_sq % 8) + 'a', (from_sq / 8) + '1',
           to_sq, (to_sq % 8) + 'a', (to_sq / 8) + '1',
           promotion_piece_type_from_str == NO_PIECE_TYPE ? ' ' : move_str[4]);
    fflush(stdout);

    printf("info string DEBUG: parse_uci_move: Board state when failing to parse %s:\n", move_str);
    fflush(stdout);
    const char* current_fen = outputFEN(board);
    printf("info string FEN: %s\n", current_fen);
    fflush(stdout);

    printf("info string DEBUG: parse_uci_move: Generated %d moves for this board state (WhiteToMove: %s):\n",
           move_list.count, board->whiteToMove ? "true" : "false");
    fflush(stdout);

    for (int i = 0; i < move_list.count; ++i) {
        char temp_move_str[6];
        moveToString(move_list.moves[i], temp_move_str);
        printf("info string DEBUG: Generated move %d: %s (Raw: %u, From: %d, To: %d, Promo: %d, Castle: %d, EP: %d, Capture: %d, DoublePawn: %d)\n",
               i, temp_move_str,
               move_list.moves[i],
               MOVE_FROM(move_list.moves[i]),
               MOVE_TO(move_list.moves[i]),
               MOVE_PROMOTION(move_list.moves[i]),
               MOVE_IS_CASTLING(move_list.moves[i]),
               MOVE_IS_EN_PASSANT(move_list.moves[i]),
               MOVE_IS_CAPTURE(move_list.moves[i]),
               MOVE_IS_DOUBLE_PAWN_PUSH(move_list.moves[i])
               );
        fflush(stdout);
    }
    return 0; // Move not found or invalid
}

// Perft: count leaf nodes to given depth. Uses global move_list and existing apply/undo.
// Now checks legality after applying each move (pseudo-legal move generation)
static unsigned long long perft(Board* board, int depth) {
    if (depth == 0) return 1ULL;

    MoveList local_moves;
    local_moves.count = 0;
    generateMoves(board, &local_moves);

    unsigned long long nodes = 0ULL;
    for (int i = 0; i < local_moves.count; ++i) {
        Move m = local_moves.moves[i];
        MoveUndoInfo undo_info;
        applyMove(board, m, &undo_info, NULL, NULL);  // perft doesn't need NNUE
        
        // Skip illegal moves (king left in check) - move generator now returns pseudo-legal moves
        if (isKingAttacked(board, !board->whiteToMove)) {
            undoMove(board, m, &undo_info, NULL, NULL);
            continue;
        }
        
        if (depth == 1) {
            nodes++;
        } else {
            nodes += perft(board, depth - 1);
        }
        undoMove(board, m, &undo_info, NULL, NULL);  // perft doesn't need NNUE
    }
    return nodes;
}

// Perft divide: print per-move counts and return total
// Now checks legality after applying each move (pseudo-legal move generation)
static unsigned long long perft_divide(Board* board, int depth) {
    MoveList local_moves;
    local_moves.count = 0;
    generateMoves(board, &local_moves);
    unsigned long long total = 0ULL;

    for (int i = 0; i < local_moves.count; ++i) {
        Move m = local_moves.moves[i];
        MoveUndoInfo undo_info;
        applyMove(board, m, &undo_info, NULL, NULL);  // perft doesn't need NNUE
        
        // Skip illegal moves (king left in check) - move generator now returns pseudo-legal moves
        if (isKingAttacked(board, !board->whiteToMove)) {
            undoMove(board, m, &undo_info, NULL, NULL);
            continue;
        }
        
        unsigned long long cnt;
        if (depth == 1) {
            cnt = 1;
        } else {
            cnt = perft(board, depth - 1);
        }
        undoMove(board, m, &undo_info, NULL, NULL);  // perft doesn't need NNUE

        char move_str[6];
        moveToString(m, move_str);
        printf("%s: %llu\n", move_str, cnt);
        fflush(stdout);
        total += cnt;
    }

    printf("Total: %llu\n", total);
    fflush(stdout);
    return total;
}


void uci_loop() {
    char line[4096];
    MoveUndoInfo undo_info;
    
    // NNUE network is ~7.5 MB - must be on heap, not stack (stack is only ~1MB on Windows)
    NNUENetwork* nnue_network = (NNUENetwork*)calloc(1, sizeof(NNUENetwork));
    if (!nnue_network) {
        fprintf(stderr, "Error: Failed to allocate memory for NNUE network\n");
        return;
    }
    NNUEAccumulator nnue_accumulator = {0};  // Accumulator is small (~4KB), can stay on stack
    
    // Search parameters - initialized with defaults
    SearchParams search_params;
    search_params_init(&search_params);

    printf("DEBUG: Starting uci_loop initialization\n"); fflush(stdout);
    
    initMoveGenerator(); // Initialize move generator data
    printf("DEBUG: Move generator initialized\n"); fflush(stdout);
    
    eval_init("quantised.bin", nnue_network);  // Load NNUE network
    printf("DEBUG: NNUE initialized, loaded=%d\n", nnue_network->loaded); fflush(stdout);

    // Default to standard start position so commands like "perft" work
    current_board = parseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    printf("DEBUG: Board parsed\n"); fflush(stdout);
    
    nnue_reset_accumulator(&current_board, &nnue_accumulator, nnue_network);  // Initialize NNUE for starting position
    printf("DEBUG: NNUE accumulator reset\n"); fflush(stdout);

    printf("%s by %s\n", ENGINE_NAME, ENGINE_AUTHOR);
    printf("DEBUG: Starting main loop\n"); fflush(stdout);

    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0; // Remove newline

        if (strcmp(line, "uci") == 0) {
            printf("id name %s\n", ENGINE_NAME);
            printf("id author %s\n", ENGINE_AUTHOR);
            // Feature enable/disable options
            printf("option name Use_LMR type check default true\n");
            printf("option name Use_NullMove type check default true\n");
            printf("option name Use_Futility type check default true\n");
            printf("option name Use_RFP type check default true\n");
            printf("option name Use_DeltaPruning type check default false\n");
            printf("option name Use_Aspiration type check default true\n");
            // Search parameter options
            printf("option name LMR_FullDepthMoves type spin default 3 min 1 max 10\n");
            printf("option name LMR_ReductionLimit type spin default 2 min 1 max 6\n");
            printf("option name NullMove_Reduction type spin default 3 min 1 max 5\n");
            printf("option name NullMove_MinDepth type spin default 3 min 1 max 6\n");
            printf("option name Futility_Margin type spin default 150 min 50 max 400\n");
            printf("option name Futility_MarginD2 type spin default 300 min 100 max 600\n");
            printf("option name Futility_MarginD3 type spin default 450 min 150 max 800\n");
            printf("option name RFP_Margin type spin default 80 min 50 max 300\n");
            printf("option name RFP_MaxDepth type spin default 8 min 2 max 10\n");
            printf("option name Delta_Margin type spin default 200 min 50 max 500\n");
            printf("option name Aspiration_Window type spin default 100 min 10 max 200\n");
            printf("uciok\n");
            fflush(stdout);
        } else if (strcmp(line, "isready") == 0) {
            printf("readyok\n");
            fflush(stdout);
        } else if (strncmp(line, "setoption", 9) == 0) {
            // Parse: setoption name <name> value <value>
            char* name_start = strstr(line, "name ");
            char* value_start = strstr(line, "value ");
            if (name_start && value_start) {
                name_start += 5; // Skip "name "
                value_start += 6; // Skip "value "
                
                // Extract option name (everything between "name " and " value")
                char option_name[64] = {0};
                int name_len = value_start - 6 - name_start - 1;
                if (name_len > 0 && name_len < 63) {
                    strncpy(option_name, name_start, name_len);
                    option_name[name_len] = '\0';
                    // Trim trailing spaces
                    while (name_len > 0 && option_name[name_len - 1] == ' ') {
                        option_name[--name_len] = '\0';
                    }
                }
                
                int value = atoi(value_start);
                bool bool_value = (strcmp(value_start, "true") == 0 || strcmp(value_start, "1") == 0);
                
                // Match option names and set values in search_params
                // Feature enable/disable flags
                if (strcmp(option_name, "Use_LMR") == 0) {
                    search_params.use_lmr = bool_value;
                    printf("info string Set Use_LMR to %s\n", bool_value ? "true" : "false");
                } else if (strcmp(option_name, "Use_NullMove") == 0) {
                    search_params.use_null_move = bool_value;
                    printf("info string Set Use_NullMove to %s\n", bool_value ? "true" : "false");
                } else if (strcmp(option_name, "Use_Futility") == 0) {
                    search_params.use_futility = bool_value;
                    printf("info string Set Use_Futility to %s\n", bool_value ? "true" : "false");
                } else if (strcmp(option_name, "Use_RFP") == 0) {
                    search_params.use_rfp = bool_value;
                    printf("info string Set Use_RFP to %s\n", bool_value ? "true" : "false");
                } else if (strcmp(option_name, "Use_DeltaPruning") == 0) {
                    search_params.use_delta_pruning = bool_value;
                    printf("info string Set Use_DeltaPruning to %s\n", bool_value ? "true" : "false");
                } else if (strcmp(option_name, "Use_Aspiration") == 0) {
                    search_params.use_aspiration = bool_value;
                    printf("info string Set Use_Aspiration to %s\n", bool_value ? "true" : "false");
                // Numeric parameters
                } else if (strcmp(option_name, "LMR_FullDepthMoves") == 0) {
                    search_params.lmr_full_depth_moves = value;
                    printf("info string Set LMR_FullDepthMoves to %d\n", value);
                } else if (strcmp(option_name, "LMR_ReductionLimit") == 0) {
                    search_params.lmr_reduction_limit = value;
                    printf("info string Set LMR_ReductionLimit to %d\n", value);
                } else if (strcmp(option_name, "NullMove_Reduction") == 0) {
                    search_params.null_move_reduction = value;
                    printf("info string Set NullMove_Reduction to %d\n", value);
                } else if (strcmp(option_name, "NullMove_MinDepth") == 0) {
                    search_params.null_move_min_depth = value;
                    printf("info string Set NullMove_MinDepth to %d\n", value);
                } else if (strcmp(option_name, "Futility_Margin") == 0) {
                    search_params.futility_margin = value;
                    printf("info string Set Futility_Margin to %d\n", value);
                } else if (strcmp(option_name, "Futility_MarginD2") == 0) {
                    search_params.futility_margin_d2 = value;
                    printf("info string Set Futility_MarginD2 to %d\n", value);
                } else if (strcmp(option_name, "Futility_MarginD3") == 0) {
                    search_params.futility_margin_d3 = value;
                    printf("info string Set Futility_MarginD3 to %d\n", value);
                } else if (strcmp(option_name, "RFP_Margin") == 0) {
                    search_params.rfp_margin = value;
                    printf("info string Set RFP_Margin to %d\n", value);
                } else if (strcmp(option_name, "RFP_MaxDepth") == 0) {
                    search_params.rfp_max_depth = value;
                    printf("info string Set RFP_MaxDepth to %d\n", value);
                } else if (strcmp(option_name, "Delta_Margin") == 0) {
                    search_params.delta_margin = value;
                    printf("info string Set Delta_Margin to %d\n", value);
                } else if (strcmp(option_name, "Aspiration_Window") == 0) {
                    search_params.aspiration_window = value;
                    printf("info string Set Aspiration_Window to %d\n", value);
                } else {
                    printf("info string Unknown option: %s\n", option_name);
                }
                fflush(stdout);
            }
        } else if (strcmp(line, "ucinewgame") == 0) {
            current_ply = 0;
            // Reset board to startpos
            current_board = parseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
            nnue_reset_accumulator(&current_board, &nnue_accumulator, nnue_network);
        } else if (strncmp(line, "position", 8) == 0) {
            char* token;
            char* rest = line + 9; // Skip "position "

            token = strtok_r(rest, " ", &rest);
            if (strcmp(token, "startpos") == 0) {
                current_board = parseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
                nnue_reset_accumulator(&current_board, &nnue_accumulator, nnue_network);  // Reset NNUE for new position
                current_ply = 0;
                printf("info string DEBUG: UCI: Parsed startpos. WhiteToMove: %s. e2_pawn: %llu, d2_pawn: %llu, e7_pawn: %llu\n", // MODIFIED %d to %llu
                       current_board.whiteToMove ? "true" : "false",
                       GET_BIT(current_board.whitePawns, SQ_E2),
                       GET_BIT(current_board.whitePawns, SQ_D2),
                       GET_BIT(current_board.blackPawns, SQ_E7));
                fflush(stdout);
                token = strtok_r(NULL, " ", &rest); // Check for "moves"
            } else if (strcmp(token, "fen") == 0) {
                char fen_str[256] = "";
                int fen_parts = 0;
                while ((token = strtok_r(NULL, " ", &rest)) != NULL && fen_parts < 6) {
                    strcat(fen_str, token);
                    strcat(fen_str, " ");
                    fen_parts++;
                    if (strcmp(token, "moves") == 0) { // "moves" can be part of FEN if not careful
                        fen_str[strlen(fen_str) - strlen("moves")-1] = '\0'; // remove "moves "
                        break;
                    }
                }
                fen_str[strlen(fen_str)-1] = '\0'; // remove trailing space
                current_board = parseFEN(fen_str);
                nnue_reset_accumulator(&current_board, &nnue_accumulator, nnue_network);  // Reset NNUE for new position
                printf("info string DEBUG: UCI: Parsed FEN: \'%s\'. Resulting WhiteToMove: %s\n",
                       fen_str, current_board.whiteToMove ? "true" : "false");
                fflush(stdout);
                if (token && strcmp(token, "moves") != 0) { // if "moves" was not the last part of fen
                     token = strtok_r(NULL, " ", &rest); // Check for "moves"
                }

            }

            if (token && strcmp(token, "moves") == 0) {
                printf("info string DEBUG: UCI: Entering moves parsing loop.\n"); fflush(stdout);
                int move_idx = 0;
                char* current_move_token; 
                while ((current_move_token = strtok_r(NULL, " ", &rest)) != NULL) {
                    Move move = parse_uci_move(&current_board, current_move_token);
                    if (move != 0) {
                        applyMove(&current_board, move, &undo_info, &nnue_accumulator, nnue_network);  // Update NNUE
                        current_ply++;
                        // Log after applying, to see the state if needed, or confirm application
                        printf("info string DEBUG: UCI: Move '%s' (parsed as %u) successfully applied.\n", current_move_token, move); fflush(stdout);
                    } else {
                        printf("info string Error: Could not parse UCI move '%s' (index %d). Engine will exit.\n", current_move_token, move_idx);
                        // Print board state before exiting for easier debugging
                        const char* fen_at_error = outputFEN(&current_board);
                        printf("info string FEN at error: %s\n", fen_at_error);
                        printf("info string DEBUG: parse_uci_move: Generated %d moves for this board state (WhiteToMove: %s) when failing to parse %s:\n",
                               move_list.count, current_board.whiteToMove ? "true" : "false", current_move_token);
                        for (int i = 0; i < move_list.count; ++i) {
                            char temp_move_str[6];
                            moveToString(move_list.moves[i], temp_move_str);
                            printf("info string DEBUG: Generated move %d: %s (Raw: %u, From: %d, To: %d, Promo: %d, Castle: %d, EP: %d, Capture: %d, DoublePawn: %d)\n",
                                   i, temp_move_str,
                                   move_list.moves[i],
                                   MOVE_FROM(move_list.moves[i]),
                                   MOVE_TO(move_list.moves[i]),
                                   MOVE_PROMOTION(move_list.moves[i]),
                                   MOVE_IS_CASTLING(move_list.moves[i]),
                                   MOVE_IS_EN_PASSANT(move_list.moves[i]),
                                   MOVE_IS_CAPTURE(move_list.moves[i]),
                                   MOVE_IS_DOUBLE_PAWN_PUSH(move_list.moves[i])
                                   );
                        }
                        fflush(stdout);
                        exit(1); // Exit the engine
                    }
                    move_idx++;
                }
                printf("info string DEBUG: UCI: Exited moves parsing loop. Processed %d moves.\n", move_idx); fflush(stdout);
            }
        } else if (strncmp(line, "perft", 5) == 0) {
            char* token;
            char* rest = line + 6; // skip "perft "
            int divide = 0;
            token = strtok_r(rest, " ", &rest);
            if (token && strcmp(token, "divide") == 0) {
                divide = 1;
                token = strtok_r(NULL, " ", &rest);
            }
            int depth = 0;
            if (token) depth = atoi(token);
            if (depth <= 0) {
                printf("info string Error: perft requires positive depth\n");
                fflush(stdout);
            } else {
                printf("info string DEBUG: UCI: Running perft depth %d (divide=%s)\n", depth, divide ? "true" : "false"); fflush(stdout);
                struct timespec t_start, t_end;
                clock_gettime(CLOCK_MONOTONIC, &t_start);
                if (divide) {
                    unsigned long long total = perft_divide(&current_board, depth);
                    clock_gettime(CLOCK_MONOTONIC, &t_end);
                    double elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0 + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;
                    double nps = elapsed_ms > 0.0 ? (total / (elapsed_ms / 1000.0)) : 0.0;
                    printf("info string perft depth %d completed: %llu nodes in %.3f ms (nps: %.0f)\n", depth, total, elapsed_ms, nps);
                    fflush(stdout);
                } else {
                    unsigned long long nodes = perft(&current_board, depth);
                    clock_gettime(CLOCK_MONOTONIC, &t_end);
                    double elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0 + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;
                    double nps = elapsed_ms > 0.0 ? (nodes / (elapsed_ms / 1000.0)) : 0.0;
                    printf("perft %d: %llu\n", depth, nodes);
                    printf("info string perft time: %.3f ms, nps: %.0f\n", elapsed_ms, nps);
                    fflush(stdout);
                }
            }
        } else if (strncmp(line, "go", 2) == 0) {
            printf("info string DEBUG: UCI: Received \'go\' command: %s\n", line);
            fflush(stdout); 

            // Verbesstes Zeitmanagement mit Soft- und Hard-Limits
            long wtime = 0, btime = 0, winc = 0, binc = 0;
            int movestogo = 0;
            int depth_limit = 0;
            uint64_t node_limit = 0;  // Knotenlimit (go nodes X)
            long movetime = 0;  // Feste Zeit pro Zug (go movetime X)
            bool infinite = false;

            char* token;
            char* rest = line + 3;
            while((token = strtok_r(rest, " ", &rest))) {
                if(strcmp(token, "wtime") == 0 && (token = strtok_r(NULL, " ", &rest))) wtime = atol(token);
                else if(strcmp(token, "btime") == 0 && (token = strtok_r(NULL, " ", &rest))) btime = atol(token);
                else if(strcmp(token, "winc") == 0 && (token = strtok_r(NULL, " ", &rest))) winc = atol(token);
                else if(strcmp(token, "binc") == 0 && (token = strtok_r(NULL, " ", &rest))) binc = atol(token);
                else if(strcmp(token, "movestogo") == 0 && (token = strtok_r(NULL, " ", &rest))) movestogo = atoi(token);
                else if(strcmp(token, "depth") == 0 && (token = strtok_r(NULL, " ", &rest))) depth_limit = atoi(token);
                else if(strcmp(token, "nodes") == 0 && (token = strtok_r(NULL, " ", &rest))) node_limit = strtoull(token, NULL, 10);
                else if(strcmp(token, "movetime") == 0 && (token = strtok_r(NULL, " ", &rest))) movetime = atol(token);
                else if(strcmp(token, "infinite") == 0) infinite = true;
            }

            long current_player_time = current_board.whiteToMove ? wtime : btime;
            long current_player_inc = current_board.whiteToMove ? winc : binc;

            long soft_limit, hard_limit;

            if (infinite || depth_limit > 0 || node_limit > 0) {
                // Unendliche Suche, Tiefenbegrenzung oder Knotenbegrenzung
                soft_limit = 0;
                hard_limit = 0;
            } else if (movetime > 0) {
                // Feste Zeit pro Zug
                soft_limit = movetime;
                hard_limit = movetime;
            } else if (current_player_time > 0) {
                // Normale Zeitkontrolle
                // Berechne die erwartete Anzahl der verbleibenden Züge
                int expected_moves = movestogo > 0 ? movestogo : 25;  // Annahme: 25 Züge bis Spielende (aggressiver)
                
                // Basis-Zeit pro Zug
                long base_time = current_player_time / expected_moves;
                
                // Inkrement voll hinzufügen
                long inc_bonus = current_player_inc;
                
                // Soft-Limit: Zeit, nach der keine neue Tiefe begonnen wird
                soft_limit = base_time + inc_bonus;
                
                // Stelle sicher, dass wir nicht zu viel Zeit nutzen (max 25% der Gesamtzeit)
                long max_time = current_player_time / 4;
                if (soft_limit > max_time) {
                    soft_limit = max_time;
                }
                
                // Hard-Limit: Absolutes Maximum (2.5x Soft-Limit, aber max 40% der Zeit)
                hard_limit = (soft_limit * 5) / 2;
                long absolute_max = (current_player_time * 40) / 100;
                if (hard_limit > absolute_max) {
                    hard_limit = absolute_max;
                }
                
                // Mindestzeit garantieren
                if (soft_limit < 50) soft_limit = 50;
                if (hard_limit < 100) hard_limit = 100;
                
                // Bei sehr wenig Zeit: aggressivere Einstellungen
                if (current_player_time < 1000) {
                    soft_limit = current_player_time / 8;
                    hard_limit = current_player_time / 4;
                    if (soft_limit < 10) soft_limit = 10;
                    if (hard_limit < 20) hard_limit = 20;
                }
            } else {
                // Keine Zeitkontrolle angegeben - Standard
                soft_limit = 2000;
                hard_limit = 5000;
            }

            printf("info string Time management: soft=%ld ms, hard=%ld ms (time=%ld, inc=%ld, movestogo=%d)\n",
                   soft_limit, hard_limit, current_player_time, current_player_inc, movestogo);
            fflush(stdout);

            printBoard(&current_board); // Print the current board state for debugging

            SearchInfo search_info;
            search_info.startTime = clock();
            search_info.softTimeLimit = soft_limit;
            search_info.hardTimeLimit = hard_limit;
            search_info.stopSearch = false;
            search_info.lastIterationTime = 0;
            search_info.nnue_acc = &nnue_accumulator;  // Use the local NNUE accumulator
            search_info.nnue_net = nnue_network;       // Use the heap-allocated NNUE network
            search_info.nodesSearched = 0;
            search_info.bestMoveThisIteration = 0;
            search_info.bestScoreThisIteration = 0;
            search_info.seldepth = 0;
            search_info.depthLimit = depth_limit;  // Set depth limit from UCI
            search_info.nodeLimit = node_limit;     // Set node limit from UCI
            search_info.params = search_params;    // Copy search parameters
            clear_search_history(&search_info);  // Initialize killer moves, history, etc.


            // This is where the search will be called.
            // For now, let's just pick the first legal move.
            // Later, this will be replaced by iterative deepening alpha-beta search.
            // Move best_move = find_best_move(&current_board, &search_info);
            generateMoves(&current_board, &move_list);
            printf("info string DEBUG: UCI: Generated %d moves before calling search.\n", move_list.count);
            fflush(stdout); // CHANGED from stderr
            Move best_move = 0;
            
            if (move_list.count > 0) {
                // Normal search
                printf("info string DEBUG: UCI: Calling iterative_deepening_search...\n");
                const char* fen_before_search = outputFEN(&current_board);
                printf("info string FEN: %s\n", fen_before_search);
                fflush(stdout);
                best_move = iterative_deepening_search(&current_board, &search_info);
                printf("info string DEBUG: UCI: iterative_deepening_search returned. Best move: %u\n", best_move);
                fflush(stdout);
                printf("Best score: %d\n", search_info.bestScoreThisIteration);
            } else {
                printf("info string DEBUG: UCI: No moves generated, not calling search.\n");
                fflush(stdout);
            }


            if (best_move != 0) {
                char move_str[6];
                moveToString(best_move, move_str);
                printf("bestmove %s\n", move_str);
            } else {
                printf("bestmove 0000\n"); // Should not happen in a legal position
            }

        } else if (strcmp(line, "stop") == 0) {
            // stop_search(); // This function will set a flag for the search to stop
            // For now, this doesn't do much until search is implemented
             printf("info string stop command received (no active search to stop yet)\n");
        } else if (strcmp(line, "quit") == 0) {
            break;
        }
        fflush(stdout);
        // fflush(stderr); // Consider adding here as well for other commands if needed
    }
    
    // Free heap-allocated NNUE network
    free(nnue_network);
}
