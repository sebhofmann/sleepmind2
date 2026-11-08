//! Position statistics for comparing generator outputs.
//!
//! usage:
//!   data_stats vf     <file.vf> ...          viriformat binpacks (new generator)
//!   data_stats games  <games.txt> ...        game text lines (new generator, unpacked)
//!   data_stats text   <data.txt> ...         old generator text "fen | score | wdl"
//!   data_stats bullet <data.dat> [n] [off]   bulletformat records (n positions from offset)
//!
//! For viriformat/game data two position sets are reported: all recorded
//! positions, and "quiet" ones (not in check, played move not a capture,
//! promotion or en passant) - the selection the old generator wrote, so that
//! row is the one to compare against old data.

use std::io::{Read, Seek, SeekFrom};

use anyhow::{bail, Context};
use datatools::{
    for_each_game, for_each_line, material_stm, material_stm_bullet, material_stm_fen,
    parse_game_line, MATE_SCORE,
};
use viriformat::{chess::piece::Colour, dataformat::{Game, WDL}};

#[derive(Default)]
struct PosStats {
    n: u64,
    big_score: u64,       // |score| > 1000
    mate_score: u64,      // |score| >= MATE_SCORE (mapped mate scores)
    big_material: u64,    // |material| >= 5
    down9: u64,           // side to move >= 9 points behind
    down9_result: f64,    // sum of results (side to move's view) of down9
    down9_score: f64,     // sum of scores (side to move's view) of down9, mates excluded
    down9_score_n: u64,
    result_sum: f64,
}

impl PosStats {
    /// `score` and `result` from the side to move's point of view.
    fn add(&mut self, score: i32, result: f64, mat: i32) {
        self.n += 1;
        self.result_sum += result;
        if score.abs() > 1000 {
            self.big_score += 1;
        }
        if score.abs() >= i32::from(MATE_SCORE) {
            self.mate_score += 1;
        }
        if mat.abs() >= 5 {
            self.big_material += 1;
        }
        if mat <= -9 {
            self.down9 += 1;
            self.down9_result += result;
            if score.abs() < i32::from(MATE_SCORE) {
                self.down9_score += f64::from(score);
                self.down9_score_n += 1;
            }
        }
    }

    fn print(&self, label: &str) {
        let pct = |x: u64| 100.0 * x as f64 / self.n.max(1) as f64;
        println!("{label}: {} positions", self.n);
        println!("  |score| > 1000:           {:8.4} %", pct(self.big_score));
        println!("  mate score (|s|>=32000):  {:8.4} %", pct(self.mate_score));
        println!("  |material diff| >= 5:     {:8.4} %", pct(self.big_material));
        println!(
            "  STM >= 9 points behind:   {:8.4} %  ({} pos, mean result {:.3}, mean score {:.0} cp)",
            pct(self.down9),
            self.down9,
            self.down9_result / self.down9.max(1) as f64,
            self.down9_score / self.down9_score_n.max(1) as f64
        );
        println!("  mean result (STM view):   {:.3}", self.result_sum / self.n.max(1) as f64);
    }
}

#[derive(Default)]
struct GameStats {
    games: u64,
    moves: u64,
    wdl: [u64; 3],
    hist: [u64; 8], // recorded positions per game: <50, <100, ..., <350, >=350
    max_len: usize,
}

struct Collector {
    all: PosStats,
    quiet: PosStats,
    games: GameStats,
}

impl Collector {
    fn new() -> Self {
        Self { all: PosStats::default(), quiet: PosStats::default(), games: GameStats::default() }
    }

    fn add_game(&mut self, game: &Game) {
        let outcome = game.outcome();
        let white_result = match outcome {
            WDL::Win => 1.0,
            WDL::Draw => 0.5,
            WDL::Loss => 0.0,
        };
        let g = &mut self.games;
        g.games += 1;
        g.moves += game.len() as u64;
        g.wdl[match outcome {
            WDL::Win => 0,
            WDL::Draw => 1,
            WDL::Loss => 2,
        }] += 1;
        g.hist[(game.len() / 50).min(7)] += 1;
        g.max_len = g.max_len.max(game.len());

        let mut board = game.initial_position();
        for (mv, score) in &game.moves {
            let white = board.turn() == Colour::White;
            let stm_score = if white { i32::from(score.get()) } else { -i32::from(score.get()) };
            let stm_result = if white { white_result } else { 1.0 - white_result };
            let mat = material_stm(&board);
            self.all.add(stm_score, stm_result, mat);
            if !board.in_check() && !board.is_tactical(*mv) {
                self.quiet.add(stm_score, stm_result, mat);
            }
            board.make_move_simple(*mv);
        }
    }

    fn print(&self) {
        let g = &self.games;
        println!("games: {}", g.games);
        println!(
            "  results (White view): {} W / {} D / {} L",
            g.wdl[0], g.wdl[1], g.wdl[2]
        );
        println!(
            "  recorded positions per game: mean {:.1}, max {}",
            g.moves as f64 / g.games.max(1) as f64,
            g.max_len
        );
        print!("  histogram (per 50):");
        for (i, c) in g.hist.iter().enumerate() {
            let label = if i == 7 { ">=350".to_string() } else { format!("<{}", (i + 1) * 50) };
            print!(" {label}:{c}");
        }
        println!();
        self.all.print("all positions");
        self.quiet.print("quiet positions (no check, no capture/promo/ep move)");
    }
}

fn main() -> anyhow::Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(mode) = args.first() else {
        bail!("usage: data_stats <vf|games|text|bullet> <files...>");
    };
    let files = args[1..].to_vec();
    match mode.as_str() {
        "vf" => {
            let mut c = Collector::new();
            for f in &files {
                for_each_game(f, |game| {
                    c.add_game(game);
                    Ok(())
                })?;
            }
            c.print();
        }
        "games" => {
            let mut c = Collector::new();
            let mut bad = 0u64;
            for_each_line(&files, |_, line, _| {
                if line.trim().is_empty() {
                    return Ok(());
                }
                match parse_game_line(line) {
                    Ok(game) => c.add_game(&game),
                    Err(_) => bad += 1,
                }
                Ok(())
            })?;
            c.print();
            println!("unparsable lines: {bad}");
        }
        "text" => {
            let mut s = PosStats::default();
            for_each_line(&files, |path, line, lineno| {
                let mut parts = line.split('|');
                let (Some(fen), Some(score), Some(wdl)) = (parts.next(), parts.next(), parts.next())
                else {
                    return Ok(());
                };
                let fen = fen.trim();
                let score: i32 = score.trim().parse().with_context(|| format!("{path}:{lineno}"))?;
                let wdl: f64 = wdl.trim().parse().with_context(|| format!("{path}:{lineno}"))?;
                let white = fen.split_whitespace().nth(1) == Some("w");
                let mat = material_stm_fen(fen).with_context(|| format!("{path}:{lineno}"))?;
                // Old text format: score and result are White-relative.
                let (score, result) = if white { (score, wdl) } else { (-score, 1.0 - wdl) };
                s.add(score, result, mat);
                Ok(())
            })?;
            s.print("old text data");
        }
        "bullet" => {
            let path = files.first().context("missing file")?;
            let n: u64 = files.get(1).map_or(Ok(4_000_000), |s| s.parse())?;
            let off: u64 = files.get(2).map_or(Ok(0), |s| s.parse())?;
            let mut f = std::fs::File::open(path)?;
            let total = f.metadata()?.len() / 32;
            let off = off.min(total.saturating_sub(n));
            f.seek(SeekFrom::Start(off * 32))?;
            let mut r = std::io::BufReader::with_capacity(1 << 20, f);
            let mut s = PosStats::default();
            let mut buf = [0u8; 32];
            for _ in 0..n.min(total - off) {
                r.read_exact(&mut buf)?;
                // SAFETY: ChessBoard is a 32-byte repr(C) struct of integers.
                let b: bulletformat::ChessBoard = unsafe { std::mem::transmute(buf) };
                // bulletformat: score and result are side-to-move relative.
                s.add(i32::from(b.score), f64::from(b.result) / 2.0, material_stm_bullet(&b));
            }
            s.print(&format!("bulletformat {path} ({} of {total} positions from {off})", s.n));
        }
        other => bail!("unknown mode {other}"),
    }
    Ok(())
}
