#!/bin/bash

# Training Data Generator für SleepMind
# Startet mehrere Instanzen des Training-Programms parallel

set -e

# Konfiguration
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRAINING_PATH="$SCRIPT_DIR/build/training"
TRAINING_DIR="$SCRIPT_DIR/training_data"
OUTPUT_FILE="$TRAINING_DIR/training_combined.txt"
TEMP_PREFIX="$TRAINING_DIR/training_temp"

# Parameter (können überschrieben werden)
NUM_GAMES=${NUM_GAMES:-1000000}          # Gesamtanzahl Spiele
CONCURRENCY=${CONCURRENCY:-32}          # Anzahl paralleler Instanzen
DEPTH=${DEPTH:-8}                       # Suchtiefe
RANDOM_MOVES=${RANDOM_MOVES:-10}         # Zufallszüge am Anfang
RANDOM_PROB=${RANDOM_PROB:-100}         # Wahrscheinlichkeit für Zufallszüge (%)
MAX_MOVES=${MAX_MOVES:-200}  # Maximale Züge pro Spiel
DRAW_THRESHOLD=${DRAW_THRESHOLD:-100} # 50-Züge-Regel
EVAL_THRESHOLD=${EVAL_THRESHOLD:-1}  # Max Bewertung in Bauern nach Zufallszügen (0=aus)
ADJUDICATE=${ADJUDICATE:-10} # Spiel beenden bei +/- N Bauern (0=aus)
FILTER_TACTICS=${FILTER_TACTICS:-1}   # Taktische Positionen filtern (0=aus)
VERBOSE=${VERBOSE:-1}  # Verbosity Level

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
echo "  Suchtiefe:         $DEPTH"
echo "  Zufallszüge:       $RANDOM_MOVES (${RANDOM_PROB}%)"
echo "  Max Züge/Spiel:    $MAX_MOVES"
echo "  Draw-Threshold:    $DRAW_THRESHOLD"
echo "  Eval-Threshold:    $EVAL_THRESHOLD Bauern (0=aus)"
echo "  Adjudicate bei:    $ADJUDICATE Bauern (0=aus)"
echo "  Filter Taktik:     $FILTER_TACTICS (1=an, 0=aus)"
echo "  Output:            $OUTPUT_FILE"
echo ""

# Array für PIDs der Hintergrundprozesse
declare -a PIDS

# Funktion zum Kombinieren der Daten
combine_data() {
    echo ""
    echo -e "${YELLOW}Kombiniere Trainingsdaten...${NC}"
    
    # Zähle temporäre Dateien
    TEMP_COUNT=$(ls -1 "${TEMP_PREFIX}"_*.* 2>/dev/null | grep -v '.log$' | wc -l || echo "0")
    
    if [ "$TEMP_COUNT" -eq 0 ]; then
        echo -e "${RED}Keine Trainingsdaten zum Kombinieren gefunden.${NC}"
        return 1
    fi
    
    echo "Gefundene Trainingsdateien: $TEMP_COUNT"
    
    # Kombiniere alle Trainingsdaten
    cat "${TEMP_PREFIX}"_*.* 2>/dev/null | grep -v '^$' | grep '|' >> "$OUTPUT_FILE"
    
    # Zähle Zeilen
    NEW_LINES=$(cat "${TEMP_PREFIX}"_*.* 2>/dev/null | grep -v '^$' | grep '|' | wc -l)
    TOTAL_LINES=$(wc -l < "$OUTPUT_FILE" 2>/dev/null || echo "0")
    
    echo ""
    echo -e "${GREEN}=== Ergebnis ===${NC}"
    echo "  Neue Positionen:     $NEW_LINES"
    echo "  Gesamt in Datei:     $TOTAL_LINES"
    echo "  Output-Datei:        $OUTPUT_FILE"
    
    # Lösche temporäre Dateien
    echo ""
    echo "Lösche temporäre Dateien..."
    rm -f "${TEMP_PREFIX}"_*
    
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
    "$TRAINING_PATH" \
        -o "$OUTPUT_TEMP" \
        -n "$INSTANCE_GAMES" \
        -d "$DEPTH" \
        -r "$RANDOM_MOVES" \
        -p "$RANDOM_PROB" \
        -e "$EVAL_THRESHOLD" \
        -a "$ADJUDICATE" \
        -f "$FILTER_TACTICS" \
        --max-moves "$MAX_MOVES" \
        --draw-threshold "$DRAW_THRESHOLD" \
        -v "$VERBOSE" \
        > "${OUTPUT_TEMP}.log" 2>&1 &
    
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
    TOTAL_POS=$(cat "${TEMP_PREFIX}"_*.* 2>/dev/null | wc -l || echo "0")
    if [ $ELAPSED -gt 0 ]; then
        POS_PER_SEC=$((TOTAL_POS / ELAPSED))
    else
        POS_PER_SEC=0
    fi
    
    printf "\r[%d/%d Instanzen laufen | %d Positionen | %d pos/sec | %ds]     " \
           "$RUNNING" "$CONCURRENCY" "$TOTAL_POS" "$POS_PER_SEC" "$ELAPSED"
    
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
    NEW_LINES=$(cat "${TEMP_PREFIX}"_*.* 2>/dev/null | grep -v '^$' | grep '|' | wc -l 2>/dev/null || echo "0")
    echo "  Laufzeit:            ${TOTAL_TIME}s"
fi

echo ""
echo -e "${GREEN}Fertig!${NC}"
