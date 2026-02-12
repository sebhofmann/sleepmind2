CC = gcc
CFLAGS = -g -Wall -Wextra -std=c11 -O3 -march=native
DEBUG_FLAGS = -g -Wall -Wextra -std=c11 -O0 -march=native -DDEBUG_NNUE_INCREMENTAL
DEBUG_EVAL_FLAGS = -g -Wall -Wextra -std=c11 -O3 -march=native -DDEBUG_NNUE_EVAL

# Optional flags: make STATS=1
ifeq ($(STATS),1)
  CFLAGS += -DSEARCH_STATS
endif

# Directories
SRC_DIR = src
BUILD_DIR = build

# Common source files (shared between engine and training)
COMMON_SRCS = board_io.c move_generator.c move.c bitboard_utils.c search.c tt.c evaluation.c board_modifiers.c zobrist.c nnue.c

# Engine source files
ENGINE_SRCS = main.c uci.c $(COMMON_SRCS)
ENGINE_OBJS = $(addprefix $(BUILD_DIR)/, $(ENGINE_SRCS:.c=.o))
ENGINE_EXEC = $(BUILD_DIR)/sleepmind

# Training data generator source files
TRAINING_SRCS = training_main.c training_data.c $(COMMON_SRCS)
TRAINING_OBJS = $(addprefix $(BUILD_DIR)/, $(TRAINING_SRCS:.c=.o))
TRAINING_EXEC = $(BUILD_DIR)/training

# Default target - build engine only
all: $(BUILD_DIR) $(ENGINE_EXEC)

# Training target
training: $(BUILD_DIR) $(TRAINING_EXEC)

# Both targets
both: all training

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(ENGINE_EXEC): $(ENGINE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

$(TRAINING_EXEC): $(TRAINING_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

debug: CFLAGS = $(DEBUG_FLAGS)
debug: clean all

debug_eval: CFLAGS = $(DEBUG_EVAL_FLAGS)
debug_eval: clean all

.PHONY: all training both clean debug debug_eval
