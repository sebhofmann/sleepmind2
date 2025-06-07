#include "uci.h"
#include "board.h"
#include "board_io.h"
#include "move.h"
#include "move_generator.h"
#include "board_modifiers.h"
#include "search.h" // Will be created later
#include "bitboard_utils.h" // ADDED
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h> // For time management

#define ENGINE_NAME "SleepMind UCI"
#define ENGINE_AUTHOR "Sebastian Hofmann (and Gemini, Claude and GPT-4)"

static Board current_board;
static MoveList move_list;

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


void uci_loop() {
    char line[4096];
    MoveUndoInfo undo_info;

    initMoveGenerator(); // Initialize move generator data
    // init_tt(); // Initialize transposition table - will be added later

    printf("%s by %s\n", ENGINE_NAME, ENGINE_AUTHOR);

    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0; // Remove newline

        if (strcmp(line, "uci") == 0) {
            printf("id name %s\n", ENGINE_NAME);
            printf("id author %s\n", ENGINE_AUTHOR);
            // Add options here if any
            printf("uciok\n");
        } else if (strcmp(line, "isready") == 0) {
            printf("readyok\n");
        } else if (strncmp(line, "position", 8) == 0) {
            char* token;
            char* rest = line + 9; // Skip "position "

            token = strtok_r(rest, " ", &rest);
            if (strcmp(token, "startpos") == 0) {
                current_board = parseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
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
                char* current_move_token; // Use a new variable for the token from strtok_r
                while ((current_move_token = strtok_r(NULL, " ", &rest)) != NULL) {
                    Move move = parse_uci_move(&current_board, current_move_token);
                    if (move != 0) {
                        applyMove(&current_board, move, &undo_info);
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
        } else if (strncmp(line, "go", 2) == 0) {
            printf("info string DEBUG: UCI: Received \'go\' command: %s\n", line);
            fflush(stdout); 

            // Basic time management: use 1/20th of available time
            long wtime = 0, btime = 0; //, winc = 0, binc = 0; // movestogo = 0; // REMOVED winc, binc
            // int depth_to_search = 6; // Default depth
            long time_for_move = 1000; // Default 1 second if no time control

            char* token;
            char* rest = line + 3;
            while((token = strtok_r(rest, " ", &rest))) {
                if(strcmp(token, "wtime") == 0 && (token = strtok_r(NULL, " ", &rest))) wtime = atol(token);
                else if(strcmp(token, "btime") == 0 && (token = strtok_r(NULL, " ", &rest))) btime = atol(token);
                // else if(strcmp(token, "winc") == 0 && (token = strtok_r(NULL, " ", &rest))) winc = atol(token); // REMOVED
                // else if(strcmp(token, "binc") == 0 && (token = strtok_r(NULL, " ", &rest))) binc = atol(token); // REMOVED
                // else if(strcmp(token, "movestogo") == 0 && (token = strtok_r(NULL, " ", &rest))) movestogo = atoi(token);
                // else if(strcmp(token, "depth") == 0 && (token = strtok_r(NULL, " ", &rest))) depth_to_search = atoi(token);
            }

            long current_player_time = current_board.whiteToMove ? wtime : btime;
            // long current_player_inc = current_board.whiteToMove ? winc : binc;

            if (current_player_time > 0) {
                time_for_move = current_player_time / 20;
                if (time_for_move == 0 && current_player_time > 0) time_for_move = 1; // Ensure at least 1ms if time is very low but >0
            } else if (wtime == 0 && btime == 0) { // No time control given, could be infinite or fixed depth
                 time_for_move = 2000; // Default to 2 seconds if no time control, adjust as needed
            }
            printf("info string DEBUG: UCI: Time for move: %ld ms (wtime: %ld, btime: %ld)\n", time_for_move, wtime, btime);
            fflush(stdout); // CHANGED from stderr

            printBoard(&current_board); // Print the current board state for debugging

            SearchInfo search_info;
            search_info.startTime = clock();
            search_info.timeLimit = time_for_move; // in milliseconds
            search_info.stopSearch = false;


            // This is where the search will be called.
            // For now, let's just pick the first legal move.
            // Later, this will be replaced by iterative deepening alpha-beta search.
            // Move best_move = find_best_move(&current_board, &search_info);
            generateMoves(&current_board, &move_list);
            printf("info string DEBUG: UCI: Generated %d moves before calling search.\n", move_list.count);
            fflush(stdout); // CHANGED from stderr
            Move best_move = 0;
            if (move_list.count > 0) {
                printf("info string DEBUG: UCI: Calling iterative_deepening_search...\n");
                const char* fen_before_search = outputFEN(&current_board); // ADDED
                printf("info string FEN: %s\n", fen_before_search); // ADDED
                fflush(stdout); // CHANGED from stderr
                best_move = iterative_deepening_search(&current_board, &search_info);
                printf("info string DEBUG: UCI: iterative_deepening_search returned. Best move: %u\n", best_move);
                fflush(stdout); // CHANGED from stderr
            } else {
                printf("info string DEBUG: UCI: No moves generated, not calling search.\n");
                fflush(stdout); // CHANGED from stderr
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
}
