//! Shared helpers for the SleepMind data tools.
//!
//! Game text format written by `build/training` (one game per line):
//!
//! ```text
//! <start FEN> | <uci>:<score> <uci>:<score> ... | <result>
//! ```
//!
//! `score` is the search score of the position before the move, White's point
//! of view (the viriformat convention), `result` is 1.0 / 0.5 / 0.0 from
//! White's point of view.

use std::io::{BufRead, BufReader, Read};

use anyhow::{anyhow, bail, Context};
use viriformat::{
    chess::{
        board::{Board, DrawType, GameOutcome, WinType},
        chessmove::Move,
        piece::{Colour, PieceType},
    },
    dataformat::{Game, WDL},
};

/// Piece values used for the material statistics/filters (P N B R Q K).
pub const PIECE_VALUES: [i32; 6] = [1, 3, 3, 5, 9, 0];

/// Score magnitude the generator uses for mate/TB scores.
pub const MATE_SCORE: i16 = 32000;

/// Parses one game line into a viriformat `Game`. Every move is checked for
/// legality by replaying it on the board.
pub fn parse_game_line(line: &str) -> anyhow::Result<Game> {
    let mut parts = line.split('|');
    let fen = parts.next().ok_or_else(|| anyhow!("missing FEN"))?.trim();
    let moves = parts.next().ok_or_else(|| anyhow!("missing move list"))?;
    let result = parts.next().ok_or_else(|| anyhow!("missing result"))?.trim();
    if parts.next().is_some() {
        bail!("too many '|' fields");
    }

    let mut board = Board::new();
    board.set_from_fen(fen).with_context(|| format!("bad FEN {fen:?}"))?;
    let mut game = Game::new(&board);

    for token in moves.split_whitespace() {
        let (uci, score) = token
            .split_once(':')
            .ok_or_else(|| anyhow!("bad move token {token:?}"))?;
        let score: i16 = score.parse().with_context(|| format!("bad score in {token:?}"))?;
        let mv = find_move(&mut board, uci)
            .ok_or_else(|| anyhow!("illegal move {uci} in {board}"))?;
        game.add_move(mv, score);
        if !board.make_move_simple(mv) {
            bail!("move {uci} leaves king in check in {board}");
        }
    }

    let outcome = match result {
        "1.0" | "1" | "1-0" => GameOutcome::WhiteWin(WinType::Mate),
        "0.5" | "1/2-1/2" => GameOutcome::Draw(DrawType::Repetition),
        "0.0" | "0" | "0-1" => GameOutcome::BlackWin(WinType::Mate),
        other => bail!("bad result {other:?}"),
    };
    game.set_outcome(outcome);
    Ok(game)
}

/// Finds the legal move with the given (standard, non-960) UCI string.
pub fn find_move(board: &mut Board, uci: &str) -> Option<Move> {
    board
        .legal_moves()
        .into_iter()
        .find(|mv| mv.display(false).to_string() == uci)
}

/// Formats a game back into the text line format (used for round-trip checks).
pub fn format_game_line(game: &Game) -> String {
    let mut board = game.initial_position();
    let mut out = format!("{board} |");
    for (mv, score) in &game.moves {
        out.push_str(&format!(" {}:{}", mv.display(false), score.get()));
        board.make_move_simple(*mv);
    }
    let res = match game.outcome() {
        WDL::Win => "1.0",
        WDL::Draw => "0.5",
        WDL::Loss => "0.0",
    };
    out.push_str(&format!(" | {res}"));
    out
}

/// Material balance (P1 N3 B3 R5 Q9) from the side to move's point of view.
pub fn material_stm(board: &Board) -> i32 {
    let layout = &board.pieces;
    let mut white = 0;
    let mut black = 0;
    for (pt, value) in [
        PieceType::Pawn,
        PieceType::Knight,
        PieceType::Bishop,
        PieceType::Rook,
        PieceType::Queen,
    ]
    .into_iter()
    .zip(PIECE_VALUES)
    {
        let bb = layout.of_type(pt);
        white += value * (bb & layout.occupied_co(Colour::White)).count() as i32;
        black += value * (bb & layout.occupied_co(Colour::Black)).count() as i32;
    }
    if board.turn() == Colour::White {
        white - black
    } else {
        black - white
    }
}

/// Material balance from the side to move's point of view for a bulletformat
/// record (pieces are stored relative to the side to move: colour bit 8 =
/// opponent, low 3 bits = P N B R Q K).
pub fn material_stm_bullet(board: &bulletformat::ChessBoard) -> i32 {
    let n = board.occ.count_ones() as usize;
    let mut mat = 0;
    for i in 0..n {
        let nib = (board.pcs[i / 2] >> (4 * (i & 1))) & 0xF;
        let pt = usize::from(nib & 7).min(5);
        let v = PIECE_VALUES[pt];
        mat += if nib & 8 == 0 { v } else { -v };
    }
    mat
}

/// Material balance from the side to move's point of view for a FEN.
pub fn material_stm_fen(fen: &str) -> Option<i32> {
    let mut fields = fen.split_whitespace();
    let placement = fields.next()?;
    let stm = fields.next()?;
    let mut mat = 0;
    for ch in placement.chars() {
        let v = match ch.to_ascii_lowercase() {
            'p' => 1,
            'n' | 'b' => 3,
            'r' => 5,
            'q' => 9,
            _ => continue,
        };
        mat += if ch.is_ascii_uppercase() { v } else { -v };
    }
    Some(if stm == "b" { -mat } else { mat })
}

/// Iterates over the lines of several text inputs ("-" = stdin).
pub fn for_each_line(
    paths: &[String],
    mut f: impl FnMut(&str, &str, usize) -> anyhow::Result<()>,
) -> anyhow::Result<()> {
    for path in paths {
        let reader: Box<dyn Read> = if path == "-" {
            Box::new(std::io::stdin())
        } else {
            Box::new(std::fs::File::open(path).with_context(|| format!("open {path}"))?)
        };
        let reader = BufReader::with_capacity(1 << 20, reader);
        for (idx, line) in reader.lines().enumerate() {
            let line = line.with_context(|| format!("read {path}"))?;
            f(path, &line, idx + 1)?;
        }
    }
    Ok(())
}

/// Reads all games of a viriformat binpack.
pub fn for_each_game(
    path: &str,
    mut f: impl FnMut(&Game) -> anyhow::Result<()>,
) -> anyhow::Result<u64> {
    let file = std::fs::File::open(path).with_context(|| format!("open {path}"))?;
    let mut reader = BufReader::with_capacity(1 << 20, file);
    let mut count = 0;
    let mut buffer = Vec::new();
    loop {
        if reader.fill_buf()?.is_empty() {
            break;
        }
        let game = Game::deserialise_from(&mut reader, buffer)
            .with_context(|| format!("game #{} in {path}", count + 1))?;
        f(&game)?;
        count += 1;
        buffer = game.into_move_buffer();
    }
    Ok(count)
}
