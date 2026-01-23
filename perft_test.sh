#!/bin/bash

# Perft Test Suite für sleepmind
# Führt Perft-Tests mit bekannten Positionen und erwarteten Ergebnissen durch
# Positionen >= 1 Mrd. Nodes werden mit "slow" Parameter ausgeführt

ENGINE="./build/sleepmind"
THRESHOLD=1000000000  # 1 Milliarde

# Farben für Output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0
SKIPPED=0
SLOW_MODE=false

# Parse Argumente
while [[ $# -gt 0 ]]; do
    case $1 in
        --slow)
            SLOW_MODE=true
            shift
            ;;
        --fast)
            SLOW_MODE=false
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [--slow] [--fast]"
            echo "  --slow  Führt auch Tests >= 1 Mrd. Nodes aus"
            echo "  --fast  Überspringt Tests >= 1 Mrd. Nodes (Standard)"
            exit 0
            ;;
        *)
            echo "Unbekannte Option: $1"
            exit 1
            ;;
    esac
done

# Prüfe ob Engine existiert
if [ ! -f "$ENGINE" ]; then
    echo -e "${RED}Fehler: Engine nicht gefunden unter $ENGINE${NC}"
    echo "Bitte erst 'make' ausführen."
    exit 1
fi

run_perft() {
    local fen="$1"
    local depth="$2"
    local expected="$3"
    local description="$4"
    
    # Entferne Tausender-Trennzeichen aus expected
    expected=$(echo "$expected" | tr -d ',.')
    
    # Prüfe ob langsamer Test
    if [ "$expected" -ge "$THRESHOLD" ]; then
        if [ "$SLOW_MODE" = false ]; then
            echo -e "${YELLOW}SKIP${NC} $description (Depth $depth): >= 1 Mrd. Nodes - nutze --slow"
            ((SKIPPED++))
            return
        fi
        echo -e "${BLUE}SLOW${NC} $description (Depth $depth): Erwartet $expected Nodes..."
    fi
    
    # Führe Perft aus
    local start_time=$(date +%s%3N)
    local result=$(echo -e "position fen $fen\nperft $depth\nquit" | $ENGINE 2>/dev/null | grep "^perft" | awk '{print $3}')
    local end_time=$(date +%s%3N)
    
    # Berechne Zeit in Millisekunden
    local elapsed_ms=$((end_time - start_time))
    if [ "$elapsed_ms" -le 0 ]; then
        elapsed_ms=1
    fi
    
    if [ -z "$result" ]; then
        echo -e "${RED}FAIL${NC} $description (Depth $depth): Keine Ausgabe von Engine"
        ((FAILED++))
        return
    fi
    
    if [ "$result" -eq "$expected" ]; then
        # Berechne Nodes pro Sekunde (nps = nodes * 1000 / ms)
        local nps=$((result * 1000 / elapsed_ms))
        local elapsed_s=$((elapsed_ms / 1000))
        local elapsed_ms_rest=$((elapsed_ms % 1000))
        echo -e "${GREEN}PASS${NC} $description (Depth $depth): $result Nodes (${elapsed_s}.${elapsed_ms_rest}s, ${nps} nps)"
        ((PASSED++))
    else
        echo -e "${RED}FAIL${NC} $description (Depth $depth): Erwartet $expected, bekommen $result"
        ((FAILED++))
    fi
}

echo "========================================"
echo "       PERFT TEST SUITE"
echo "========================================"
echo "Engine: $ENGINE"
echo "Mode: $([ "$SLOW_MODE" = true ] && echo "SLOW (alle Tests)" || echo "FAST (< 1 Mrd.)")"
echo "========================================"
echo ""

# ============================================
# Position 1: Startposition
# ============================================
echo -e "${BLUE}[Position 1: Startposition]${NC}"
FEN1="rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

run_perft "$FEN1" 1 20 "Startposition"
run_perft "$FEN1" 2 400 "Startposition"
run_perft "$FEN1" 3 8902 "Startposition"
run_perft "$FEN1" 4 197281 "Startposition"
run_perft "$FEN1" 5 4865609 "Startposition"
run_perft "$FEN1" 6 119060324 "Startposition"
run_perft "$FEN1" 7 3195901860 "Startposition"
run_perft "$FEN1" 8 84998978956 "Startposition"

echo ""

# ============================================
# Position 2: Kiwipete
# ============================================
echo -e "${BLUE}[Position 2: Kiwipete]${NC}"
FEN2="r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -"

run_perft "$FEN2" 1 48 "Kiwipete"
run_perft "$FEN2" 2 2039 "Kiwipete"
run_perft "$FEN2" 3 97862 "Kiwipete"
run_perft "$FEN2" 4 4085603 "Kiwipete"
run_perft "$FEN2" 5 193690690 "Kiwipete"
run_perft "$FEN2" 6 8031647685 "Kiwipete"

echo ""

# ============================================
# Position 3: Endspiel
# ============================================
echo -e "${BLUE}[Position 3: Endspiel]${NC}"
FEN3="8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"

run_perft "$FEN3" 1 14 "Endspiel"
run_perft "$FEN3" 2 191 "Endspiel"
run_perft "$FEN3" 3 2812 "Endspiel"
run_perft "$FEN3" 4 43238 "Endspiel"
run_perft "$FEN3" 5 674624 "Endspiel"
run_perft "$FEN3" 6 11030083 "Endspiel"
run_perft "$FEN3" 7 178633661 "Endspiel"
run_perft "$FEN3" 8 3009794393 "Endspiel"

echo ""

# ============================================
# Position 4: Promotions
# ============================================
echo -e "${BLUE}[Position 4: Promotions]${NC}"
FEN4="r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1"

run_perft "$FEN4" 1 6 "Promotions"
run_perft "$FEN4" 2 264 "Promotions"
run_perft "$FEN4" 3 9467 "Promotions"
run_perft "$FEN4" 4 422333 "Promotions"
run_perft "$FEN4" 5 15833292 "Promotions"
run_perft "$FEN4" 6 706045033 "Promotions"

echo ""

# ============================================
# Position 5: Alternative Position
# ============================================
echo -e "${BLUE}[Position 5: Alternative Position]${NC}"
FEN5="rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8"

run_perft "$FEN5" 1 44 "Alternative"
run_perft "$FEN5" 2 1486 "Alternative"
run_perft "$FEN5" 3 62379 "Alternative"
run_perft "$FEN5" 4 2103487 "Alternative"
run_perft "$FEN5" 5 89941194 "Alternative"

echo ""

# ============================================
# Zusammenfassung
# ============================================
echo "========================================"
echo "           ZUSAMMENFASSUNG"
echo "========================================"
echo -e "${GREEN}Bestanden: $PASSED${NC}"
echo -e "${RED}Fehlgeschlagen: $FAILED${NC}"
echo -e "${YELLOW}Übersprungen: $SKIPPED${NC}"
echo "========================================"

if [ $FAILED -gt 0 ]; then
    exit 1
else
    exit 0
fi
