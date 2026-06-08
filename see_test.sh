#!/bin/bash
# SEE-Validierung gegen Arasan-Suite (unit.cpp testSee).
# Erwartungswerte mit MEINEN Figurenwerten (P100 N320 B330 R500 Q900) nachgerechnet.
# Nur Nicht-Promo-Captures (das was mein SEE-Pruning nutzt). Quiet/Promo-Faelle ausgelassen.
ENG=./build/sleepmind

# Format: FEN|fromto|expected
CASES=(
"4R3/2r3p1/5bk1/1p1r3p/p2PR1P1/P1BK1P2/1P6/8 b - -|h5g4|0"
"4R3/2r3p1/5bk1/1p1r1p1p/p2PR1P1/P1BK1P2/1P6/8 b - -|h5g4|0"
"4r1k1/5pp1/nbp4p/1p2p2q/1P2P1b1/1BP2N1P/1B2QPPK/3R4 b - -|g4f3|-10"
"2r1r1k1/pp1bppbp/3p1np1/q3P3/2P2P2/1P2B3/P1N1B1PP/2RQ1RK1 b - -|d6e5|100"
"8/4kp2/2npp3/1Nn5/1p2PQP1/7q/1PP1B3/4KR1r b - -|h1f1|0"
"8/4kp2/2npp3/1Nn5/1p2P1P1/7q/1PP1B3/4KR1r b - -|h1f1|0"
"2r2r1k/6bp/p7/2q2p1Q/3PpP2/1B6/P5PP/2RR3K b - -|c5c1|100"
"r2qk1nr/pp2ppbp/2b3p1/2p1p3/8/2N2N2/PPPP1PPP/R1BQR1K1 w kq -|f3e5|100"
"6r1/4kq2/b2p1p2/p1pPb3/p1P2B1Q/2P4P/2B1R1P1/6K1 w - -|f4e5|0"
"3q2nk/pb1r1p2/np6/3P2Pp/2p1P3/2R4B/PQ3P1P/3R2K1 w - h6|g5h6|0"
"3q2nk/pb1r1p2/np6/3P2Pp/2p1P3/2R1B2B/PQ3P1P/3R2K1 w - h6|g5h6|100"
"2r4r/1P4pk/p2p1b1p/7n/BB3p2/2R2p2/P1P2P2/4RK2 w - -|c3c8|500"
"2r5/1P4pk/p2p1b1p/5b1n/BB3p2/2R2p2/P1P2P2/4RK2 w - -|c3c8|330"
"2r4k/2r4p/p7/2b2p1b/4pP2/1BR5/P1R3PP/2Q4K w - -|c3c5|330"
"8/pp6/2pkp3/4bp2/2R3b1/2P5/PP4B1/1K6 w - -|g2c6|-230"
"4q3/1p1pr1k1/1B2rp2/6p1/p3PP2/P3R1P1/1P2R1K1/4Q3 b - -|e6e4|-400"
"4k3/8/2p1p3/3p4/4P3/8/8/4K3 w - - 0 1|e4d5|0"
"4q3/1p1pr1kb/1B2rp2/6p1/p3PP2/P3R1P1/1P2R1K1/4Q3 b - -|h7e4|100"
)

pass=0; fail=0
for c in "${CASES[@]}"; do
  fen="${c%%|*}"; rest="${c#*|}"; mv="${rest%%|*}"; exp="${rest##*|}"
  out=$(printf 'position fen %s\nseedump\nquit\n' "$fen" | $ENG 2>/dev/null | grep "^see $mv ")
  got=$(echo "$out" | sed 's/.*= //')
  if [ "$got" = "$exp" ]; then
    pass=$((pass+1)); # echo "OK   $mv = $got"
  else
    fail=$((fail+1)); echo "FAIL $mv : erwartet $exp, bekommen '${got:-<kein Zug gefunden>}'  [$fen]"
  fi
done
echo "----"
echo "PASS=$pass FAIL=$fail"
