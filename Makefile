CC = gcc
CFLAGS = -g -Wall -Wextra -std=c11 -O3 -march=native # Added optimization flags for NNUE performance
DEBUG_FLAGS = -g -Wall -Wextra -std=c11 -O0 -march=native -DDEBUG_NNUE_INCREMENTAL
DEBUG_EVAL_FLAGS = -g -Wall -Wextra -std=c11 -O3 -march=native -DDEBUG_NNUE_EVAL

# Dateien
SRCS = main.c board_io.c move_generator.c move.c bitboard_utils.c uci.c search.c tt.c evaluation.c board_modifiers.c zobrist.c nnue.c
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

debug: CFLAGS = $(DEBUG_FLAGS)
debug: clean all

debug_eval: CFLAGS = $(DEBUG_EVAL_FLAGS)
debug_eval: clean all

.PHONY: all clean debug debug_eval
