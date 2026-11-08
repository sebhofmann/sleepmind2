//! Copies a bulletformat file (32 bytes/position), dropping every position
//! whose material difference (P1 N3 B3 R5 Q9, side to move's view) has an
//! absolute value >= the limit (default 5). The input file is only read.
//!
//! usage: filter_material -i <in.dat> -o <out.dat> [--min-abs-diff 5]
//!
//! Background: the old generator stopped games at the first |score| > 1000
//! before recording, so lopsided-material positions in the legacy data are
//! almost exclusively pawn races / pre-adjudication artefacts with misleading
//! labels (see training/diagnostics on branch deep-nnue-diagnostics).

use std::io::{BufReader, BufWriter, Read, Write};

use anyhow::{bail, Context};
use datatools::material_stm_bullet;

const RECORD: usize = 32;
const CHUNK_RECORDS: usize = 1 << 20; // 32 MiB per read

fn main() -> anyhow::Result<()> {
    let mut input = None;
    let mut output = None;
    let mut limit: i32 = 5;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-i" | "--input" => input = args.next(),
            "-o" | "--output" => output = args.next(),
            "--min-abs-diff" => limit = args.next().context("missing value")?.parse()?,
            "-h" | "--help" => {
                println!("usage: filter_material -i <in.dat> -o <out.dat> [--min-abs-diff 5]");
                return Ok(());
            }
            other => bail!("unknown argument {other}"),
        }
    }
    let (Some(input), Some(output)) = (input, output) else {
        bail!("usage: filter_material -i <in.dat> -o <out.dat> [--min-abs-diff 5]");
    };
    if std::fs::canonicalize(&input).ok() == std::fs::canonicalize(&output).ok() {
        bail!("output must differ from input");
    }

    let in_file = std::fs::File::open(&input).with_context(|| format!("open {input}"))?;
    let total_bytes = in_file.metadata()?.len();
    if total_bytes % RECORD as u64 != 0 {
        bail!("{input}: size {total_bytes} is not a multiple of {RECORD}");
    }
    let total = total_bytes / RECORD as u64;
    let out_file = std::fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&output)
        .with_context(|| format!("create {output} (must not exist yet)"))?;

    let mut reader = BufReader::with_capacity(4 << 20, in_file);
    let mut writer = BufWriter::with_capacity(4 << 20, out_file);
    let mut buf = vec![0u8; CHUNK_RECORDS * RECORD];
    let mut out = Vec::with_capacity(buf.len());
    let (mut read, mut kept) = (0u64, 0u64);
    let mut dropped_by_diff = [0u64; 40];
    let start = std::time::Instant::now();

    while read < total {
        let n = ((total - read) as usize).min(CHUNK_RECORDS);
        reader.read_exact(&mut buf[..n * RECORD])?;
        out.clear();
        for rec in buf[..n * RECORD].chunks_exact(RECORD) {
            let arr: [u8; RECORD] = rec.try_into().unwrap();
            // SAFETY: ChessBoard is a 32-byte repr(C) struct of integers.
            let b: bulletformat::ChessBoard = unsafe { std::mem::transmute(arr) };
            let mat = material_stm_bullet(&b);
            if mat.abs() >= limit {
                dropped_by_diff[(mat.unsigned_abs() as usize).min(39)] += 1;
            } else {
                out.extend_from_slice(rec);
                kept += 1;
            }
        }
        writer.write_all(&out)?;
        read += n as u64;
        if read % (CHUNK_RECORDS as u64 * 64) == 0 {
            eprintln!(
                "  {read}/{total} ({:.1} %), {:.0} s",
                100.0 * read as f64 / total as f64,
                start.elapsed().as_secs_f64()
            );
        }
    }
    writer.flush()?;

    let dropped = total - kept;
    println!("read {total} positions from {input}");
    println!(
        "kept {kept}, dropped {dropped} ({:.4} %) with |material diff| >= {limit}",
        100.0 * dropped as f64 / total.max(1) as f64
    );
    print!("dropped by |diff|:");
    for (d, c) in dropped_by_diff.iter().enumerate() {
        if *c > 0 {
            print!(" {d}:{c}");
        }
    }
    println!();
    println!("written to {output} in {:.0} s", start.elapsed().as_secs_f64());
    Ok(())
}
