#!/bin/bash
# Unbeaufsichtigte Pipeline (7.11.2026), tiefe Architektur (training_deep.rs, NNUE_ARCH=deep):
#   1. Training 1100 SBs (3 Stufen)  2. Netz -> quantised_deep.bin -> Variante "deep1100"
#   3. Engine-Evals gegen bullets Float-Referenz prüfen  4. Repo-Build zurück auf Legacy
# Ein SPRT wird NICHT gestartet.
set -euo pipefail

ROOT=/home/paschty/workspace/sleepmind2
VARIANT=deep1100
NAME=sleepmind_deep
CKPT="$ROOT/training/checkpoints/$NAME-stage2-200"
STATUS="$ROOT/variants/${VARIANT}_pipeline.status"
EXPECTED_SIZE=19009600

status() { echo "$(date '+%F %T') $*" | tee -a "$STATUS"; }
trap 'status "FEHLER in Zeile $LINENO (exit $?)"' ERR

status "Start Training ($NAME)"
cd "$ROOT/training"
./target/release/training_deep -d ../training_data/training_data.dat -s 1100 -t 6 -n "$NAME"
status "Training fertig"

[ -f "$CKPT/quantised.bin" ] || { status "FEHLER: $CKPT/quantised.bin fehlt"; exit 1; }
size=$(stat -c %s "$CKPT/quantised.bin")
[ "$size" = "$EXPECTED_SIZE" ] || { status "FEHLER: Netzgröße $size != $EXPECTED_SIZE"; exit 1; }

cd "$ROOT"
status "Clean-Build NNUE_ARCH=deep mit neuem Netz"
cp "$CKPT/quantised.bin" "$ROOT/quantised_deep.bin"
make clean && make both NNUE_ARCH=deep
make test NNUE_ARCH=deep
NNUE_FILE="$ROOT/quantised_deep.bin" ./variants.sh create "$VARIANT"
cmp "$CKPT/quantised.bin" "$ROOT/variants/$VARIANT/quantised.bin"

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
status "Variante $VARIANT angelegt, Repo-Build wieder Legacy"
