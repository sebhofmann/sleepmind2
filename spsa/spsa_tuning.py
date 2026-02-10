#!/usr/bin/env python3
"""
SPSA Tuning Script for SleepMind Chess Engine (Fishtest/OpenBench Style)

This implements the SPSA variant used by Fishtest and OpenBench:
1. Perturb all parameters simultaneously with random ±c direction
2. Play games: θ+ vs θ- directly (not against baseline)
3. Update: θ += r * (score - 0.5) * 2 * direction

This is simpler and more effective for chess engine tuning than classical SPSA.
"""

import time
import random
import math
import json
import os
import sys
import logging
from typing import Dict, List
from concurrent.futures import ThreadPoolExecutor, as_completed
import chess
import chess.engine
import chess.pgn

# =============================================================================
# Configuration
# =============================================================================

ENGINE_PATH = "../build/sleepmind"
OPENING_BOOK_PATH = "/home/paschty/Downloads/klo_eco_a00-e97v/klo_250_eco_a00-e97_variations.pgn"

NUM_GAMES = 128  # Games per iteration (θ+ vs θ-)
TIME_PER_MOVE_MS = 200
MAX_ITERATIONS = 300
SAVE_INTERVAL = 5
MAX_PARALLEL_GAMES = 32

logging.getLogger("chess.engine").setLevel(logging.ERROR)


# =============================================================================
# Parameter Definitions (Fishtest/OpenBench Style)
# =============================================================================
# Format: name -> {default, min, max, c, r}
#   c = perturbation size (how far to deviate for testing)
#   r = step size (how much to move based on result)
#
# Guidelines from Fishtest:
#   c ≈ (max - min) / 20  (5% of range)
#   r ≈ c / 4             (smaller steps for stability)
# =============================================================================

PARAMETERS = {
    # LMR parameters (range ~10)
    "LMR_FullDepthMoves": {
        "default": 3, "min": 1, "max": 10,
        "c": 1, "r": 0.5
    },
    "LMR_ReductionLimit": {
        "default": 2, "min": 1, "max": 6,
        "c": 1, "r": 0.5
    },

    # Null Move parameters (range ~5)
    "NullMove_Reduction": {
        "default": 3, "min": 1, "max": 5,
        "c": 1, "r": 0.5
    },
    "NullMove_MinDepth": {
        "default": 3, "min": 1, "max": 6,
        "c": 1, "r": 0.5
    },

    # Futility parameters (range ~350-650)
    "Futility_Margin": {
        "default": 150, "min": 50, "max": 400,
        "c": 20, "r": 8
    },
    "Futility_MarginD2": {
        "default": 300, "min": 100, "max": 600,
        "c": 30, "r": 10
    },
    "Futility_MarginD3": {
        "default": 450, "min": 150, "max": 800,
        "c": 40, "r": 12
    },

    # RFP parameters
    "RFP_Margin": {
        "default": 80, "min": 50, "max": 300,
        "c": 15, "r": 5
    },
    "RFP_MaxDepth": {
        "default": 8, "min": 2, "max": 10,
        "c": 1, "r": 0.5
    },

    # Aspiration parameters
    "Aspiration_Window": {
        "default": 100, "min": 10, "max": 200,
        "c": 10, "r": 4
    },

    # Razoring parameters
    "Razor_Margin": {
        "default": 300, "min": 100, "max": 600,
        "c": 25, "r": 8
    },
}


# =============================================================================
# Opening Book
# =============================================================================

class OpeningBook:
    def __init__(self, pgn_path: str):
        self.openings = []
        self.load_openings(pgn_path)

    def load_openings(self, pgn_path: str):
        if not os.path.exists(pgn_path):
            print(f"Warning: Opening book not found at {pgn_path}")
            return

        print(f"Loading opening book from {pgn_path}...")
        with open(pgn_path, 'r', encoding='utf-8', errors='ignore') as pgn_file:
            while True:
                game = chess.pgn.read_game(pgn_file)
                if game is None:
                    break

                moves = list(game.mainline_moves())
                if len(moves) >= 4:
                    self.openings.append(moves)

        print(f"Loaded {len(self.openings)} openings")

    def get_opening(self, max_moves: int = 8) -> List[chess.Move]:
        if not self.openings:
            return []

        opening = random.choice(self.openings)
        n = min(len(opening), max_moves)
        if n % 2 == 1:
            n -= 1
        return opening[:n]

    def get_openings(self, count: int) -> List[List[chess.Move]]:
        return [self.get_opening() for _ in range(count)]


# =============================================================================
# SPSA Tuner (Fishtest Style)
# =============================================================================

class SPSATuner:
    def __init__(self, tune_only=None):
        # Store params as floats internally for smooth updates
        self.params = {name: float(info["default"]) for name, info in PARAMETERS.items()}
        self.best_params = self.get_int_params()
        self.iteration = 0
        self.history = []
        self.opening_book = OpeningBook(OPENING_BOOK_PATH)
        # Which parameters to actually tune (None = all)
        self.tune_only = tune_only

        self.load_state()

    def get_int_params(self) -> Dict[str, int]:
        """Convert float params to integers for engine"""
        return {name: self.clamp_param(name, round(self.params[name])) for name in self.params}

    def clamp_param(self, name: str, value: int) -> int:
        info = PARAMETERS[name]
        return max(info["min"], min(info["max"], value))

    def load_state(self):
        if os.path.exists("spsa_state.json"):
            with open("spsa_state.json", "r") as f:
                state = json.load(f)
                self.params = {k: float(v) for k, v in state.get("params", self.params).items()}
                self.best_params = state.get("best_params", self.best_params)
                self.iteration = state.get("iteration", 0)
            print(f"Resumed from iteration {self.iteration}")

        if os.path.exists("spsa_history.json"):
            with open("spsa_history.json", "r") as f:
                self.history = json.load(f)

    def save_state(self):
        state = {
            "params": self.params,
            "best_params": self.best_params,
            "iteration": self.iteration
        }
        with open("spsa_state.json", "w") as f:
            json.dump(state, f, indent=2)

        with open("spsa_history.json", "w") as f:
            json.dump(self.history, f, indent=2)

    def get_perturbation(self) -> Dict[str, int]:
        """Generate ±1 direction for each tuned parameter, 0 for fixed ones"""
        return {
            name: random.choice([-1, 1]) if self.tune_only is None or name in self.tune_only else 0
            for name in PARAMETERS
        }

    def create_theta_plus_minus(self, direction: Dict[str, int]) -> tuple:
        """Create θ+ and θ- parameter sets"""
        theta_plus = {}
        theta_minus = {}

        for name, d in direction.items():
            c = PARAMETERS[name]["c"]
            base = round(self.params[name])

            plus_val = self.clamp_param(name, base + c * d)
            minus_val = self.clamp_param(name, base - c * d)

            theta_plus[name] = plus_val
            theta_minus[name] = minus_val

        return theta_plus, theta_minus

    def play_match(self, params_a: Dict[str, int], params_b: Dict[str, int],
                   openings: List[List[chess.Move]]) -> float:
        """Play games between A and B. Returns score for A (0 to 1)."""
        wins_a = 0
        wins_b = 0
        draws = 0

        # Each opening played twice (color swap)
        game_configs = []
        for i, opening in enumerate(openings):
            # Game 1: A plays White
            game_configs.append({
                "game_num": i * 2,
                "white_params": params_a,
                "black_params": params_b,
                "a_is_white": True,
                "opening": opening
            })
            # Game 2: A plays Black
            game_configs.append({
                "game_num": i * 2 + 1,
                "white_params": params_b,
                "black_params": params_a,
                "a_is_white": False,
                "opening": opening
            })

        with ThreadPoolExecutor(max_workers=MAX_PARALLEL_GAMES) as executor:
            futures = {
                executor.submit(
                    self.play_game,
                    cfg["white_params"],
                    cfg["black_params"],
                    cfg["opening"]
                ): cfg
                for cfg in game_configs
            }

            for future in as_completed(futures):
                cfg = futures[future]
                a_is_white = cfg["a_is_white"]

                try:
                    result = future.result()  # 1=white wins, -1=black wins, 0=draw

                    if result == 1:  # White won
                        if a_is_white:
                            wins_a += 1
                        else:
                            wins_b += 1
                    elif result == -1:  # Black won
                        if a_is_white:
                            wins_b += 1
                        else:
                            wins_a += 1
                    else:
                        draws += 1

                except Exception as e:
                    draws += 1

        total = len(game_configs)
        score = (wins_a + 0.5 * draws) / total
        print(f"  Result: +{wins_a} -{wins_b} ={draws} | θ+ score: {score:.3f}")
        return score

    def play_game(self, white_params: Dict[str, int], black_params: Dict[str, int],
                  opening: List[chess.Move]) -> int:
        """Play single game. Returns 1=white wins, -1=black wins, 0=draw"""
        engine_w = None
        engine_b = None

        try:
            engine_w = chess.engine.SimpleEngine.popen_uci(ENGINE_PATH)
            engine_b = chess.engine.SimpleEngine.popen_uci(ENGINE_PATH)

            for name, val in white_params.items():
                try:
                    engine_w.configure({name: val})
                except:
                    pass
            for name, val in black_params.items():
                try:
                    engine_b.configure({name: val})
                except:
                    pass

            board = chess.Board()
            for move in opening:
                if move in board.legal_moves:
                    board.push(move)

            while not board.is_game_over():
                engine = engine_w if board.turn == chess.WHITE else engine_b
                result = engine.play(board, chess.engine.Limit(time=TIME_PER_MOVE_MS / 1000))
                board.push(result.move)

            if board.is_checkmate():
                return -1 if board.turn == chess.WHITE else 1
            return 0

        except:
            return 0

        finally:
            for eng in [engine_w, engine_b]:
                if eng:
                    try:
                        eng.quit()
                    except:
                        pass

    def run_iteration(self):
        """Run one SPSA iteration (Fishtest style)"""
        self.iteration += 1

        print(f"\n{'=' * 60}")
        print(f"Iteration {self.iteration}")
        print(f"{'=' * 60}")

        # Generate perturbation direction
        direction = self.get_perturbation()

        # Create θ+ and θ-
        theta_plus, theta_minus = self.create_theta_plus_minus(direction)

        # Show what we're testing
        print("\nTesting θ+ vs θ-:")
        for name in PARAMETERS:
            cur = round(self.params[name])
            print(f"  {name}: {theta_minus[name]} vs {theta_plus[name]}  (current: {cur})")

        # Get openings (same for both color swaps)
        num_pairs = NUM_GAMES // 2
        openings = self.opening_book.get_openings(num_pairs)

        # Play θ+ vs θ- directly
        print(f"\nPlaying {NUM_GAMES} games (θ+ vs θ-)...")
        score = self.play_match(theta_plus, theta_minus, openings)

        # =================================================================
        # Fishtest-style update:
        #   θ += r * (score - 0.5) * 2 * direction
        #
        # If score > 0.5: θ+ won → move toward θ+ (same direction as perturbation)
        # If score < 0.5: θ- won → move toward θ- (opposite direction)
        # If score = 0.5: draw → no change
        #
        # The factor (score - 0.5) * 2 maps [0,1] to [-1,1]
        # =================================================================

        gradient = (score - 0.5) * 2  # Range: -1 to +1

        print(f"\nUpdating parameters (gradient: {gradient:+.3f}):")

        for name in PARAMETERS:
            r = PARAMETERS[name]["r"]
            d = direction[name]

            update = r * gradient * d
            old_val = self.params[name]
            new_val = old_val + update

            # Clamp to bounds
            info = PARAMETERS[name]
            new_val = max(float(info["min"]), min(float(info["max"]), new_val))
            self.params[name] = new_val

            if abs(update) > 0.01:
                print(f"  {name}: {old_val:.2f} -> {new_val:.2f} ({update:+.2f})")

        # Calculate Elo estimate
        elo = 0
        if 0 < score < 1:
            elo = -400 * math.log10(1 / score - 1)

        # Record history
        self.history.append({
            "iteration": self.iteration,
            "score": score,
            "elo_diff": round(elo, 1),
            "params": self.get_int_params()
        })

        # Update best if θ+ was clearly better
        if score > 0.53:
            self.best_params = theta_plus.copy()
            print(f"\n*** New best: θ+ won with {score:.1%} ***")
        elif score < 0.47:
            self.best_params = theta_minus.copy()
            print(f"\n*** New best: θ- won with {1-score:.1%} ***")

        # Print current state
        print(f"\nCurrent parameters (internal float values):")
        int_params = self.get_int_params()
        for name in PARAMETERS:
            val = int_params[name]
            fval = self.params[name]
            diff = val - PARAMETERS[name]["default"]
            sign = "+" if diff > 0 else ""
            print(f"  {name}: {val} (float: {fval:.2f}, {sign}{diff} from default)")

        print(f"\nθ+ vs θ- Elo: {elo:+.1f}")

        if self.iteration % SAVE_INTERVAL == 0:
            self.save_state()
            print("State saved.")


def main():
    # Parse optional parameter names from command line
    tune_only = None
    if len(sys.argv) > 1:
        tune_only = []
        for arg in sys.argv[1:]:
            if arg in PARAMETERS:
                tune_only.append(arg)
            else:
                print(f"Error: Unknown parameter '{arg}'")
                print(f"Available parameters: {', '.join(PARAMETERS.keys())}")
                sys.exit(1)

    print("=" * 60)
    print("SleepMind SPSA Tuning (Fishtest/OpenBench Style)")
    print("=" * 60)
    print()
    if tune_only:
        print(f"Tuning only: {', '.join(tune_only)}")
        print(f"Other parameters fixed at defaults.")
    else:
        print("Tuning all parameters.")
    print(f"Games per iteration: {NUM_GAMES}")
    print(f"Time per move: {TIME_PER_MOVE_MS}ms")
    print(f"Max parallel games: {MAX_PARALLEL_GAMES}")
    print()

    if not os.path.exists(ENGINE_PATH):
        print(f"Error: Engine not found at {ENGINE_PATH}")
        sys.exit(1)

    tuner = SPSATuner(tune_only=tune_only)

    try:
        while tuner.iteration < MAX_ITERATIONS:
            tuner.run_iteration()

    except KeyboardInterrupt:
        print("\n\nInterrupted.")

    tuner.save_state()

    print("\n" + "=" * 60)
    print("Final Results")
    print("=" * 60)
    print("\nBest parameters:")
    for name, val in tuner.best_params.items():
        diff = val - PARAMETERS[name]["default"]
        sign = "+" if diff > 0 else ""
        print(f"  {name}: {val} ({sign}{diff})")

    with open("best_params.json", "w") as f:
        json.dump(tuner.best_params, f, indent=2)
    print("\nSaved to best_params.json")


if __name__ == "__main__":
    main()
