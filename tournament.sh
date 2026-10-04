#!/bin/bash

set -e

CHECKPOINTS_DIR="training/checkpoints"
ARCHIVE_DIR="archive/gen3"
BUILD_SLEEPMIND="build/sleepmind"
WORKSPACE="/home/paschty/workspace/sleepmind2"
VARIANTS_DIR="$WORKSPACE/variants"

# Runner (fastchess), Eröffnungsbuch (UHO) und Adjudication
source "$(dirname "${BASH_SOURCE[0]}")/testing_common.sh"

# Liest uci_options.txt einer Variante und gibt Engine-Optionen für den Runner aus
# (gleiches Format wie variants.sh: Zeilen "name=value", '#' = Kommentar).
load_uci_opts() {
    local dir="$1" opts="" line n v
    if [ -f "$dir/uci_options.txt" ]; then
        while IFS= read -r line || [ -n "$line" ]; do
            [[ "$line" =~ ^#.*$ ]] && continue
            [[ -z "$line" ]] && continue
            n="${line%%=*}"; v="${line#*=}"
            if [ -n "$n" ] && [ -n "$v" ]; then
                python3 "$WORKSPACE/uci_options.py" "$dir/sleepmind" "$n=$v" || return 2
                opts="$opts option.$n=$v"
            fi
        done < "$dir/uci_options.txt"
    fi
    echo "$opts"
}

# Gemeinsame Test-Einstellungen (per Env überschreibbar)
TC="${TC:-10+0.1}"
CONCURRENCY="${CONCURRENCY:-30}"

# SPRT-Parameter (per Env überschreibbar) - Standard: "ist die neue Version >0 Elo?"
ELO0="${ELO0:-0}"       # H0: kein Gewinn
ELO1="${ELO1:-3}"       # H1: +3 Elo
ALPHA="${ALPHA:-0.05}"  # Falsch-Positiv-Rate
BETA="${BETA:-0.05}"    # Falsch-Negativ-Rate
# Elo-Modell des SPRT (nur fastchess): logistic | normalized | bayesian
SPRT_MODEL="${SPRT_MODEL:-logistic}"

MODE="${1:-tournament}"

usage_sprt() {
    cat <<EOF
SPRT-Modus: zwei Varianten 1-gegen-1, läuft bis PASS/FAIL.

  ./tournament.sh sprt <NEW> [BASE]

  NEW    Name der Variante (Ordner unter variants/), die getestet wird
  BASE   Name der Baseline-Variante (Default: baseline)

  -> wie bei variants.sh nur der Name, nicht der ganze Pfad.
     uci_options.txt der jeweiligen Variante wird automatisch geladen.

Env-Overrides: TC=$TC CONCURRENCY=$CONCURRENCY
               ELO0=$ELO0 ELO1=$ELO1 ALPHA=$ALPHA BETA=$BETA
               SPRT_MODEL=$SPRT_MODEL RUNNER=$RUNNER ADJUDICATE=$ADJUDICATE
               OPENINGS_FILE=$OPENINGS_FILE

Beispiel:
  ./tournament.sh sprt newx
  ELO1=5 TC=8+0.08 CONCURRENCY=24 ./tournament.sh sprt newx baseline
EOF
}

case "$MODE" in
  sprt)
    NEW_NAME="$2"
    BASE_NAME="${3:-baseline}"

    if [ -z "$NEW_NAME" ]; then
        usage_sprt
        exit 1
    fi

    NEW_DIR="$VARIANTS_DIR/$NEW_NAME"
    BASE_DIR="$VARIANTS_DIR/$BASE_NAME"

    if [ ! -x "$NEW_DIR/sleepmind" ]; then
        echo "Fehler: Variante '$NEW_NAME' nicht gefunden ($NEW_DIR/sleepmind)"
        echo "Vorhandene Varianten:"; find "$VARIANTS_DIR" -maxdepth 1 -mindepth 1 -type d -printf '  - %f\n' 2>/dev/null
        exit 1
    fi
    if [ ! -x "$BASE_DIR/sleepmind" ]; then
        echo "Fehler: Baseline-Variante '$BASE_NAME' nicht gefunden ($BASE_DIR/sleepmind)"
        echo "Vorhandene Varianten:"; find "$VARIANTS_DIR" -maxdepth 1 -mindepth 1 -type d -printf '  - %f\n' 2>/dev/null
        exit 1
    fi

    NEW_OPTS=$(load_uci_opts "$NEW_DIR")
    BASE_OPTS=$(load_uci_opts "$BASE_DIR")

    # TC im Dateinamen: die PGN wird über Läufe hinweg angehängt, so mischen
    # sich wenigstens keine Zeitkontrollen in einer Datei.
    PGNOUT="$VARIANTS_DIR/sprt_${NEW_NAME}_vs_${BASE_NAME}_tc${TC}.pgn"

    ensure_book
    setup_match_args
    pgnout_args "$PGNOUT"
    sprt_args "$ELO0" "$ELO1" "$ALPHA" "$BETA"

    echo "=== SPRT ($RUNNER) ==="
    echo "  NEW : $NEW_NAME${NEW_OPTS:+  (uci:$NEW_OPTS)}"
    echo "  BASE: $BASE_NAME${BASE_OPTS:+  (uci:$BASE_OPTS)}"
    echo "  TC=$TC  concurrency=$CONCURRENCY"
    echo "  H0=$ELO0 Elo  H1=$ELO1 Elo  alpha=$ALPHA beta=$BETA  model=$SPRT_MODEL"
    echo "  Buch: $OPENINGS_FILE"
    echo ""

    # -rounds hoch ansetzen: SPRT stoppt selbst, sobald eine Schranke gerissen wird.
    # -games 2 -repeat  => jede Eröffnung mit beiden Farben gespielt (fair).
    # $NEW_OPTS/$BASE_OPTS bewusst unquoted: sollen in einzelne Argumente splitten.
    "$RUNNER_BIN" \
        -engine name="$NEW_NAME" cmd="$NEW_DIR/sleepmind" dir="$NEW_DIR" $NEW_OPTS \
        -engine name="$BASE_NAME" cmd="$BASE_DIR/sleepmind" dir="$BASE_DIR" $BASE_OPTS \
        -each proto=uci tc="$TC" \
        "${SPRT_ARGS[@]}" \
        -games 2 -rounds 50000 -repeat \
        -concurrency "$CONCURRENCY" \
        "${OPENING_ARGS[@]}" \
        "${ADJUDICATION_ARGS[@]}" \
        "${PGNOUT_ARGS[@]}" \
        -ratinginterval 10 \
        "${RUNNER_ARGS[@]}"

    echo ""
    echo "SPRT beendet. PGN: $PGNOUT"
    ;;

  tournament|"")
    # ---- Bestehendes Round-Robin über alle Checkpoints (unverändert) ----
    mkdir -p "$ARCHIVE_DIR"

    ENGINE_ARGS=""

    for checkpoint in "$CHECKPOINTS_DIR"/*; do
        if [ -d "$checkpoint" ]; then
            checkpoint_name=$(basename "$checkpoint")
            target_dir="$ARCHIVE_DIR/$checkpoint_name"

            echo "Verarbeite Checkpoint: $checkpoint_name"

            mkdir -p "$target_dir"

            if [ -f "$checkpoint/quantised.bin" ]; then
                cp "$checkpoint/quantised.bin" "$target_dir/"
            else
                echo "  Warnung: quantised.bin nicht gefunden in $checkpoint"
                continue
            fi

            cp "$BUILD_SLEEPMIND" "$target_dir/"

            ENGINE_ARGS="$ENGINE_ARGS -engine name=$checkpoint_name cmd=$WORKSPACE/$target_dir/sleepmind dir=$WORKSPACE/$target_dir"

            echo "  -> Kopiert nach $target_dir"
        fi
    done

    ENGINE_ARGS="$ENGINE_ARGS -engine name=gen1 cmd=$WORKSPACE/archive/gen1/sleepmind-4/sleepmind dir=$WORKSPACE/archive/gen1/sleepmind-4"
    ENGINE_ARGS="$ENGINE_ARGS -engine name=gen2 cmd=$WORKSPACE/archive/gen2/sleepmind-4/sleepmind dir=$WORKSPACE/archive/gen2/sleepmind-4"
    ENGINE_ARGS="$ENGINE_ARGS -engine name=base_no_net cmd=$WORKSPACE/$BUILD_SLEEPMIND dir=$WORKSPACE"

    echo ""
    echo "Starte Turnier..."
    echo ""

    ensure_book
    setup_match_args
    pgnout_args "$ARCHIVE_DIR/tournament_results.pgn"

    # 3 Eröffnungen je Paarung, jede mit beiden Farben (= 6 Partien)
    "$RUNNER_BIN" \
        $ENGINE_ARGS \
        -each proto=uci tc="$TC" \
        -games 2 -rounds 3 -repeat \
        -concurrency "$CONCURRENCY" \
        "${OPENING_ARGS[@]}" \
        "${ADJUDICATION_ARGS[@]}" \
        "${PGNOUT_ARGS[@]}" \
        "${RUNNER_ARGS[@]}"

    echo ""
    echo "Turnier abgeschlossen! Ergebnisse in $ARCHIVE_DIR/tournament_results.pgn"
    ;;

  *)
    echo "Unbekannter Modus: $MODE"
    echo "  ./tournament.sh            -> Round-Robin (alle Checkpoints)"
    echo "  ./tournament.sh sprt ...   -> SPRT (1-gegen-1)"
    usage_sprt
    exit 1
    ;;
esac
