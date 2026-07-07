CC = gcc
CFLAGS = -g -Wall -Wextra -std=c11 -O3 -march=native
DEBUG_FLAGS = -g -Wall -Wextra -std=c11 -O0 -march=native -DDEBUG_NNUE_INCREMENTAL
DEBUG_EVAL_FLAGS = -g -Wall -Wextra -std=c11 -O3 -march=native -DDEBUG_NNUE_EVAL

ifeq ($(shell uname -s),Darwin)
  CFLAGS += -D_DARWIN_C_SOURCE
  DEBUG_FLAGS += -D_DARWIN_C_SOURCE
  DEBUG_EVAL_FLAGS += -D_DARWIN_C_SOURCE
endif

# Optional flags: make STATS=1, or make hl_256, or make hl_768
ifeq ($(STATS),1)
  CFLAGS += -DSEARCH_STATS
endif

ifeq ($(hl_768),1)
  CFLAGS += -DNNUE_HIDDEN_SIZE=768
endif

# Directories
SRC_DIR = src
BUILD_DIR = build

# Common source files (shared between engine and training)
COMMON_SRCS = board_io.c move_generator.c move.c bitboard_utils.c search.c tt.c evaluation.c board_modifiers.c zobrist.c nnue.c syzygy.c tbprobe.c

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

# Vendored Fathom probing code: needs POSIX (mmap) under -std=c11, and its
# warnings are not actionable for us, so they are suppressed.
$(BUILD_DIR)/tbprobe.o: $(SRC_DIR)/tbprobe.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -w -MMD -MP -I$(SRC_DIR) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -MMD -MP -I$(SRC_DIR) -c $< -o $@

# Auto-generated header dependencies (so .h changes trigger rebuilds)
-include $(wildcard $(BUILD_DIR)/*.d)

clean:
	rm -rf $(BUILD_DIR)

debug: CFLAGS = $(DEBUG_FLAGS)
debug: clean all

debug_eval: CFLAGS = $(DEBUG_EVAL_FLAGS)
debug_eval: clean all

.PHONY: all training both clean debug debug_eval
