CC = gcc
CFLAGS = -g -Wall -Wextra -std=c11 -O3 -march=native
DEBUG_FLAGS = -g -Wall -Wextra -std=c11 -O0 -march=native -DDEBUG_NNUE_INCREMENTAL
DEBUG_EVAL_FLAGS = -g -Wall -Wextra -std=c11 -O3 -march=native -DDEBUG_NNUE_EVAL

ifeq ($(shell uname -s),Darwin)
  CFLAGS += -D_DARWIN_C_SOURCE
  DEBUG_FLAGS += -D_DARWIN_C_SOURCE
  DEBUG_EVAL_FLAGS += -D_DARWIN_C_SOURCE
endif

# Optional flags: make STATS=1, or make hl_64 (NNUE_ARCH=legacy only), or make hl_256, or make hl_512, or make hl_768, or make hl_1024
ifeq ($(STATS),1)
  CFLAGS += -DSEARCH_STATS
endif

# make runtime_magics=1: search for the magic numbers at startup instead of using the built-in constants
ifeq ($(runtime_magics),1)
  CFLAGS += -DRUNTIME_MAGICS
endif

ifeq ($(hl_64),1)
  CFLAGS += -DNNUE_HIDDEN_SIZE=64
endif

ifeq ($(hl_256),1)
  CFLAGS += -DNNUE_HIDDEN_SIZE=256
endif

ifeq ($(hl_512),1)
  CFLAGS += -DNNUE_HIDDEN_SIZE=512
endif

ifeq ($(hl_768),1)
  CFLAGS += -DNNUE_HIDDEN_SIZE=768
endif

ifeq ($(hl_1024),1)
  CFLAGS += -DNNUE_HIDDEN_SIZE=1024
endif

# NNUE architecture: the default is the deep net (training/training_deep.rs),
# make NNUE_ARCH=threats builds deep + threat/pawn-pair inputs (training/training_threats.rs),
# make NNUE_ARCH=legacy builds the old single-layer net (training/training.rs). Each
# architecture embeds its own net file (quantised_deep.bin / quantised_threats.bin /
# quantised.bin); override with NNUE_FILE=<path>.
NNUE_ARCH ?= deep
ifeq ($(NNUE_ARCH),deep)
  NNUE_FILE ?= quantised_deep.bin
  NNUE_FLAGS = -DNNUE_ARCH=1
else ifeq ($(NNUE_ARCH),threats)
  NNUE_FILE ?= quantised_threats.bin
  NNUE_FLAGS = -DNNUE_ARCH=2
else ifeq ($(NNUE_ARCH),legacy)
  NNUE_FILE ?= quantised.bin
  NNUE_FLAGS = -DNNUE_ARCH=0
else
  $(error unknown NNUE_ARCH '$(NNUE_ARCH)', use legacy, deep or threats)
endif
NNUE_FLAGS += -DNNUE_EMBED_FILE='"$(NNUE_FILE)"'
CFLAGS += $(NNUE_FLAGS)
DEBUG_FLAGS += $(NNUE_FLAGS)
DEBUG_EVAL_FLAGS += $(NNUE_FLAGS)

# make debug / make debug_eval set BUILD_MODE for their inner make run
ifeq ($(BUILD_MODE),debug)
  CFLAGS = $(DEBUG_FLAGS)
else ifeq ($(BUILD_MODE),debug_eval)
  CFLAGS = $(DEBUG_EVAL_FLAGS)
endif

# Directories
SRC_DIR = src
BUILD_DIR = build

# The stamp rule below must not become the default goal
.DEFAULT_GOAL := all

# Rebuild everything when the NNUE configuration changes, so objects of different
# architectures never get mixed
NNUE_STAMP = $(BUILD_DIR)/.nnue_config
NNUE_CONFIG = $(NNUE_ARCH) $(NNUE_FILE) $(CFLAGS)
$(shell mkdir -p $(BUILD_DIR); [ "$$(cat $(NNUE_STAMP) 2>/dev/null)" = "$(NNUE_CONFIG)" ] || echo "$(NNUE_CONFIG)" > $(NNUE_STAMP))
$(NNUE_STAMP):
	@mkdir -p $(BUILD_DIR)
	@echo "$(NNUE_CONFIG)" > $@

# Common source files (shared between engine and training)
COMMON_SRCS = board_io.c move_generator.c move.c bitboard_utils.c search.c tt.c evaluation.c board_modifiers.c zobrist.c nnue.c nnue_threats.c syzygy.c tbprobe.c

# Engine source files
ENGINE_SRCS = main.c uci.c $(COMMON_SRCS)
ENGINE_OBJS = $(addprefix $(BUILD_DIR)/, $(ENGINE_SRCS:.c=.o))
ENGINE_OBJS += $(BUILD_DIR)/embedded_nnue.o
ENGINE_EXEC = $(BUILD_DIR)/sleepmind

# Training data generator source files
TRAINING_SRCS = training_main.c training_data.c $(COMMON_SRCS)
TRAINING_OBJS = $(addprefix $(BUILD_DIR)/, $(TRAINING_SRCS:.c=.o))
# Same embedded network as the engine (net file of the selected NNUE_ARCH at build time).
TRAINING_OBJS += $(BUILD_DIR)/embedded_nnue.o
TRAINING_EXEC = $(BUILD_DIR)/training

# Default target - build engine only
all: $(BUILD_DIR) $(ENGINE_EXEC)

# Training target
training: $(BUILD_DIR) $(TRAINING_EXEC)

# Both targets
both: all training

test-nnue: $(BUILD_DIR) $(BUILD_DIR)/nnue_load_test
	$(BUILD_DIR)/nnue_load_test

test-pawn-hash: $(BUILD_DIR) $(BUILD_DIR)/pawn_hash_test
	$(BUILD_DIR)/pawn_hash_test

test-pawn-correction: $(BUILD_DIR) $(BUILD_DIR)/pawn_correction_test
	$(BUILD_DIR)/pawn_correction_test

test-long-game-history: $(BUILD_DIR) $(BUILD_DIR)/long_game_history_test
	$(BUILD_DIR)/long_game_history_test

test: test-nnue test-pawn-hash test-pawn-correction test-long-game-history

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(ENGINE_EXEC): $(ENGINE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

$(TRAINING_EXEC): $(TRAINING_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

# Vendored Fathom probing code: needs POSIX (mmap) under -std=c11, and its
# warnings are not actionable for us, so they are suppressed.
$(BUILD_DIR)/tbprobe.o: $(SRC_DIR)/tbprobe.c $(NNUE_STAMP)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -w -MMD -MP -I$(SRC_DIR) -c $< -o $@

# Keep the release engine self-contained. The explicit dependency ensures that
# replacing the trained net always relinks the executable.
$(BUILD_DIR)/embedded_nnue.o: $(SRC_DIR)/embedded_nnue.S $(NNUE_FILE) $(NNUE_STAMP)
	$(CC) $(CFLAGS) -c $< -o $@

# nnue.o plus what the threat features need (magic attacks)
NNUE_TEST_OBJS = $(addprefix $(BUILD_DIR)/, nnue.o nnue_threats.o move_generator.o bitboard_utils.o board_modifiers.o zobrist.o move.o board_io.o)

$(BUILD_DIR)/nnue_load_test: tests/nnue_load_test.c $(NNUE_TEST_OBJS) $(BUILD_DIR)/embedded_nnue.o
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $^ -lm

$(BUILD_DIR)/pawn_hash_test: tests/pawn_hash_test.c $(NNUE_TEST_OBJS) $(BUILD_DIR)/embedded_nnue.o
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $^ -lm

$(BUILD_DIR)/long_game_history_test: tests/long_game_history_test.c $(NNUE_TEST_OBJS) $(BUILD_DIR)/embedded_nnue.o
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $^ -lm

$(BUILD_DIR)/pawn_correction_test: tests/pawn_correction_test.c
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(NNUE_STAMP)
	$(CC) $(CFLAGS) -MMD -MP -I$(SRC_DIR) -c $< -o $@

# Auto-generated header dependencies (so .h changes trigger rebuilds)
-include $(wildcard $(BUILD_DIR)/*.d)

clean:
	rm -rf $(BUILD_DIR)

# Separate make runs: "clean all" in one run is unreliable, since make caches the
# directory contents before clean removes them
debug:
	$(MAKE) clean
	$(MAKE) all BUILD_MODE=debug

debug_eval:
	$(MAKE) clean
	$(MAKE) all BUILD_MODE=debug_eval

.PHONY: all training both test test-nnue test-pawn-hash test-pawn-correction clean debug debug_eval
