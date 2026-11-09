//! Threat and pawn-pair inputs for the threats NNUE (training_threats.rs, engine: NNUE_ARCH=threats).
//!
//! Same feature space as bullet's `examples/advanced` (pawnocchio), implemented independently.
//! The engine (src/nnue_threats.c) computes exactly the same indices, keep both in sync.
//!
//! All features are seen from one perspective. A perspective transforms squares with
//! `sq ^ m`, m = (56 if the perspective is the second side) ^ (7 if its own king is on files e-h),
//! and colours relative to itself (0 = own, 1 = opponent). In bulletformat the side to move is
//! always white, so "stm" is perspective 0 and "ntm" perspective 1.
//!
//! Index layout (one shared input layer, i8 weights + i16 bias):
//!   [0, 4560)       pawn pairs: every unordered pair of pawns (any colours) at most one file apart,
//!                   pawn id = colour * 48 + sq - 8, index = hi * (hi - 1) / 2 + lo
//!   [4560, 64368)   threats: attacker colour * 29904 + per-piece block, see `Tables`
//! A threat is "attacker (not a king) attacks an occupied square". Targets per attacker:
//!   pawn -> knight, rook;  knight, queen -> P N B R Q;  bishop, rook -> P N B R  (both colours,
//!   kings are never targets). Attacks between two pieces of the same type are only counted
//!   when the target square is below the attacker's (in perspective coordinates).

use bullet::game::formats::{bulletformat::ChessBoard, montyformat::chess::Attacks};

pub const NUM_PAIRS: usize = 96 * 95 / 2;
pub const THREATS_PER_SIDE: usize = 29904;
pub const NUM_INPUTS: usize = NUM_PAIRS + 2 * THREATS_PER_SIDE;
/// As in bullet's example: at most 16 pawns -> 120 pairs, plus up to 128 threats (real positions
/// stay far below, max seen 50 threats / 85 features in 2M positions).
pub const MAX_ACTIVE: usize = 120 + 128;

const PAWN: usize = 0;
const KING: usize = 5;

/// Target slot of victim type per attacker type (-1 = not a feature)
const TARGET_SLOT: [[i8; 6]; 5] = [
    [-1, 0, -1, 1, -1, -1], // pawn: N R
    [0, 1, 2, 3, 4, -1],    // knight: P N B R Q
    [0, 1, 2, 3, -1, -1],   // bishop: P N B R
    [0, 1, 2, 3, -1, -1],   // rook: P N B R
    [0, 1, 2, 3, 4, -1],    // queen: P N B R Q
];
const SLOTS: [usize; 5] = [2, 5, 4, 4, 5];
/// Pawn block: per target slot 6 ranks (2..7) x 14 attack directions
const PAWN_SLOT_SIZE: usize = 6 * 14;

pub struct Tables {
    /// empty-board attacks of N, B, R, Q
    attacks: [[u64; 64]; 4],
    /// number of empty-board attacks on squares below sq, per N, B, R, Q
    sq_base: [[u16; 64]; 4],
    /// total empty-board attacks per N, B, R, Q
    count: [usize; 4],
    /// start of each piece's block inside one side's 29904 threats (P, N, B, R, Q)
    offset: [usize; 5],
}

fn ray_attacks(sq: usize, dirs: &[(i32, i32)]) -> u64 {
    let mut bb = 0u64;
    for &(dr, df) in dirs {
        let (mut r, mut f) = ((sq / 8) as i32 + dr, (sq % 8) as i32 + df);
        while (0..8).contains(&r) && (0..8).contains(&f) {
            bb |= 1 << (r * 8 + f);
            r += dr;
            f += df;
        }
    }
    bb
}

impl Tables {
    pub fn new() -> Self {
        const DIAG: [(i32, i32); 4] = [(1, 1), (1, -1), (-1, 1), (-1, -1)];
        const ORTH: [(i32, i32); 4] = [(1, 0), (-1, 0), (0, 1), (0, -1)];
        let mut attacks = [[0u64; 64]; 4];
        for sq in 0..64 {
            attacks[0][sq] = Attacks::knight(sq);
            attacks[1][sq] = ray_attacks(sq, &DIAG);
            attacks[2][sq] = ray_attacks(sq, &ORTH);
            attacks[3][sq] = attacks[1][sq] | attacks[2][sq];
        }
        let mut sq_base = [[0u16; 64]; 4];
        let mut count = [0usize; 4];
        for p in 0..4 {
            for sq in 0..64 {
                sq_base[p][sq] = count[p] as u16;
                count[p] += attacks[p][sq].count_ones() as usize;
            }
        }
        let mut offset = [0usize; 5];
        let mut next = 2 * SLOTS[PAWN] * PAWN_SLOT_SIZE;
        for p in 1..5 {
            offset[p] = next;
            next += 2 * SLOTS[p] * count[p - 1];
        }
        assert_eq!(next, THREATS_PER_SIDE);
        Self { attacks, sq_base, count, offset }
    }

    /// Index inside one attacker side's threat block, squares/colours already in perspective
    /// coordinates. None if the pair is not a feature.
    fn threat_index(&self, attacker: usize, from: usize, victim: usize, victim_colour: usize, to: usize) -> Option<usize> {
        let slot = TARGET_SLOT[attacker][victim];
        if slot < 0 {
            return None;
        }
        let slot = slot as usize + victim_colour * SLOTS[attacker];
        if attacker == PAWN {
            let file = from % 8;
            let right = usize::from(to % 8 > file);
            return Some(slot * PAWN_SLOT_SIZE + (from / 8 - 1) * 14 + 2 * file + right - 1);
        }
        if attacker == victim && to > from {
            return None;
        }
        let p = attacker - 1;
        let below = self.attacks[p][from] & ((1u64 << to) - 1);
        Some(self.offset[attacker] + slot * self.count[p] + usize::from(self.sq_base[p][from]) + below.count_ones() as usize)
    }
}

impl Default for Tables {
    fn default() -> Self {
        Self::new()
    }
}

/// Piece bitboards of a position: [colour][type]
pub fn piece_bbs(pos: &ChessBoard) -> [[u64; 6]; 2] {
    let mut bbs = [[0u64; 6]; 2];
    for (pc, sq) in pos.into_iter() {
        bbs[usize::from(pc & 8 > 0)][usize::from(pc & 7)] |= 1 << sq;
    }
    bbs
}

fn for_each_bit(mut bb: u64, mut f: impl FnMut(usize)) {
    while bb != 0 {
        f(bb.trailing_zeros() as usize);
        bb &= bb - 1;
    }
}

/// Calls `emit` with every active feature of `perspective` (0 = white/stm, 1 = black/ntm).
pub fn map_perspective(tables: &Tables, bbs: &[[u64; 6]; 2], perspective: usize, mut emit: impl FnMut(usize)) {
    let king = bbs[perspective][KING].trailing_zeros() as usize;
    let m = (if perspective == 1 { 56 } else { 0 }) ^ (if king % 8 >= 4 { 7 } else { 0 });

    // pawn pairs
    let pawns = bbs[0][PAWN] | bbs[1][PAWN];
    let pawn_id = |sq: usize| {
        let colour = usize::from(bbs[1][PAWN] & (1 << sq) != 0) ^ perspective;
        colour * 48 + (sq ^ m) - 8
    };
    for_each_bit(pawns, |a| {
        let file = a % 8;
        let mut band = 0x0101_0101_0101_0101u64 << file;
        if file > 0 {
            band |= 0x0101_0101_0101_0101u64 << (file - 1);
        }
        if file < 7 {
            band |= 0x0101_0101_0101_0101u64 << (file + 1);
        }
        // each unordered pair once: partner on a higher square
        let higher = !(2u64 << a).wrapping_sub(1);
        let id_a = pawn_id(a);
        for_each_bit(pawns & band & higher, |b| {
            let id_b = pawn_id(b);
            let (lo, hi) = (id_a.min(id_b), id_a.max(id_b));
            emit(hi * (hi - 1) / 2 + lo);
        });
    });

    // threats
    let mut piece_on = [(0usize, 0usize); 64];
    let mut occ = 0u64;
    for colour in 0..2 {
        for pt in 0..6 {
            for_each_bit(bbs[colour][pt], |sq| piece_on[sq] = (colour, pt));
            occ |= bbs[colour][pt];
        }
    }
    for colour in 0..2 {
        let side_base = NUM_PAIRS + (colour ^ perspective) * THREATS_PER_SIDE;
        for pt in PAWN..KING {
            for_each_bit(bbs[colour][pt], |from| {
                let attacks = match pt {
                    0 => Attacks::pawn(from, colour),
                    1 => Attacks::knight(from),
                    2 => Attacks::bishop(from, occ),
                    3 => Attacks::rook(from, occ),
                    _ => Attacks::queen(from, occ),
                } & occ;
                for_each_bit(attacks, |to| {
                    let (vc, vt) = piece_on[to];
                    if let Some(idx) = tables.threat_index(pt, from ^ m, vt, vc ^ perspective, to ^ m) {
                        emit(side_base + idx);
                    }
                });
            });
        }
    }
}
