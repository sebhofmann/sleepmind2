#!/bin/bash
# Kurz-Pipeline zum Datenvergleich (11.11.2026): gleiche Positionszahl aus gen12 (neueste
# Partien) und gen13, je ein Deep-Netz mit nur SBS Superbatches (3 Stufen), danach
# Varianten mit eingebettetem Netz und ein Match mit fester Knotenzahl.
#   gen13.vf        : alle gen13-Partien (79.655.000 Positionen)
#   gen12_newest.vf : 11_11_26_1_1.games + die letzten Partien aus 09_11_26_2_1.games
set -euo pipefail

ROOT=/home/paschty/workspace/sleepmind2
SBS="${SBS:-15}"
GENS="${GENS:-gen13 gen12}"
NODES="${NODES:-30000}"
TAG="${TAG:-}"   # Namenszusatz für Wiederholungsläufe, z. B. _r2
STATUS="$ROOT/variants/datacmp_pipeline.status"
# ARCH=deep (Standard) oder ARCH=legacy mit HL=<Hidden-Größe>, z. B. ARCH=legacy HL=64
ARCH="${ARCH:-deep}"
HL="${HL:-64}"
if [ "$ARCH" = legacy ]; then
    # BATCH=<Positionen pro Batch> (nur legacy-Trainer), Standard 131072
    TRAINER=training; ARCH_ARGS=(--hidden "$HL" ${BATCH:+--batch-size "$BATCH"}); MAKE_ARGS=(NNUE_ARCH=legacy "hl_$HL=1")
    # (768*10*HL + HL) i16 + 8 * (2*HL + 1) i16, auf 64 Bytes aufgefüllt
    EXPECTED_SIZE=$(( ((7680 * HL + HL + 8 * (2 * HL + 1)) * 2 + 63) / 64 * 64 ))
else
    TRAINER=training_deep; ARCH_ARGS=(); MAKE_ARGS=(NNUE_ARCH=deep); EXPECTED_SIZE=19009600
fi

declare -A DATA=( [gen13]="$ROOT/training_data/gen13.vf" [gen12]="$ROOT/training_data/gen12_newest.vf" )

status() { echo "$(date '+%F %T') $*" | tee -a "$STATUS"; }
trap 'status "FEHLER in Zeile $LINENO (exit $?)"' ERR

# FLAT_WDL=0.5: eine einzige Stufe mit konstantem WDL-Anteil (kein Warmup, kein Finetune)
FLAT_WDL="${FLAT_WDL:-}"
if [ -n "$FLAT_WDL" ]; then
    TRAIN_ARGS=(--flat-wdl "$FLAT_WDL"); FINAL="stage0-$SBS"
else
    stage2_sbs=$(( SBS * 2 / 11 )); [ "$stage2_sbs" -ge 1 ] || stage2_sbs=1
    TRAIN_ARGS=(); FINAL="stage2-$stage2_sbs"
fi

cd "$ROOT/training"
cargo build --release --bin "$TRAINER"
for gen in $GENS; do
    name="datacmp_${gen}_s${SBS}${TAG}"
    status "Start Training $name"
    "./target/release/$TRAINER" --viri "${DATA[$gen]}" -s "$SBS" -t 6 -n "$name" --save-rate 5 \
        "${ARCH_ARGS[@]}" "${TRAIN_ARGS[@]}"
    ckpt="$ROOT/training/checkpoints/$name-$FINAL"
    [ -f "$ckpt/quantised.bin" ] || { status "FEHLER: $ckpt/quantised.bin fehlt"; exit 1; }
    size=$(stat -c %s "$ckpt/quantised.bin")
    [ "$size" = "$EXPECTED_SIZE" ] || { status "FEHLER: Netzgröße $size != $EXPECTED_SIZE"; exit 1; }
    status "Training $name fertig"
done

cd "$ROOT"
for gen in $GENS; do
    name="datacmp_${gen}_s${SBS}${TAG}"
    export NNUE_FILE="$ROOT/training/checkpoints/$name-$FINAL/quantised.bin"
    make clean && make both "${MAKE_ARGS[@]}"
    ./variants.sh create "$name"
    status "Variante $name angelegt"
done
unset NNUE_FILE
make clean && make both
status "Repo-Build wieder Standard; starte Match nodes=$NODES"

NODES="$NODES" CONCURRENCY=16 ./tournament.sh sprt "datacmp_gen13_s${SBS}${TAG}" "datacmp_gen12_s${SBS}${TAG}" \
    > "$ROOT/variants/datacmp_gen13_vs_gen12_s${SBS}${TAG}_nodes${NODES}.log" 2>&1
status "Match beendet"
