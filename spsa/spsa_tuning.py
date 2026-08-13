#!/usr/bin/env python3
"""
SPSA Tuning Script for SleepMind Chess Engine (Fishtest/OpenBench Style)

This implements the SPSA variant used by Fishtest and OpenBench:
1. Perturb all parameters simultaneously with random ±c direction
2. Play games: θ+ vs θ- directly (not against baseline)
3. Update: θ += r * (score - 0.5) * 2 * direction

This is simpler and more effective for chess engine tuning than classical SPSA.
"""

import random
import json
import os
import logging
import threading
import argparse
from pathlib import Path
from typing import Dict, List, Optional
from concurrent.futures import ThreadPoolExecutor, as_completed
import chess
import chess.engine
import chess.pgn

# =============================================================================
# Configuration
# =============================================================================

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent
ENGINE_PATH = os.environ.get("SPSA_ENGINE", str(PROJECT_DIR / "build" / "sleepmind"))
OPENING_BOOK_PATH = os.environ.get(
    "SPSA_OPENINGS", "/home/paschty/Downloads/2moves_v2.pgn"
)
STATE_DIR = Path(os.environ.get("SPSA_STATE_DIR", str(SCRIPT_DIR))).resolve()

TIME_PER_MOVE_MS = int(os.environ.get("SPSA_MOVETIME_MS", "1000"))
MAX_ITERATIONS = int(os.environ.get("SPSA_ITERATIONS", "36000"))
SAVE_INTERVAL = int(os.environ.get("SPSA_SAVE_INTERVAL", "50"))
PRINT_INTERVAL = int(os.environ.get("SPSA_PRINT_INTERVAL", "50"))
MAX_PARALLEL_PAIRS = int(os.environ.get("SPSA_PARALLEL_PAIRS", "24"))

# Spall decay schedules (fishtest-style): the c/r values in PARAMETERS are
# END values; early iterations use larger perturbations and learning rates.
#   c_k = c_end * (MAX_ITERATIONS / k)^GAMMA
#   r_k = r_end * ((A + MAX_ITERATIONS) / (A + k))^ALPHA
SPSA_ALPHA = 0.602
SPSA_GAMMA = 0.101
SPSA_A = MAX_ITERATIONS // 10  # stabilization constant

# Updates happen per pair (2 games), but the r values below follow the c/4
# heuristic calibrated for ~24-game batches. Scale per-pair updates so the
# drift and noise per *game* stay the same as with 24-game batches.
R_PAIR_SCALE = 2.0 / 24.0

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

PARAMETER_OVERRIDES = {
    # LMR parameters (range ~10)
    "LMR_FullDepthMoves": {
        "default": 3, "min": 1, "max": 10,
        "c": 1, "r": 0.5
    },
    "LMR_ReductionLimit": {
        "default": 2, "min": 1, "max": 6,
        "c": 1, "r": 0.5
    },

    # Late Move Pruning parameters (skip quiets after base + depth^2 moves)
    "LMP_Base": {
        "default": 3, "min": 1, "max": 20,
        "c": 1, "r": 0.5
    },
    "LMP_MaxDepth": {
        "default": 8, "min": 1, "max": 12,
        "c": 1, "r": 0.5
    },

    # Null Move parameters (reduction is adaptive in-engine, not tunable)
    "NullMove_MinDepth": {
        "default": 3, "min": 1, "max": 6,
        "c": 1, "r": 0.5
    },

    # Futility parameters (range ~350-650)
    "Futility_Margin": {
        "default": 69, "min": 50, "max": 400,
        "c": 20, "r": 8
    },
    "Futility_MarginD2": {
        "default": 253, "min": 100, "max": 600,
        "c": 30, "r": 10
    },
    "Futility_MarginD3": {
        "default": 269, "min": 150, "max": 800,
        "c": 40, "r": 12
    },

    # RFP parameters
    "RFP_Margin": {
        "default": 93, "min": 50, "max": 300,
        "c": 15, "r": 5
    },
    "RFP_MaxDepth": {
        "default": 6, "min": 2, "max": 10,
        "c": 1, "r": 0.5
    },

    # Aspiration parameters
    "Aspiration_Window": {
        "default": 83, "min": 10, "max": 200,
        "c": 10, "r": 4
    },

    # Razoring parameters
    "Razor_Margin": {
        "default": 220, "min": 100, "max": 600,
        "c": 25, "r": 8
    },

    # History update scale (fix_7: butterfly + continuation history)
    "Hist_BonusMult": {
        "default": 518, "min": 50, "max": 800,
        "c": 35, "r": 9
    },
    "Hist_BonusSub": {
        "default": 199, "min": 0, "max": 1000,
        "c": 50, "r": 12
    },
    "Hist_BonusMax": {
        "default": 4325, "min": 500, "max": 16000,
        "c": 600, "r": 150
    },
    "Hist_MalusMult": {
        "default": 1167, "min": 100, "max": 4000,
        "c": 200, "r": 50
    },
    "Hist_MalusSub": {
        "default": 780, "min": 0, "max": 2000,
        "c": 100, "r": 25
    },
    "Hist_MalusMax": {
        "default": 3846, "min": 500, "max": 16000,
        "c": 600, "r": 150
    },
    "FMH_Weight": {
        "default": 166, "min": 0, "max": 256,
        "c": 10, "r": 3
    },

    # LMR thresholds on the combined quiet ordering score (fix_7)
    "LMR_StatLow2": {
        "default": -25544, "min": -49000, "max": 0,
        "c": 2400, "r": 600
    },
    "LMR_StatLow1": {
        "default": -10365, "min": -49000, "max": 0,
        "c": 2400, "r": 600
    },
    "LMR_StatHigh1": {
        "default": 21295, "min": 0, "max": 49000,
        "c": 2400, "r": 600
    },
    "LMR_StatHigh2": {
        "default": 23868, "min": 0, "max": 49000,
        "c": 2400, "r": 600
    },
}

# Operational spin options are valid UCI options, but not search parameters.
# Every other spin option advertised by the engine is picked up automatically.
SPSA_EXCLUDED_OPTIONS = {"Hash", "Threads", "MultiPV", "SyzygyProbeLimit"}


def discover_parameters(engine_path: str) -> Dict[str, dict]:
    """Build SPSA metadata from the engine's live UCI option declaration."""
    engine = chess.engine.SimpleEngine.popen_uci(engine_path)
    try:
        discovered = {}
        for name, option in engine.options.items():
            if option.type != "spin" or name in SPSA_EXCLUDED_OPTIONS:
                continue
            if option.default is None or option.min is None or option.max is None:
                continue
            span = option.max - option.min
            automatic = {
                "default": int(option.default),
                "min": int(option.min),
                "max": int(option.max),
                "c": max(1, round(span / 20)),
                "r": max(0.5, round(span / 80, 2)),
            }
            # Keep deliberately calibrated c/r values, but always trust the
            # running engine for default and legal range.
            override = PARAMETER_OVERRIDES.get(name, {})
            automatic.update({k: override[k] for k in ("c", "r") if k in override})
            discovered[name] = automatic
        return discovered
    finally:
        engine.quit()


PARAMETERS: Dict[str, dict] = {}


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
    def __init__(self, parameters: Dict[str, dict], tune_only=None,
                 state_dir: Optional[Path] = None):
        global PARAMETERS
        PARAMETERS = parameters
        self.state_dir = state_dir or SCRIPT_DIR
        self.state_dir.mkdir(parents=True, exist_ok=True)
        # Store params as floats internally for smooth updates
        self.params = {name: float(info["default"]) for name, info in PARAMETERS.items()}
        self.best_params = self.get_int_params()
        self.iteration = 0
        self.history = []
        self.opening_book = OpeningBook(OPENING_BOOK_PATH)
        # Which parameters to actually tune (None = all)
        self.tune_only = tune_only

        with (self.state_dir / "spsa_parameters.json").open("w") as f:
            json.dump(PARAMETERS, f, indent=2)

        self.load_state()

    def get_int_params(self) -> Dict[str, int]:
        """Convert float params to integers for engine"""
        return {name: self.clamp_param(name, round(self.params[name])) for name in self.params}

    def clamp_param(self, name: str, value: int) -> int:
        info = PARAMETERS[name]
        return max(info["min"], min(info["max"], value))

    def load_state(self):
        state_path = self.state_dir / "spsa_state.json"
        history_path = self.state_dir / "spsa_history.json"
        if state_path.exists():
            with state_path.open("r") as f:
                state = json.load(f)
                # Merge: keep defaults for params added after the state was saved
                for k, v in state.get("params", {}).items():
                    if k in self.params:
                        self.params[k] = float(v)
                self.best_params = state.get("best_params", self.best_params)
                self.iteration = state.get("iteration", 0)
            print(f"Resumed from iteration {self.iteration}")

        if history_path.exists():
            with history_path.open("r") as f:
                self.history = json.load(f)

    def save_state(self):
        state = {
            "params": self.params,
            "best_params": self.best_params,
            "iteration": self.iteration
        }
        with (self.state_dir / "spsa_state.json").open("w") as f:
            json.dump(state, f, indent=2)

        with (self.state_dir / "spsa_history.json").open("w") as f:
            json.dump(self.history, f, indent=2)

    def get_perturbation(self) -> Dict[str, int]:
        """Generate ±1 direction for each tuned parameter, 0 for fixed ones"""
        return {
            name: random.choice([-1, 1]) if self.tune_only is None or name in self.tune_only else 0
            for name in PARAMETERS
        }

    def current_c(self, name: str) -> float:
        """Perturbation size at this iteration (decays toward PARAMETERS c)"""
        k = max(1, self.iteration + 1)
        return PARAMETERS[name]["c"] * (MAX_ITERATIONS / k) ** SPSA_GAMMA

    def current_r(self, name: str) -> float:
        """Learning rate at this iteration (decays toward PARAMETERS r)"""
        k = max(1, self.iteration + 1)
        return PARAMETERS[name]["r"] * ((SPSA_A + MAX_ITERATIONS) / (SPSA_A + k)) ** SPSA_ALPHA

    def create_theta_plus_minus(self, direction: Dict[str, int]) -> tuple:
        """Create θ+ and θ- parameter sets"""
        theta_plus = {}
        theta_minus = {}

        for name, d in direction.items():
            c = round(self.current_c(name))
            base = round(self.params[name])

            plus_val = self.clamp_param(name, base + c * d)
            minus_val = self.clamp_param(name, base - c * d)

            theta_plus[name] = plus_val
            theta_minus[name] = minus_val

        return theta_plus, theta_minus

    def play_pair(self, theta_plus: Dict[str, int], theta_minus: Dict[str, int],
                  opening: List[chess.Move]) -> float:
        """Play one opening pair (color swap), sequentially on this worker slot.
        Returns score for θ+ in [0, 1]."""
        engine_w = None
        engine_b = None
        try:
            engine_w = chess.engine.SimpleEngine.popen_uci(ENGINE_PATH)
            engine_b = chess.engine.SimpleEngine.popen_uci(ENGINE_PATH)
            r1 = self.play_game(engine_w, engine_b, theta_plus, theta_minus, opening)
            r2 = self.play_game(engine_w, engine_b, theta_minus, theta_plus, opening)
            return ((r1 + 1) / 2 + (1 - r2) / 2) / 2
        finally:
            for engine in (engine_w, engine_b):
                if engine is None:
                    continue
                try:
                    engine.quit()
                except (chess.engine.EngineError, TimeoutError, OSError):
                    pass

    def play_game(self, engine_w, engine_b, white_params: Dict[str, int], black_params: Dict[str, int],
                  opening: List[chess.Move]) -> int:
        """Play single game. Returns 1=white wins, -1=black wins, 0=draw"""
        engine_w.configure(white_params)
        engine_b.configure(black_params)

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

    def apply_update(self, direction: Dict[str, int], score: float):
        """Fishtest-style update from one pair result. Caller holds the lock.
        θ += r * (score - 0.5) * 2 * direction, r scaled to pair size."""
        gradient = (score - 0.5) * 2  # Range: -1 to +1

        for name in PARAMETERS:
            d = direction[name]
            if d == 0:
                continue
            r = self.current_r(name) * R_PAIR_SCALE
            info = PARAMETERS[name]
            new_val = self.params[name] + r * gradient * d
            self.params[name] = max(float(info["min"]), min(float(info["max"]), new_val))

    def worker(self):
        """One asynchronous SPSA step: snapshot θ±c, play a pair, update θ."""
        with self.lock:
            if self.iteration >= MAX_ITERATIONS:
                return
            direction = self.get_perturbation()
            theta_plus, theta_minus = self.create_theta_plus_minus(direction)
        opening = self.opening_book.get_opening()

        score = self.play_pair(theta_plus, theta_minus, opening)

        with self.lock:
            self.apply_update(direction, score)
            self.iteration += 1
            it = self.iteration

            self.history.append({
                "iteration": it,
                "score": score,
                "params": self.get_int_params()
            })

            print(f"[{it}/{MAX_ITERATIONS}] pair score θ+: {score:.2f}")

            if it % PRINT_INTERVAL == 0:
                print(f"\n--- Parameters after {it} pairs ({it * 2} games) ---")
                int_params = self.get_int_params()
                for name in PARAMETERS:
                    diff = int_params[name] - PARAMETERS[name]["default"]
                    sign = "+" if diff > 0 else ""
                    print(f"  {name}: {int_params[name]} ({sign}{diff} from default)")
                print()

            if it % SAVE_INTERVAL == 0:
                self.save_state()

    def run_async(self):
        """Continuous fishtest-style loop: MAX_PARALLEL_PAIRS worker slots,
        each plays one opening pair and applies its update immediately —
        no iteration barrier, no idle cores waiting for stragglers."""
        self.lock = threading.Lock()
        remaining = MAX_ITERATIONS - self.iteration
        if remaining <= 0:
            print("MAX_ITERATIONS already reached.")
            return

        executor = ThreadPoolExecutor(max_workers=MAX_PARALLEL_PAIRS)
        futures = [executor.submit(self.worker) for _ in range(remaining)]
        try:
            for future in as_completed(futures):
                exc = future.exception()
                if exc is not None:
                    print(f"Worker error: {exc}")
        except KeyboardInterrupt:
            print("\nInterrupted - cancelling pending pairs, waiting for running games...")
            executor.shutdown(wait=True, cancel_futures=True)
            raise
        executor.shutdown()


def main():
    parser = argparse.ArgumentParser(description="Tune SleepMind UCI spin options with SPSA")
    parser.add_argument("parameters", nargs="*", help="only tune these UCI options")
    parser.add_argument("--list", action="store_true", help="list discovered tunable options")
    parser.add_argument("--reset", action="store_true", help="ignore existing SPSA state")
    args = parser.parse_args()

    global PARAMETERS
    try:
        PARAMETERS = discover_parameters(ENGINE_PATH)
    except (chess.engine.EngineError, FileNotFoundError, PermissionError, OSError) as exc:
        parser.error(f"cannot query UCI engine {ENGINE_PATH}: {exc}")
    if not PARAMETERS:
        parser.error("engine advertises no tunable UCI spin options")

    if args.list:
        for name, info in PARAMETERS.items():
            print(f"{name}: default={info['default']} min={info['min']} "
                  f"max={info['max']} c={info['c']} r={info['r']}")
        return

    unknown = sorted(set(args.parameters) - set(PARAMETERS))
    if unknown:
        parser.error(f"unknown/non-tunable option(s): {', '.join(unknown)}")
    tune_only = args.parameters or None

    if args.reset:
        for filename in ("spsa_state.json", "spsa_history.json"):
            path = STATE_DIR / filename
            if path.exists():
                path.rename(path.with_suffix(path.suffix + ".bak"))

    print("=" * 60)
    print("SleepMind SPSA Tuning (Fishtest/OpenBench Style)")
    print("=" * 60)
    print()
    if tune_only:
        print(f"Tuning only: {', '.join(tune_only)}")
        print(f"Other parameters fixed at defaults.")
    else:
        print("Tuning all parameters.")
    print(f"Update granularity: 1 opening pair (2 games), asynchronous")
    print(f"Max pairs: {MAX_ITERATIONS} ({MAX_ITERATIONS * 2} games)")
    print(f"Time per move: {TIME_PER_MOVE_MS}ms")
    print(f"Discovered tunable UCI options: {len(PARAMETERS)}")
    print(f"Parallel pair slots: {MAX_PARALLEL_PAIRS}")
    print()

    if not os.path.exists(ENGINE_PATH):
        print(f"Error: Engine not found at {ENGINE_PATH}")
        sys.exit(1)

    tuner = SPSATuner(PARAMETERS, tune_only=tune_only, state_dir=STATE_DIR)

    try:
        tuner.run_async()
    except KeyboardInterrupt:
        print("\n\nInterrupted.")

    tuner.save_state()

    print("\n" + "=" * 60)
    print("Final Results")
    print("=" * 60)
    print("\nFinal parameters (converged θ):")
    final_params = tuner.get_int_params()
    for name, val in final_params.items():
        diff = val - PARAMETERS[name]["default"]
        sign = "+" if diff > 0 else ""
        print(f"  {name}: {val} ({sign}{diff})")

    with (STATE_DIR / "best_params.json").open("w") as f:
        json.dump(final_params, f, indent=2)
    print("\nSaved to best_params.json")


if __name__ == "__main__":
    main()
