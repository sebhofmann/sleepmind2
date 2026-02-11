#include "board_io.h"
#include "board.h"
#include "zobrist.h"

Board parseFEN(const char* fen) {
    Board board = {};
    
    // Initialize all bitboards and piece array to empty
    clear_piece_array(&board);
    board.historyIndex = 0;
    memset(board.history, 0, sizeof(board.history));

    // implement FEN parsing
    int rank = 7, file = 0;
    int fenIndex = 0;
    char currentChar;

    // 1. Piece placement - use put_piece for branchless updates
    while ((currentChar = fen[fenIndex]) != ' ' && currentChar != '\0') {
        if (currentChar == '/') {
            rank--;
            file = 0;
            fenIndex++;
            continue;
        }

        if (currentChar >= '1' && currentChar <= '8') {
            file += (currentChar - '0');
            fenIndex++;
            continue;
        }

        int sq = rank * 8 + file;
        uint8_t piece = NO_PIECE;

        switch (currentChar) {
            case 'P': piece = W_PAWN; break;
            case 'N': piece = W_KNIGHT; break;
            case 'B': piece = W_BISHOP; break;
            case 'R': piece = W_ROOK; break;
            case 'Q': piece = W_QUEEN; break;
            case 'K': piece = W_KING; break;
            case 'p': piece = B_PAWN; break;
            case 'n': piece = B_KNIGHT; break;
            case 'b': piece = B_BISHOP; break;
            case 'r': piece = B_ROOK; break;
            case 'q': piece = B_QUEEN; break;
            case 'k': piece = B_KING; break;
        }
        
        if (piece != NO_PIECE) {
            put_piece(&board, piece, sq);
        }
        
        file++;
        fenIndex++;
    }

    // Skip the space after piece placement
    if (fen[fenIndex] == ' ') {
        fenIndex++;
    }

    // 2. Active color
    if (fen[fenIndex] == 'w') {
        board.whiteToMove = true;
    } else if (fen[fenIndex] == 'b') {
        board.whiteToMove = false;
    }
    fenIndex++; // Move past active color character
    // Skip the space
    if (fen[fenIndex] == ' ') {
        fenIndex++;
    }

    // 3. Castling availability
    board.castlingRights = 0;
    if (fen[fenIndex] == '-') {
        fenIndex++;
    } else {
        while (fen[fenIndex] != ' ' && fen[fenIndex] != '\0') {
            switch (fen[fenIndex]) {
                case 'K': board.castlingRights |= WHITE_KINGSIDE_CASTLE; break;
                case 'Q': board.castlingRights |= WHITE_QUEENSIDE_CASTLE; break;
                case 'k': board.castlingRights |= BLACK_KINGSIDE_CASTLE; break;
                case 'q': board.castlingRights |= BLACK_QUEENSIDE_CASTLE; break;
            }
            fenIndex++;
        }
    }
    if (fen[fenIndex] == ' ') {
        fenIndex++;
    }


    // 4. En passant target square
    // TODO: Implement en passant target square parsing
    if (fen[fenIndex] != '-') {
        if (fen[fenIndex+1] >= '1' && fen[fenIndex+1] <= '8') {
             // Assuming format like "e3"
            int epFile = fen[fenIndex] - 'a';
            int epRank = fen[fenIndex+1] - '1';
            board.enPassantSquare = epRank * 8 + epFile;
            fenIndex += 2;
        } else {
            board.enPassantSquare = SQ_NONE; // No en passant
            fenIndex++; // Skip '-'
        }
    } else {
        board.enPassantSquare = SQ_NONE; // No en passant
        fenIndex++; // Skip '-'
    }

    if (fen[fenIndex] == ' ') {
        fenIndex++;
    }

    // 5. Halfmove clock
    // TODO: Implement halfmove clock parsing
    // sscanf is not ideal here for robustness, but for simplicity:
    int halfMoves = 0;
    sscanf(&fen[fenIndex], "%d", &halfMoves);
    board.halfMoveClock = halfMoves;
    while (fen[fenIndex] != ' ' && fen[fenIndex] != '\0') { // Move fenIndex past the number
        fenIndex++;
    }
    if (fen[fenIndex] == ' ') {
        fenIndex++;
    }


    // 6. Fullmove number
    // TODO: Implement fullmove number parsing
    int fullMoves = 0;
    sscanf(&fen[fenIndex], "%d", &fullMoves);
    board.fullMoveNumber = fullMoves;
    // No need to advance fenIndex further as this is the last part

    // 7. Zobrist key
    board.zobristKey = calculate_zobrist_key(&board);

    return board;
}

const char* outputFEN(const Board* board) {
    static char fen[128]; // Buffer for FEN string
    int index = 0;

    // 1. Piece placement
    for (int rank = 7; rank >= 0; rank--) {
        int emptyCount = 0;
        for (int file = 0; file < 8; file++) {
            Bitboard squareBit = 1ULL << (rank * 8 + file);

            if (board->whitePawns & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'P';
            } else if (board->whiteKnights & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'N';
            } else if (board->whiteBishops & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'B';
            } else if (board->whiteRooks & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'R';
            } else if (board->whiteQueens & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'Q';
            } else if (board->whiteKings & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'K';
            } else if (board->blackPawns & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'p';
            } else if (board->blackKnights & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'n';
            } else if (board->blackBishops & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'b';
            } else if (board->blackRooks & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'r';
            } else if (board->blackQueens & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'q';
            } else if (board->blackKings & squareBit) {
                if (emptyCount > 0) {
                    fen[index++] = '0' + emptyCount;
                    emptyCount = 0;
                }
                fen[index++] = 'k';
            } else {
                emptyCount++;
            }
        }
        if (emptyCount > 0) {
            fen[index++] = '0' + emptyCount;
        }
        if (rank > 0) {
            fen[index++] = '/';
        }
    }

    // 2. Active color
    fen[index++] = ' ';
    fen[index++] = board->whiteToMove ? 'w' : 'b';

    // 3. Castling availability
    fen[index++] = ' ';
    bool castlingAvailable = false;
    if (board->castlingRights & WHITE_KINGSIDE_CASTLE) { fen[index++] = 'K'; castlingAvailable = true; }
    if (board->castlingRights & WHITE_QUEENSIDE_CASTLE) { fen[index++] = 'Q'; castlingAvailable = true; }
    if (board->castlingRights & BLACK_KINGSIDE_CASTLE) { fen[index++] = 'k'; castlingAvailable = true; }
    if (board->castlingRights & BLACK_QUEENSIDE_CASTLE) { fen[index++] = 'q'; castlingAvailable = true; }
    if (!castlingAvailable) {
        fen[index++] = '-';
    }

    // 4. En passant target square
    fen[index++] = ' ';
    if (board->enPassantSquare != SQ_NONE) {
        fen[index++] = 'a' + (board->enPassantSquare % 8);
        fen[index++] = '1' + (board->enPassantSquare / 8);
    } else {
        fen[index++] = '-';
    }

    // 5. Halfmove clock
    fen[index++] = ' ';
    index += sprintf(&fen[index], "%d", board->halfMoveClock);

    // 6. Fullmove number
    fen[index++] = ' ';
    index += sprintf(&fen[index], "%d", board->fullMoveNumber);

    fen[index] = '\0'; // Null-terminate the string
    return fen;
}

void printBoard(const Board* board) {
    // Print rank numbers and the board
    for (int rank = 7; rank >= 0; rank--) {
        printf("%d ", rank + 1); // Print rank number (1-8)
        for (int file = 0; file < 8; file++) {
            Bitboard squareBit = 1ULL << (rank * 8 + file);
            const char* displayStr = ""; 

            if (board->whitePawns & squareBit) {
                displayStr = "♙";
            } else if (board->whiteKnights & squareBit) {
                displayStr = "♘";
            } else if (board->whiteBishops & squareBit) {
                displayStr = "♗";
            } else if (board->whiteRooks & squareBit) {
                displayStr = "♖";
            } else if (board->whiteQueens & squareBit) {
                displayStr = "♕";
            } else if (board->whiteKings & squareBit) {
                displayStr = "♔";
            } else if (board->blackPawns & squareBit) {
                displayStr = "♟";
            } else if (board->blackKnights & squareBit) {
                displayStr = "♞";
            } else if (board->blackBishops & squareBit) {
                displayStr = "♝";
            } else if (board->blackRooks & squareBit) {
                displayStr = "♜";
            } else if (board->blackQueens & squareBit) {
                displayStr = "♛";
            } else if (board->blackKings & squareBit) {
                displayStr = "♚";
            } else {
                // Empty square: A1 (rank 0, file 0) is dark.
                // (rank + file) % 2 == 0 for dark squares.
                if ((rank + file) % 2 != 0) { // Light square
                    displayStr = " "; // Use a space for empty light squares
                } else { // Dark square
                    displayStr = "·"; // Use a middle dot for empty dark squares
                }
            }
            printf("%s ", displayStr); // Print the character and a space for separation
        }
        printf("\n");
    }

    // Print file letters
    printf("  "); // Align with the board (rank numbers take 2 chars: "8 ")
    for (char f = 'a'; f <= 'h'; f++) {
        printf("%c ", f);
    }
    printf("\n\n"); // Extra newline for separation

    // Print game state information
    printf("To move: %s\n", board->whiteToMove ? "White" : "Black");
    
    if (board->enPassantSquare != SQ_NONE) {
        char epFile = 'a' + (board->enPassantSquare % 8);
        int epRank = (board->enPassantSquare / 8) + 1;
        printf("En passant: %c%d\n", epFile, epRank);
    } else {
        printf("En passant: -\n");
    }
    
    printf("Halfmove clock: %d\n", board->halfMoveClock);
    printf("Fullmove number: %d\n", board->fullMoveNumber);
}
