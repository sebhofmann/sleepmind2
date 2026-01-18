#!/bin/bash

set -e

WORKSPACE="/home/paschty/workspace/sleepmind2"
VARIANTS_DIR="$WORKSPACE/variants"
BUILD_SLEEPMIND="$WORKSPACE/build/sleepmind"
NNUE_FILE="$WORKSPACE/quantised.bin"
OPENINGS_FILE="/home/paschty/Downloads/klo_eco_a00-e97v/klo_25_eco_a00-e97_variations.pgn"

# Erstelle variants Ordner falls nicht vorhanden
mkdir -p "$VARIANTS_DIR"

# =============================================================================
# Funktion 1: Variante erstellen
# Usage: ./variants.sh create <name> [option1=value1] [option2=value2] ...
# =============================================================================
create_variant() {
    local name="$1"
    shift  # Entferne den Namen, Rest sind UCI-Optionen
    
    if [ -z "$name" ]; then
        echo "Fehler: Kein Name angegeben!"
        echo "Usage: $0 create <name> [option1=value1] [option2=value2] ..."
        echo ""
        echo "Verfügbare UCI-Optionen:"
        echo "  Use_LMR=true/false"
        echo "  Use_NullMove=true/false"
        echo "  Use_Futility=true/false"
        echo "  Use_RFP=true/false"
        echo "  Use_DeltaPruning=true/false"
        echo "  Use_Aspiration=true/false"
        echo "  LMR_FullDepthMoves=<1-10>"
        echo "  LMR_ReductionLimit=<1-6>"
        echo "  NullMove_Reduction=<1-5>"
        echo "  NullMove_MinDepth=<1-6>"
        echo "  Futility_Margin=<50-400>"
        echo "  Futility_MarginD2=<100-600>"
        echo "  Futility_MarginD3=<150-800>"
        echo "  RFP_Margin=<50-300>"
        echo "  RFP_MaxDepth=<2-10>"
        echo "  Delta_Margin=<50-500>"
        echo "  Aspiration_Window=<10-200>"
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
    
    # Speichere UCI-Optionen falls angegeben
    local uci_file="$target_dir/uci_options.txt"
    if [ $# -gt 0 ]; then
        echo "# UCI-Optionen für Variante: $name" > "$uci_file"
        echo "# Erstellt am: $(date)" >> "$uci_file"
        echo "" >> "$uci_file"
        
        for option in "$@"; do
            # Prüfe ob das Format name=value ist
            if [[ "$option" == *"="* ]]; then
                echo "$option" >> "$uci_file"
                echo "  UCI-Option: $option"
            else
                echo "Warnung: Ungültiges Format '$option' (erwartet: name=value)"
            fi
        done
        
        echo ""
        echo "UCI-Optionen gespeichert in: $uci_file"
    fi
    
    echo ""
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
            local uci_options=""
            
            # Lade UCI-Optionen falls vorhanden
            if [ -f "$variant_dir/uci_options.txt" ]; then
                while IFS= read -r line || [ -n "$line" ]; do
                    # Überspringe Kommentare und leere Zeilen
                    [[ "$line" =~ ^#.*$ ]] && continue
                    [[ -z "$line" ]] && continue
                    
                    # Extrahiere name und value
                    local opt_name="${line%%=*}"
                    local opt_value="${line#*=}"
                    
                    if [ -n "$opt_name" ] && [ -n "$opt_value" ]; then
                        uci_options="$uci_options option.$opt_name=$opt_value"
                    fi
                done < "$variant_dir/uci_options.txt"
            fi
            
            ENGINE_ARGS="$ENGINE_ARGS -engine name=$name cmd=$variant_dir/sleepmind dir=$variant_dir$uci_options"
            count=$((count + 1))
            
            if [ -n "$uci_options" ]; then
                echo "Gefunden: $name (mit UCI-Optionen)"
            else
                echo "Gefunden: $name"
            fi
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
        -each proto=uci tc=10+0.2 \
        -games "$games" \
        -rounds 1 \
        -concurrency 32 \
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
            
            # Zeige UCI-Optionen falls vorhanden
            if [ -f "$variant_dir/uci_options.txt" ]; then
                while IFS= read -r line || [ -n "$line" ]; do
                    # Überspringe Kommentare und leere Zeilen
                    [[ "$line" =~ ^#.*$ ]] && continue
                    [[ -z "$line" ]] && continue
                    echo "      $line"
                done < "$variant_dir/uci_options.txt"
            fi
        fi
    done
}

# =============================================================================
# Main
# =============================================================================
case "${1:-}" in
    create)
        shift
        create_variant "$@"
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
        echo "  create <name> [options]  - Erstelle neue Variante aus aktuellem Build"
        echo "  tournament [games]       - Starte Turnier (default: 10 Spiele pro Paarung)"
        echo "  list                     - Zeige alle Varianten"
        echo ""
        echo "Beispiele:"
        echo "  $0 create baseline"
        echo "  $0 create no_lmr Use_LMR=false"
        echo "  $0 create aggressive_pruning Futility_Margin=200 RFP_Margin=150"
        echo "  $0 create tuned LMR_FullDepthMoves=3 NullMove_Reduction=4"
        echo "  $0 tournament 20"
        echo ""
        echo "Verfügbare UCI-Optionen:"
        echo "  Feature Flags:    Use_LMR, Use_NullMove, Use_Futility, Use_RFP,"
        echo "                    Use_DeltaPruning, Use_Aspiration (=true/false)"
        echo "  LMR:              LMR_FullDepthMoves (1-10), LMR_ReductionLimit (1-6)"
        echo "  Null Move:        NullMove_Reduction (1-5), NullMove_MinDepth (1-6)"
        echo "  Futility:         Futility_Margin (50-400), Futility_MarginD2 (100-600),"
        echo "                    Futility_MarginD3 (150-800)"
        echo "  RFP:              RFP_Margin (50-300), RFP_MaxDepth (2-10)"
        echo "  QSearch:          Delta_Margin (50-500)"
        echo "  Aspiration:       Aspiration_Window (10-200)"
        ;;
esac
