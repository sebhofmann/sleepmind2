#!/bin/bash
# Unbeaufsichtigte Pipeline (7.11.2026):
#   1. Training 1100 SBs (3 Stufen)  2. Netz -> Variante "stages1100"
#   3. Repo-Build wieder auf main-Netz  4. LTC-SPRT stages1100 vs. baseline
set -euo pipefail

ROOT=/home/paschty/workspace/sleepmind2
VARIANT=stages1100
NET="$ROOT/training/checkpoints/sleepmind-stage2-200/quantised.bin"
STATUS="$ROOT/variants/${VARIANT}_pipeline.status"
EXPECTED_SIZE=$(stat -c %s "$ROOT/variants/baseline/quantised.bin")

status() { echo "$(date '+%F %T') $*" | tee -a "$STATUS"; }
trap 'status "FEHLER in Zeile $LINENO (exit $?)"' ERR

status "Start Training"
cd "$ROOT/training"
./target/release/training -d ../training_data/training_data.dat -s 1100 -t 6
status "Training fertig"

[ -f "$NET" ] || { status "FEHLER: $NET fehlt"; exit 1; }
size=$(stat -c %s "$NET")
[ "$size" = "$EXPECTED_SIZE" ] || { status "FEHLER: Netzgröße $size != $EXPECTED_SIZE"; exit 1; }

cd "$ROOT"
status "Clean-Build mit neuem Netz"
cp "$NET" "$ROOT/quantised.bin"
make clean && make both NNUE_ARCH=legacy
./variants.sh create "$VARIANT"
cmp "$NET" "$ROOT/variants/$VARIANT/quantised.bin"

# Sanity: Eval der Startstellung + kurze Suche, darf nicht crashen
printf 'uci\nisready\nposition startpos\neval\ngo depth 12\n' | timeout 60 "$ROOT/variants/$VARIANT/sleepmind" \
    > "$ROOT/variants/${VARIANT}_sanity.txt" 2>&1 || true
grep -q bestmove "$ROOT/variants/${VARIANT}_sanity.txt" || { status "FEHLER: Sanity-Check ohne bestmove"; exit 1; }
status "Variante $VARIANT angelegt, Sanity ok"

# Repo wieder auf main-Netz zurück (Clean-Build)
cp "$ROOT/variants/baseline/quantised.bin" "$ROOT/quantised.bin"
make clean && make both NNUE_ARCH=legacy
git -C "$ROOT" diff --quiet -- quantised.bin
status "Repo-Build wieder auf main-Netz"

status "Start SPRT LTC 60+0.6, concurrency 16"
TC=60+0.6 CONCURRENCY=16 ./tournament.sh sprt "$VARIANT" baseline > "$ROOT/variants/${VARIANT}_sprt_ltc.log" 2>&1
status "SPRT beendet"
