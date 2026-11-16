#!/bin/bash
# Unbeaufsichtigte Pipeline (16.11.2026): neues Deep-Netz für main.
#   Daten: Altdaten (bulletformat, |Materialdifferenz| >= 5 gefiltert) + gen12.vf + gen13_all.vf
#          (alle 18 gen13-Dateien), proportional gemischt (kein --viri-fraction)
#   1. Training 1100 SBs (3 Stufen, wie deep_gen12)
#   2. Clean-Build mit dem neuen Netz (per NNUE_FILE, quantised_deep.bin im Repo bleibt
#      unverändert) -> Variante deep_gen13
#   3. Engine-Evals gegen bullets Float-Referenz prüfen, Repo-Build zurück auf Standard
#   4. Fixed-Nodes-SPRT (30k) gegen den aktuellen main, höchstens FIXED_NODES_MINUTES
#   5. LTC-SPRT 60+0.6 gegen den aktuellen main
# STAGE=train|test (Standard: beides) erlaubt den Wiedereinstieg.
set -euo pipefail

ROOT=/home/paschty/workspace/sleepmind2
VARIANT=deep_gen13
BASE=main_e68ee92
NAME=sleepmind_deep_gen13
OLD_DATA="$ROOT/training_data/training_data_mat4.dat"
VIRI_DATA=("$ROOT/training_data/gen12.vf" "$ROOT/training_data/gen13_all.vf")
CKPT="$ROOT/training/checkpoints/$NAME-stage2-200"
STATUS="$ROOT/variants/${VARIANT}_pipeline.status"
EXPECTED_SIZE=19009600
NODES=30000
CONCURRENCY=16
FIXED_NODES_MINUTES="${FIXED_NODES_MINUTES:-60}"
LTC=60+0.6
STAGE="${STAGE:-all}"

status() { echo "$(date '+%F %T') $*" | tee -a "$STATUS"; }
trap 'status "FEHLER in Zeile $LINENO (exit $?)"' ERR

if [ "$STAGE" = all ] || [ "$STAGE" = train ]; then
    # Fortsetzen nach Abbruch, z. B.
    #   RESUME_ARGS="--start-stage 1 --start 51 -r checkpoints/$NAME-stage1-50/optimiser_state"
    RESUME_ARGS="${RESUME_ARGS:-}"
    for f in "$OLD_DATA" "${VIRI_DATA[@]}"; do
        [ -s "$f" ] || { status "FEHLER: Daten fehlen: $f"; exit 1; }
    done
    status "Start Training ($NAME)${RESUME_ARGS:+ fortgesetzt: $RESUME_ARGS}"
    cd "$ROOT/training"
    cargo build --release --bin training_deep
    viri_args=()
    for f in "${VIRI_DATA[@]}"; do viri_args+=(--viri "$f"); done
    # shellcheck disable=SC2086 # RESUME_ARGS soll in einzelne Argumente zerfallen
    ./target/release/training_deep -d "$OLD_DATA" "${viri_args[@]}" -s 1100 -t 6 -n "$NAME" $RESUME_ARGS
    status "Training fertig"

    [ -f "$CKPT/quantised.bin" ] || { status "FEHLER: $CKPT/quantised.bin fehlt"; exit 1; }
    size=$(stat -c %s "$CKPT/quantised.bin")
    [ "$size" = "$EXPECTED_SIZE" ] || { status "FEHLER: Netzgröße $size != $EXPECTED_SIZE"; exit 1; }

    cd "$ROOT"
    status "Clean-Build mit neuem Netz"
    export NNUE_FILE="$CKPT/quantised.bin"
    make clean && make both
    make test
    rm -rf "$ROOT/variants/$VARIANT"
    ./variants.sh create "$VARIANT"
    cmp "$CKPT/quantised.bin" "$ROOT/variants/$VARIANT/quantised.bin"
    unset NNUE_FILE

    # Engine (Integer) gegen bullets Float-Graph auf denselben FENs
    cd "$ROOT/training"
    ./target/release/training_deep -r "$CKPT/optimiser_state" --eval eval_fens.txt | grep -P '^-?\d+\.\d\t' \
        > "$ROOT/variants/${VARIANT}_ref_evals.txt"
    python3 engine_probe.py "$ROOT/variants/$VARIANT/sleepmind" eval_fens.txt 0 > "$ROOT/variants/${VARIANT}_engine_evals.txt"
    mean=$(paste "$ROOT/variants/${VARIANT}_ref_evals.txt" "$ROOT/variants/${VARIANT}_engine_evals.txt" \
        | awk -F'\t' '{d=$3-$1; if(d<0)d=-d; s+=d; n++} END{printf "%.1f", s/n}')
    status "Engine vs. Float-Referenz: mittlere Abweichung $mean cp"
    awk -v m="$mean" 'BEGIN{exit !(m < 30)}' || { status "FEHLER: Abweichung zu groß"; exit 1; }

    cd "$ROOT"
    make clean && make both
    if cmp -s build/sleepmind "variants/$BASE/sleepmind"; then
        status "Variante $VARIANT angelegt, Repo-Build wieder Standard (identisch mit $BASE)"
    else
        status "Variante $VARIANT angelegt, Repo-Build wieder Standard (Binary weicht von $BASE ab)"
    fi
fi

if [ "$STAGE" = all ] || [ "$STAGE" = test ]; then
    cd "$ROOT"
    [ -x "variants/$VARIANT/sleepmind" ] || { status "FEHLER: variants/$VARIANT/sleepmind fehlt"; exit 1; }
    [ -x "variants/$BASE/sleepmind" ] || { status "FEHLER: variants/$BASE/sleepmind fehlt"; exit 1; }
    cmp quantised_deep.bin "variants/$BASE/quantised.bin" || { status "FEHLER: $BASE hat nicht das main-Netz"; exit 1; }

    LOG="$ROOT/variants/${VARIANT}_vs_${BASE}_sprt_nodes${NODES}.log"
    status "Start Fixed-Nodes-SPRT $VARIANT vs $BASE, nodes=$NODES, concurrency=$CONCURRENCY, max. $FIXED_NODES_MINUTES min"
    NODES=$NODES CONCURRENCY=$CONCURRENCY timeout -s INT "${FIXED_NODES_MINUTES}m" \
        ./tournament.sh sprt "$VARIANT" "$BASE" > "$LOG" 2>&1 || true
    status "Fixed-Nodes beendet: $(grep -E '^(Elo|LLR|Games)' "$LOG" | tail -n 3 | tr '\n' ' ')"

    LOG="$ROOT/variants/${VARIANT}_vs_${BASE}_sprt_ltc.log"
    status "Start LTC-SPRT $VARIANT vs $BASE, tc=$LTC, concurrency=$CONCURRENCY"
    TC=$LTC CONCURRENCY=$CONCURRENCY ./tournament.sh sprt "$VARIANT" "$BASE" > "$LOG" 2>&1 || true
    status "LTC beendet: $(grep -E '^(Elo|LLR|Games)' "$LOG" | tail -n 3 | tr '\n' ' ')"
fi
