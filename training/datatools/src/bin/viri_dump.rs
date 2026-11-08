//! Prints the games of a viriformat binpack in the generator's text line
//! format (`FEN | uci:score ... | result`), for round-trip checks.
//!
//! usage: viri_dump <file.vf> [max_games]

use std::io::Write;

use anyhow::bail;
use datatools::{for_each_game, format_game_line};

fn main() -> anyhow::Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(path) = args.first() else { bail!("usage: viri_dump <file.vf> [max_games]") };
    let limit: u64 = args.get(1).map_or(Ok(u64::MAX), |s| s.parse())?;
    let stdout = std::io::stdout();
    let mut out = std::io::BufWriter::new(stdout.lock());
    let mut printed = 0;
    for_each_game(path, |game| {
        if printed < limit {
            writeln!(out, "{}", format_game_line(game))?;
            printed += 1;
        }
        Ok(())
    })?;
    out.flush()?;
    Ok(())
}
