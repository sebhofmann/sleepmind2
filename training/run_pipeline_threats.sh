#!/bin/bash
# Pipeline für das Threats-Netz (training_threats.rs, NNUE_ARCH=threats):
#   1. Training (3 Stufen)  2. Netz -> quantised_threats.bin -> Variante
#   3. Engine-Evals gegen bullets Float-Referenz prüfen  4. Repo-Build zurück auf Legacy
# Startet KEINEN SPRT. Als systemd-User-Unit starten, z. B.
#   systemd-run --user --unit threats-pipeline -p StandardOutput=append:$PWD/pipeline_threats.log \
#       -p StandardError=append:$PWD/pipeline_threats.log --setenv=PATH=$PATH \
#       --setenv=DATA_ARGS="-d /pfad/alt.dat --viri /pfad/neu.vf" ./run_pipeline_threats.sh
# Fortsetzen: RESUME_ARGS="--start-stage 1 --start 51 -r checkpoints/<name>-stage1-50/optimiser_state"
set -euo pipefail

ROOT=/home/paschty/workspace/sleepmind2
VARIANT=${VARIANT:-threats1100}
NAME=${NAME:-sleepmind_$VARIANT}
SUPERBATCHES=${SUPERBATCHES:-1100}
DATA_ARGS=${DATA_ARGS:?DATA_ARGS setzen, z. B. "-d <bulletformat> --viri <viriformat>"}
RESUME_ARGS="${RESUME_ARGS:-}"
STATUS="$ROOT/variants/${VARIANT}_pipeline.status"
# 768x16 PSQT (i16) + 64368 x 768 Threats/Pawn-Pairs (i8) + Bias + Deep-Ausgangsschichten, auf 64 B gepolstert
EXPECTED_SIZE=68444224

# letzter Checkpoint von Stufe 2 (bei SUPERBATCHES=1100: stage2-200)
STAGE2_SBS=$(( SUPERBATCHES * 2 / 11 )); [ "$STAGE2_SBS" -ge 1 ] || STAGE2_SBS=1
CKPT="$ROOT/training/checkpoints/$NAME-stage2-$STAGE2_SBS"

status() { echo "$(date '+%F %T') $*" | tee -a "$STATUS"; }
trap 'status "FEHLER in Zeile $LINENO (exit $?)"' ERR

status "Start Training ($NAME, $SUPERBATCHES SBs)${RESUME_ARGS:+ fortgesetzt: $RESUME_ARGS}"
cd "$ROOT/training"
cargo build --release --bin training_threats
# shellcheck disable=SC2086 # DATA_ARGS/RESUME_ARGS sollen in einzelne Argumente zerfallen
./target/release/training_threats $DATA_ARGS -s "$SUPERBATCHES" -t 8 -n "$NAME" $RESUME_ARGS
status "Training fertig"

[ -f "$CKPT/quantised.bin" ] || { status "FEHLER: $CKPT/quantised.bin fehlt"; exit 1; }
size=$(stat -c %s "$CKPT/quantised.bin")
[ "$size" = "$EXPECTED_SIZE" ] || { status "FEHLER: Netzgröße $size != $EXPECTED_SIZE"; exit 1; }

cd "$ROOT"
status "Clean-Build NNUE_ARCH=threats mit neuem Netz"
cp "$CKPT/quantised.bin" "$ROOT/quantised_threats.bin"
make clean && make both NNUE_ARCH=threats
make test NNUE_ARCH=threats
NNUE_FILE="$ROOT/quantised_threats.bin" ./variants.sh create "$VARIANT"
cmp "$CKPT/quantised.bin" "$ROOT/variants/$VARIANT/quantised.bin"

# Engine (Integer) gegen bullets Float-Graph auf denselben FENs
cd "$ROOT/training"
./target/release/training_threats -r "$CKPT/optimiser_state" --eval eval_fens.txt | grep -P '^-?\d+\.\d\t' \
    > "$ROOT/variants/${VARIANT}_ref_evals.txt"
python3 engine_probe.py "$ROOT/variants/$VARIANT/sleepmind" eval_fens.txt 0 > "$ROOT/variants/${VARIANT}_engine_evals.txt"
mean=$(paste "$ROOT/variants/${VARIANT}_ref_evals.txt" "$ROOT/variants/${VARIANT}_engine_evals.txt" \
    | awk -F'\t' '{d=$3-$1; if(d<0)d=-d; s+=d; n++} END{printf "%.1f", s/n}')
status "Engine vs. Float-Referenz: mittlere Abweichung $mean cp"
awk -v m="$mean" 'BEGIN{exit !(m < 30)}' || { status "FEHLER: Abweichung zu groß"; exit 1; }

cd "$ROOT"
make clean && make both
status "Variante $VARIANT angelegt, Repo-Build wieder Legacy"
