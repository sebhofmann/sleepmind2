#!/usr/bin/env python3
"""Print per FEN: static eval (cp, white view) and node count of a fixed-depth search.

usage: engine_probe.py <engine> <fens.txt> [depth]   (depth 0 = eval only)
"""
import os
import re
import subprocess
import sys

engine, fen_file = sys.argv[1], sys.argv[2]
depth = int(sys.argv[3]) if len(sys.argv) > 3 else 8

p = subprocess.Popen([engine], cwd=os.path.dirname(engine) or ".", stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)


def send(cmd):
    p.stdin.write(cmd + "\n")
    p.stdin.flush()


def wait_for(prefix):
    lines = []
    while True:
        line = p.stdout.readline()
        if not line:
            raise SystemExit(f"engine exited while waiting for {prefix!r}")
        lines.append(line.rstrip("\n"))
        if line.startswith(prefix):
            return lines


send("uci")
wait_for("uciok")
for fen in open(fen_file):
    fen = fen.strip()
    if not fen:
        continue
    send("ucinewgame")
    send(f"position fen {fen}")
    send("eval")
    ev = wait_for("info string Evaluation")[-1]
    cp = int(re.search(r"Evaluation: (-?\d+)", ev).group(1))
    nodes = "-"
    if depth > 0:
        send(f"go depth {depth}")
        out = wait_for("bestmove")
        found = [re.search(r" nodes (\d+)", l) for l in out]
        found = [m.group(1) for m in found if m]
        nodes = found[-1] if found else "?"
    print(f"{cp}\t{nodes}\t{fen}", flush=True)
send("quit")
