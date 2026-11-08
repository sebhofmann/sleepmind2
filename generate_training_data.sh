#!/bin/bash

# Training Data Generator für SleepMind
# Startet mehrere Instanzen des Training-Programms parallel.
#
# Ausgabe: ganze Partien, eine pro Zeile ("Start-FEN | zug:score ... | Ergebnis"),
# gesammelt in training_combined.games. Umwandlung ins Viriformat für den Trainer:
#   ./pack_training_games.sh -i training_data -o data.vf
# Keine Eval-Adjudication und keine Stellungsfilter mehr: Partien enden nur nach
# den Schachregeln (bzw. exakt per Syzygy), gefiltert wird im Trainer-Loader.

set -e

# Konfiguration
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRAINING_PATH="$SCRIPT_DIR/build/training"
TRAINING_DIR="${TRAINING_DIR:-$SCRIPT_DIR/training_data}"
OUTPUT_FILE="$TRAINING_DIR/training_combined.games"
TEMP_PREFIX="$TRAINING_DIR/training_temp"

# Parameter (können überschrieben werden)
NUM_GAMES=${NUM_GAMES:-10000000}          # Gesamtanzahl Spiele
CONCURRENCY=${CONCURRENCY:-32}          # Anzahl paralleler Instanzen
DEPTH=${DEPTH:-6}                       # Suchtiefe
NODES=${NODES:-5000}                       # Knotenlimit (0=benutze Tiefe)
RANDOM_MOVES=${RANDOM_MOVES:-12}         # Zufallszüge am Anfang
RANDOM_PROB=${RANDOM_PROB:-50}         # Wahrscheinlichkeit für Zufallszüge (%)
MAX_MOVES=${MAX_MOVES:-2000}  # Sicherheitslimit in Halbzügen; Partien, die es erreichen, werden verworfen
DRAW_THRESHOLD=${DRAW_THRESHOLD:-50} # 50-Züge-Regel
EVAL_THRESHOLD=${EVAL_THRESHOLD:-4}  # Max Bewertung in Bauern nach Zufallszügen (0=aus)
VERBOSE=${VERBOSE:-1}  # Verbosity Level
SYZYGY_PATH=${SYZYGY_PATH:-}          # Pfad zu Syzygy-Tablebases (leer=aus)
SYZYGY_PROBE_LIMIT=${SYZYGY_PROBE_LIMIT:-7}  # Max. Steinezahl für TB-Adjudication

# Farben für Output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== SleepMind Training Data Generator ===${NC}"
echo ""

# Prüfe ob Training-Programm existiert
if [ ! -f "$TRAINING_PATH" ]; then
    echo -e "${RED}Fehler: Training-Programm nicht gefunden: $TRAINING_PATH${NC}"
    echo "Bitte zuerst 'make training' ausführen."
    exit 1
fi

# Erstelle Training-Verzeichnis
mkdir -p "$TRAINING_DIR"

# Lösche alte temporäre Dateien
rm -f "$TEMP_PREFIX"_*

# Berechne Spiele pro Instanz
GAMES_PER_INSTANCE=$((NUM_GAMES / CONCURRENCY))
REMAINING_GAMES=$((NUM_GAMES % CONCURRENCY))

echo -e "${YELLOW}Konfiguration:${NC}"
echo "  Training-Programm: $TRAINING_PATH"
echo "  Gesamtspiele:      $NUM_GAMES"
echo "  Parallelität:      $CONCURRENCY Instanzen"
echo "  Spiele/Instanz:    $GAMES_PER_INSTANCE (+$REMAINING_GAMES Rest)"
if [ "$NODES" -gt 0 ]; then
echo "  Suchknoten:        $NODES"
else
echo "  Suchtiefe:         $DEPTH"
fi
echo "  Zufallszüge:       $RANDOM_MOVES (${RANDOM_PROB}%)"
echo "  Zuglimit:          $MAX_MOVES Halbzüge (Partie wird verworfen)"
echo "  50-Züge-Regel:     $DRAW_THRESHOLD"
echo "  Eval-Threshold:    $EVAL_THRESHOLD Bauern (0=aus)"
echo "  Eval-Adjudication: keine"
if [ -n "$SYZYGY_PATH" ]; then
echo "  Syzygy-TB:         $SYZYGY_PATH (bis $SYZYGY_PROBE_LIMIT Steine)"
else
echo "  Syzygy-TB:         aus"
fi
echo "  Output:            $OUTPUT_FILE"
echo ""

# Optionale Syzygy-Argumente (nur wenn ein Pfad gesetzt ist)
SYZYGY_ARGS=()
if [ -n "$SYZYGY_PATH" ]; then
    SYZYGY_ARGS=(-S "$SYZYGY_PATH" -L "$SYZYGY_PROBE_LIMIT")
fi

# Array für PIDs der Hintergrundprozesse
declare -a PIDS

# Funktion zum Kombinieren der Daten
combine_data() {
    echo ""
    echo -e "${YELLOW}Kombiniere Trainingsdaten...${NC}"
    
    # Zähle temporäre Dateien
    TEMP_COUNT=$(ls -1 "${TEMP_PREFIX}"_*.* 2>/dev/null | grep -v '\.log$' | wc -l || echo "0")
    
    if [ "$TEMP_COUNT" -eq 0 ]; then
        echo -e "${RED}Keine Trainingsdaten zum Kombinieren gefunden.${NC}"
        return 1
    fi
    
    echo "Gefundene Trainingsdateien: $TEMP_COUNT"
    
    # Kombiniere alle Partien (eine pro Zeile; Logs ausgenommen)
    local data_files
    data_files=$(ls -1 "${TEMP_PREFIX}"_*.* 2>/dev/null | grep -v '\.log$')
    cat $data_files 2>/dev/null | grep '|' >> "$OUTPUT_FILE"

    NEW_LINES=$(cat $data_files 2>/dev/null | grep -c '|' || true)
    TOTAL_LINES=$(wc -l < "$OUTPUT_FILE" 2>/dev/null || echo "0")

    echo ""
    echo -e "${GREEN}=== Ergebnis ===${NC}"
    echo "  Neue Partien:        $NEW_LINES"
    echo "  Gesamt in Datei:     $TOTAL_LINES Partien"
    echo "  Output-Datei:        $OUTPUT_FILE"
    echo "  Verworfen (Zuglimit): $(grep -h 'Discarded (move limit)' "${TEMP_PREFIX}"_*.log 2>/dev/null | awk '{s+=$NF} END {print s+0}')"

    # Lösche die temporären Datendateien; die Instanz-Logs (mit Summary:
    # Partien, Verworfene, Ø Länge, pos/sec) bleiben bis zum nächsten Start.
    echo ""
    echo "Lösche temporäre Datendateien (Logs bleiben: ${TEMP_PREFIX}_*.log)..."
    rm -f $data_files
    
    return 0
}

# Cleanup-Funktion für Ctrl+C
cleanup() {
    echo ""
    echo -e "${YELLOW}Beende alle Instanzen...${NC}"
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
    # Kurz warten damit Prozesse ihre Daten schreiben können
    sleep 2
    wait 2>/dev/null
    
    echo -e "${YELLOW}Abgebrochen - kombiniere bisherige Daten...${NC}"
    combine_data
    
    echo ""
    echo -e "${GREEN}Fertig (vorzeitig beendet)!${NC}"
    exit 0
}

trap cleanup SIGINT SIGTERM

echo -e "${GREEN}Starte $CONCURRENCY Trainings-Instanzen...${NC}"
echo ""

# Starte parallele Instanzen
for i in $(seq 1 $CONCURRENCY); do
    # Erste Instanzen bekommen ggf. mehr Spiele
    if [ $i -le $REMAINING_GAMES ]; then
        INSTANCE_GAMES=$((GAMES_PER_INSTANCE + 1))
    else
        INSTANCE_GAMES=$GAMES_PER_INSTANCE
    fi
    
    OUTPUT_TEMP="${TEMP_PREFIX}_${i}"
    
    echo -e "${BLUE}  Instanz $i: $INSTANCE_GAMES Spiele -> ${OUTPUT_TEMP}.*${NC}"
    
    # Starte Training im Hintergrund
    # Wenn NODES > 0, benutze Knotenlimit statt Tiefe
    if [ "$NODES" -gt 0 ]; then
        "$TRAINING_PATH" \
            -o "$OUTPUT_TEMP" \
            -n "$INSTANCE_GAMES" \
            -N "$NODES" \
            -r "$RANDOM_MOVES" \
            -p "$RANDOM_PROB" \
            -e "$EVAL_THRESHOLD" \
            --max-moves "$MAX_MOVES" \
            --draw-threshold "$DRAW_THRESHOLD" \
            -v "$VERBOSE" \
            "${SYZYGY_ARGS[@]}" \
            > "${OUTPUT_TEMP}.log" 2>&1 &
    else
        "$TRAINING_PATH" \
            -o "$OUTPUT_TEMP" \
            -n "$INSTANCE_GAMES" \
            -d "$DEPTH" \
            -r "$RANDOM_MOVES" \
            -p "$RANDOM_PROB" \
            -e "$EVAL_THRESHOLD" \
            --max-moves "$MAX_MOVES" \
            --draw-threshold "$DRAW_THRESHOLD" \
            -v "$VERBOSE" \
            "${SYZYGY_ARGS[@]}" \
            > "${OUTPUT_TEMP}.log" 2>&1 &
    fi
    
    PIDS+=($!)
done

echo ""
echo -e "${YELLOW}Warte auf Abschluss aller Instanzen...${NC}"
echo "(Ctrl+C zum Abbrechen)"
echo ""

# Warte auf alle Prozesse und zeige Fortschritt
START_TIME=$(date +%s)
while true; do
    RUNNING=0
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            RUNNING=$((RUNNING + 1))
        fi
    done
    
    if [ $RUNNING -eq 0 ]; then
        break
    fi
    
    # Zeige Status
    ELAPSED=$(($(date +%s) - START_TIME))
    # Partien = Zeilen der Datendateien; Positionen = Summe der letzten Statuszeilen
    TOTAL_GAMES=$(ls -1 "${TEMP_PREFIX}"_*.* 2>/dev/null | grep -v '\.log$' | xargs -r cat | wc -l)
    TOTAL_POS=$(for f in "${TEMP_PREFIX}"_*.log; do grep '^\[Status' "$f" 2>/dev/null | tail -1; done \
        | sed -n 's/.* \([0-9]*\) positions.*/\1/p' | awk '{s+=$1} END {print s+0}')
    if [ $ELAPSED -gt 0 ]; then
        POS_PER_SEC=$((TOTAL_POS / ELAPSED))
    else
        POS_PER_SEC=0
    fi

    printf "\r[%d/%d Instanzen laufen | %d Partien | %d Positionen | %d pos/sec | %ds]     " \
           "$RUNNING" "$CONCURRENCY" "$TOTAL_GAMES" "$TOTAL_POS" "$POS_PER_SEC" "$ELAPSED"
    
    sleep 5
done

echo ""
echo ""
echo -e "${GREEN}Alle Instanzen abgeschlossen!${NC}"
echo ""

# Prüfe Exit-Codes
FAILED=0
for i in "${!PIDS[@]}"; do
    if ! wait "${PIDS[$i]}"; then
        echo -e "${RED}  Instanz $((i+1)) mit Fehler beendet${NC}"
        FAILED=$((FAILED + 1))
    fi
done

if [ $FAILED -gt 0 ]; then
    echo -e "${YELLOW}Warnung: $FAILED Instanz(en) mit Fehlern${NC}"
fi

# Kombiniere die Daten
END_TIME=$(date +%s)
TOTAL_TIME=$((END_TIME - START_TIME))

combine_data

if [ $TOTAL_TIME -gt 0 ]; then
    # NEW_LINES wurde in combine_data gesetzt
    echo "  Laufzeit:            ${TOTAL_TIME}s"
fi

echo ""
echo -e "${GREEN}Fertig!${NC}"
