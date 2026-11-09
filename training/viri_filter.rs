//! Viriformat position filter and loader settings, shared by training.rs and
//! training_deep.rs.

use bullet::value::loader::viribinpack::{Board, Filter, Move, WDL};
use std::cell::{Cell, RefCell};

// =============================================================================
// Data loading
// =============================================================================
//
// Two kinds of input, usable together (see mixed_loader.rs):
//   --data  bulletformat files (legacy data, already filtered when generated;
//           the old file must first go through datatools/filter_material)
//   --viri  viriformat binpacks of whole games (new generator). All recorded
//           positions are stored, the filter below picks the training positions
//           every time a game is unpacked.

// Viriformat loader filter, after bullet's examples/advanced/filter.rs.
// The generator records from the first position after the random opening
// (ply = RANDOM_MOVES = 12), so MIN_PLY 12 keeps every recorded position.
const FILTER_MIN_PLY: u32 = 12;
const FILTER_MIN_PIECES: u32 = 4;
// Drops |score| >= 10000, which includes the mate/TB scores (stored as +/-32000).
const FILTER_MAX_EVAL: u32 = 10000;
// Drops draws with |score| > 2500 and positions where the eventual winner's
// score is below -2500.
const FILTER_MAX_EVAL_INCORRECTNESS: u32 = 2500;
const FILTER_CHECK: bool = true;
// Played move is a capture, promotion or en passant.
const FILTER_TACTICAL: bool = true;
const FILTER_CASTLING: bool = true;
// Randomly skip positions (re-drawn every epoch), 0.0 disables.
const FILTER_RANDOM_SKIP_PROBABILITY: f64 = 0.15;
// Resample towards the piece-count distribution of the bullet example. This
// discards at least half of the positions, so it is off by default.
const FILTER_PIECE_COUNT_DISTRIBUTION: bool = false;

const VIRI_FILTER: Filter = Filter {
    min_ply: FILTER_MIN_PLY,
    min_pieces: FILTER_MIN_PIECES,
    max_eval: FILTER_MAX_EVAL,
    filter_tactical: FILTER_TACTICAL,
    filter_check: FILTER_CHECK,
    filter_castling: FILTER_CASTLING,
    max_eval_incorrectness: FILTER_MAX_EVAL_INCORRECTNESS,
    random_fen_skipping: FILTER_RANDOM_SKIP_PROBABILITY > 0.0,
    random_fen_skip_probability: FILTER_RANDOM_SKIP_PROBABILITY,
    wdl_filtered: false,
    wdl_model_params_a: [0.0; 4],
    wdl_model_params_b: [0.0; 4],
    material_min: 17,
    material_max: 78,
    mom_target: 58,
    wdl_heuristic_scale: 1.0,
};

// Loader buffer (shuffled per buffer) and unpacking threads for viriformat.
pub const VIRI_BUFFER_MB: usize = 1024;
pub const VIRI_THREADS: usize = 8;

fn piece_count_acceptance(board: &Board) -> f64 {
    #[rustfmt::skip]
    const DESIRED_DISTRIBUTION: [f64; 33] = [
        0.018411966423, 0.020641545085, 0.022727271053,
        0.024669162740, 0.026467201733, 0.028121406444,
        0.029631758462, 0.030998276198, 0.032220941240,
        0.033299772000, 0.034234750067, 0.035025893853,
        0.035673184944, 0.036176641754, 0.036536245870,
        0.036752015705, 0.036823932846, 0.036752015705,
        0.036536245870, 0.036176641754, 0.035673184944,
        0.035025893853, 0.034234750067, 0.033299772000,
        0.032220941240, 0.030998276198, 0.029631758462,
        0.028121406444, 0.026467201733, 0.024669162740,
        0.022727271053, 0.020641545085, 0.018411966423,
    ];

    thread_local! {
        static PIECE_COUNT_STATS: RefCell<[u64; 33]> = const { RefCell::new([0; 33]) };
        static PIECE_COUNT_TOTAL: Cell<u64> = const { Cell::new(0) };
    }

    let pc = board.pieces.occupied().count() as usize;
    let count = PIECE_COUNT_STATS.with_borrow_mut(|stats| {
        stats[pc] += 1;
        stats[pc]
    });
    let total = PIECE_COUNT_TOTAL.with(|t| {
        let total = t.get() + 1;
        t.set(total);
        total
    });
    let frequency = count as f64 / total as f64;
    (0.5 * DESIRED_DISTRIBUTION[pc] / frequency).clamp(0.0, 1.0)
}

/// Per-position filter for viriformat games. `eval` and `wdl` are White-relative
/// (viriformat convention).
pub fn should_keep(board: &Board, mv: Move, eval: i16, wdl: f32) -> bool {
    use rand::Rng;
    let mut rng = rand::rng();
    let wdl = match wdl {
        1.0 => WDL::Win,
        0.5 => WDL::Draw,
        _ => WDL::Loss,
    };
    !VIRI_FILTER.should_filter(mv, i32::from(eval), board, wdl, &mut rng)
        && (!FILTER_PIECE_COUNT_DISTRIBUTION || rng.random_bool(piece_count_acceptance(board)))
}
