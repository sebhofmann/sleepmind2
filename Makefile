CC = gcc
CFLAGS = -g -Wall -Wextra -std=c11 # Added -Wextra and -std=c11 for better warnings and C standard

# Dateien
SRCS = main.c board_io.c move_generator.c move.c bitboard_utils.c uci.c search.c tt.c evaluation.c board_modifiers.c zobrist.c
BUILD_DIR = build
OBJS = $(addprefix $(BUILD_DIR)/, $(SRCS:.c=.o))
EXEC = $(BUILD_DIR)/sleepmind

# Regeln
all: $(BUILD_DIR) $(EXEC)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(EXEC): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
