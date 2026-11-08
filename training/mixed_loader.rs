//! Data loading for the SleepMind trainers: legacy bulletformat files and
//! viriformat binpacks, alone or mixed into the same batches.
//!
//! The viriformat games are read with bullet's `ViriBinpackLoader`, which
//! applies the per-position filter while unpacking (so it is re-drawn every
//! epoch). When both sources are given, every chunk handed to the trainer
//! consists of a fixed share of positions from each source, shuffled together.
//! By default the share is proportional to the number of positions each source
//! contributes per epoch, so both are seen equally often.

use std::{
    fs::File,
    io::{BufRead, BufReader, Cursor},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc, Arc,
    },
};

use bullet::{
    game::formats::bulletformat::ChessBoard,
    value::loader::{
        viribinpack::{Board, Game, Move, WDL},
        DirectSequentialDataLoader, ViriBinpackLoader, ViriFilter,
    },
};
use bullet_trainer::reader::DataReader;

pub type KeepFn = fn(&Board, Move, i16, f32) -> bool;

/// Positions per mixed chunk handed to the trainer.
const MIX_CHUNK: usize = 1 << 20;

#[derive(Clone)]
pub struct MixedLoader {
    bullet: Option<DirectSequentialDataLoader>,
    viri: Option<ViriBinpackLoader>,
    viri_fraction: f64,
}

impl MixedLoader {
    /// `viri_fraction`: share of viriformat positions per chunk when both
    /// sources are present (`None` = proportional to the source sizes).
    pub fn new(
        bullet_paths: &[String],
        viri_paths: &[String],
        keep: KeepFn,
        viri_buffer_mb: usize,
        viri_threads: usize,
        viri_fraction: Option<f64>,
    ) -> Self {
        assert!(!bullet_paths.is_empty() || !viri_paths.is_empty(), "no training data given");

        let bullet = (!bullet_paths.is_empty()).then(|| {
            let paths: Vec<&str> = bullet_paths.iter().map(String::as_str).collect();
            DirectSequentialDataLoader::new(&paths)
        });
        let viri = (!viri_paths.is_empty()).then(|| {
            for path in viri_paths {
                assert!(std::path::Path::new(path).exists(), "File not found: {path}");
            }
            let paths: Vec<&str> = viri_paths.iter().map(String::as_str).collect();
            ViriBinpackLoader::new_concat_multiple(&paths, viri_buffer_mb, viri_threads, ViriFilter::Custom(keep))
        });

        let bullet_positions: u64 = bullet_paths
            .iter()
            .map(|p| std::fs::metadata(p).map(|m| m.len()).unwrap_or(0) / std::mem::size_of::<ChessBoard>() as u64)
            .sum();
        if !bullet_paths.is_empty() {
            println!("bulletformat:  {bullet_positions} positions in {} file(s)", bullet_paths.len());
        }

        let mut fraction = if bullet.is_none() { 1.0 } else { 0.0 };
        if !viri_paths.is_empty() {
            let (moves, pass_rate) = estimate_viri_positions(viri_paths, keep, 50_000);
            let est = moves as f64 * pass_rate;
            println!(
                "viriformat:    {moves} recorded positions in {} file(s), filter keeps ~{:.1} % -> ~{:.0} per epoch",
                viri_paths.len(),
                100.0 * pass_rate,
                est
            );
            if bullet.is_some() {
                fraction = viri_fraction.unwrap_or(est / (est + bullet_positions as f64));
                println!(
                    "mixing:        {:.2} % viriformat / {:.2} % bulletformat per chunk ({})",
                    100.0 * fraction,
                    100.0 * (1.0 - fraction),
                    if viri_fraction.is_some() { "set by --viri-fraction" } else { "proportional to size" }
                );
            }
        }
        assert!((0.0..=1.0).contains(&fraction), "viri fraction must be within [0, 1]");

        Self { bullet, viri, viri_fraction: fraction }
    }
}

/// Counts the recorded positions of all games (cheap, no replay) and estimates
/// the filter pass rate from the first `sample_games` games.
pub fn estimate_viri_positions(paths: &[String], keep: KeepFn, sample_games: usize) -> (u64, f64) {
    // header (32 byte packed board) + 4 bytes per move + 4 byte terminator
    const HEADER: usize = 32;
    const MOVE: usize = 4;
    let mut moves = 0u64;
    let (mut sampled, mut sampled_pass, mut games) = (0u64, 0u64, 0usize);
    let mut buf = Vec::new();
    for path in paths {
        let mut reader = BufReader::with_capacity(1 << 20, File::open(path).unwrap());
        while !reader.fill_buf().unwrap().is_empty() {
            buf.clear();
            Game::deserialise_fast_into_buffer(&mut reader, &mut buf).unwrap();
            let n = (buf.len() - HEADER) / MOVE - 1;
            moves += n as u64;
            if games < sample_games {
                let game = Game::deserialise_from(&mut Cursor::new(&buf), Vec::new()).unwrap();
                sampled += n as u64;
                sampled_pass += game.filter_pass_count_with_filter_callback(|mv, eval, board, wdl, _| {
                    let wdl = match wdl {
                        WDL::Win => 1.0,
                        WDL::Draw => 0.5,
                        WDL::Loss => 0.0,
                    };
                    !keep(board, mv, eval as i16, wdl)
                });
            }
            games += 1;
        }
    }
    (moves, sampled_pass as f64 / sampled.max(1) as f64)
}

impl DataReader<ChessBoard> for MixedLoader {
    fn read_chunks<F: FnMut(&[ChessBoard]) -> bool>(&self, skip_count: usize, mut f: F) {
        match (&self.bullet, &self.viri) {
            (Some(bullet), None) => DataReader::<ChessBoard>::read_chunks(bullet, skip_count, f),
            (None, Some(viri)) => DataReader::<ChessBoard>::read_chunks(viri, skip_count, f),
            (Some(bullet), Some(viri)) => {
                let stop = Arc::new(AtomicBool::new(false));
                // Resuming: only the bulletformat stream can skip (the viri
                // loader always starts at the beginning of its files).
                let bullet_skip = (skip_count as f64 * (1.0 - self.viri_fraction)) as usize;
                let mut bullet = Source::spawn(bullet.clone(), bullet_skip, stop.clone());
                let mut viri = Source::spawn(viri.clone(), 0, stop.clone());

                let n_viri = (MIX_CHUNK as f64 * self.viri_fraction).round() as usize;
                let n_bullet = MIX_CHUNK - n_viri;
                let mut rng = XorShift(0x9E37_79B9_7F4A_7C15);
                let mut out = Vec::with_capacity(MIX_CHUNK);
                loop {
                    out.clear();
                    if !bullet.take(n_bullet, &mut out) || !viri.take(n_viri, &mut out) {
                        break;
                    }
                    rng.shuffle(&mut out);
                    if f(&out) {
                        break;
                    }
                }
                // Dropping the receivers makes the producers' next send fail,
                // which ends their read_chunks loops.
                stop.store(true, Ordering::Relaxed);
            }
            (None, None) => unreachable!(),
        }
    }
}

/// A data source read on its own thread, consumed in arbitrary-size pieces.
struct Source {
    rx: mpsc::Receiver<Vec<ChessBoard>>,
    buf: Vec<ChessBoard>,
    pos: usize,
}

impl Source {
    fn spawn<R: DataReader<ChessBoard>>(reader: R, skip: usize, stop: Arc<AtomicBool>) -> Self {
        let (tx, rx) = mpsc::sync_channel::<Vec<ChessBoard>>(2);
        std::thread::spawn(move || {
            reader.read_chunks(skip, |chunk: &[ChessBoard]| stop.load(Ordering::Relaxed) || tx.send(chunk.to_vec()).is_err());
        });
        Self { rx, buf: Vec::new(), pos: 0 }
    }

    /// Appends `n` positions to `out`; false if the source has ended.
    fn take(&mut self, mut n: usize, out: &mut Vec<ChessBoard>) -> bool {
        while n > 0 {
            if self.pos == self.buf.len() {
                match self.rx.recv() {
                    Ok(buf) => {
                        self.buf = buf;
                        self.pos = 0;
                    }
                    Err(_) => return false,
                }
            }
            let k = n.min(self.buf.len() - self.pos);
            out.extend_from_slice(&self.buf[self.pos..self.pos + k]);
            self.pos += k;
            n -= k;
        }
        true
    }
}

struct XorShift(u64);

impl XorShift {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    fn shuffle<T>(&mut self, data: &mut [T]) {
        for i in (1..data.len()).rev() {
            let j = (self.next() % (i as u64 + 1)) as usize;
            data.swap(i, j);
        }
    }
}
