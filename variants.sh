#!/bin/bash

set -e

WORKSPACE="/home/paschty/workspace/sleepmind2"
VARIANTS_DIR="$WORKSPACE/variants"
BUILD_SLEEPMIND="$WORKSPACE/build/sleepmind"
NNUE_FILE="$WORKSPACE/quantised.bin"
OPENINGS_FILE="/home/paschty/Downloads/klo_eco_a00-e97v/klo_250_eco_a00-e97_variations.pgn"

# Erstelle variants Ordner falls nicht vorhanden
mkdir -p "$VARIANTS_DIR"

# =============================================================================
# Funktion 1: Variante erstellen
# Usage: ./variants.sh create <name>
# =============================================================================
create_variant() {
    local name="$1"
    
    if [ -z "$name" ]; then
        echo "Fehler: Kein Name angegeben!"
        echo "Usage: $0 create <name>"
        exit 1
    fi
    
    local target_dir="$VARIANTS_DIR/$name"
    
    if [ -d "$target_dir" ]; then
        echo "Fehler: Variante '$name' existiert bereits!"
        echo "Lösche zuerst mit: rm -rf $target_dir"
        exit 1
    fi
    
    # Prüfe ob Build existiert
    if [ ! -f "$BUILD_SLEEPMIND" ]; then
        echo "Fehler: $BUILD_SLEEPMIND nicht gefunden! Bitte erst 'make' ausführen."
        exit 1
    fi
    
    # Erstelle Zielordner
    mkdir -p "$target_dir"
    
    # Kopiere Executable
    cp "$BUILD_SLEEPMIND" "$target_dir/sleepmind"
    
    # Kopiere NNUE falls vorhanden
    if [ -f "$NNUE_FILE" ]; then
        cp "$NNUE_FILE" "$target_dir/quantised.bin"
        echo "Variante '$name' erstellt mit NNUE in: $target_dir"
    else
        echo "Warnung: Keine NNUE-Datei gefunden ($NNUE_FILE)"
        echo "Variante '$name' erstellt ohne NNUE in: $target_dir"
    fi
    
    ls -la "$target_dir"
}

# =============================================================================
# Funktion 2: Turnier zwischen allen Varianten
# Usage: ./variants.sh tournament <games_per_pair>
# =============================================================================
run_tournament() {
    local games="${1:-10}"  # Default: 10 Spiele pro Paarung
    
    # Sammle alle Varianten
    local ENGINE_ARGS=""
    local count=0
    
    for variant_dir in "$VARIANTS_DIR"/*; do
        if [ -d "$variant_dir" ] && [ -f "$variant_dir/sleepmind" ]; then
            local name=$(basename "$variant_dir")
            ENGINE_ARGS="$ENGINE_ARGS -engine name=$name cmd=$variant_dir/sleepmind dir=$variant_dir"
            count=$((count + 1))
            echo "Gefunden: $name"
        fi
    done
    
    if [ $count -lt 2 ]; then
        echo "Fehler: Mindestens 2 Varianten benötigt für ein Turnier!"
        echo "Aktuelle Varianten: $count"
        echo "Erstelle Varianten mit: $0 create <name>"
        exit 1
    fi
    
    echo ""
    echo "Starte Turnier mit $count Engines, $games Spiele pro Paarung..."
    echo ""
    
    local result_file="$VARIANTS_DIR/tournament_$(date +%Y%m%d_%H%M%S).pgn"
    
    # Starte das Turnier
    cutechess-cli \
        $ENGINE_ARGS \
        -each proto=uci tc=10+0.1 \
        -games "$games" \
        -rounds 1 \
        -concurrency 4 \
        -openings file="$OPENINGS_FILE" format=pgn order=random \
        -pgnout "$result_file" \
        -recover \
        -repeat
    
    echo ""
    echo "Turnier abgeschlossen! Ergebnisse in: $result_file"
}

# =============================================================================
# Funktion 3: Liste alle Varianten
# Usage: ./variants.sh list
# =============================================================================
list_variants() {
    echo "Vorhandene Varianten in $VARIANTS_DIR:"
    echo ""
    
    for variant_dir in "$VARIANTS_DIR"/*; do
        if [ -d "$variant_dir" ]; then
            local name=$(basename "$variant_dir")
            local has_nnue="ohne NNUE"
            if [ -f "$variant_dir/quantised.bin" ]; then
                has_nnue="mit NNUE"
            fi
            echo "  - $name ($has_nnue)"
        fi
    done
}

# =============================================================================
# Main
# =============================================================================
case "${1:-}" in
    create)
        create_variant "$2"
        ;;
    tournament)
        run_tournament "$2"
        ;;
    list)
        list_variants
        ;;
    *)
        echo "Usage: $0 <command> [args]"
        echo ""
        echo "Commands:"
        echo "  create <name>       - Erstelle neue Variante aus aktuellem Build"
        echo "  tournament [games]  - Starte Turnier (default: 10 Spiele pro Paarung)"
        echo "  list                - Zeige alle Varianten"
        echo ""
        echo "Beispiel:"
        echo "  $0 create baseline"
        echo "  $0 create optimized"
        echo "  $0 tournament 20"
        ;;
esac
