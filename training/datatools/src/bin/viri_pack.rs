//! Packs game text lines (written by `build/training`) into a viriformat binpack.
//!
//! usage: viri_pack -o <out.vf> [--append] [--strict] <games.txt|-> ...
//!
//! Every move is replayed and checked for legality. Lines that fail to parse
//! (e.g. a truncated last line of a killed generator) are skipped and counted;
//! with --strict any such line is an error.

use std::io::{BufWriter, Write};

use anyhow::{bail, Context};
use datatools::{for_each_line, parse_game_line};
use viriformat::dataformat::WDL;

fn main() -> anyhow::Result<()> {
    let mut out = None;
    let mut append = false;
    let mut strict = false;
    let mut inputs = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-o" | "--output" => out = args.next(),
            "--append" => append = true,
            "--strict" => strict = true,
            "-h" | "--help" => {
                println!("usage: viri_pack -o <out.vf> [--append] [--strict] <games.txt|-> ...");
                return Ok(());
            }
            _ => inputs.push(arg),
        }
    }
    let Some(out) = out else { bail!("missing -o <out.vf>") };
    if inputs.is_empty() {
        bail!("no input files");
    }

    let file = std::fs::OpenOptions::new()
        .create(true)
        .write(true)
        .append(append)
        .truncate(!append)
        .open(&out)
        .with_context(|| format!("open {out}"))?;
    let mut writer = BufWriter::with_capacity(1 << 20, file);

    let (mut games, mut positions, mut empty, mut bad) = (0u64, 0u64, 0u64, 0u64);
    let mut wdl = [0u64; 3];
    for_each_line(&inputs, |path, line, lineno| {
        if line.trim().is_empty() {
            return Ok(());
        }
        match parse_game_line(line) {
            Ok(game) => {
                if game.is_empty() {
                    empty += 1;
                    return Ok(());
                }
                game.serialise_into(&mut writer)?;
                games += 1;
                positions += game.len() as u64;
                wdl[match game.outcome() {
                    WDL::Win => 0,
                    WDL::Draw => 1,
                    WDL::Loss => 2,
                }] += 1;
            }
            Err(e) => {
                bad += 1;
                if strict {
                    bail!("{path}:{lineno}: {e:#}");
                }
                if bad <= 10 {
                    eprintln!("warning: {path}:{lineno}: skipped ({e:#})");
                }
            }
        }
        Ok(())
    })?;
    writer.flush()?;

    println!("packed {games} games / {positions} positions into {out}");
    println!("  results (White view): {} wins / {} draws / {} losses", wdl[0], wdl[1], wdl[2]);
    println!("  skipped: {empty} empty games, {bad} unparsable lines");
    Ok(())
}
