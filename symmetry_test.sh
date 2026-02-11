#!/bin/bash

# Symmetry Test Suite for sleepmind
# Tests evaluation and search for color bias (white vs black)
# Uses mirrored positions to check if eval/search treats both colors equally

ENGINE="./build/sleepmind"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0
WARNINGS=0

# Tolerance for evaluation differences (in centipawns)
EVAL_TOLERANCE=10  # Allow small NNUE rounding differences
SEARCH_TOLERANCE=30  # Allow some variation in search scores

# Check if engine exists
if [ ! -f "$ENGINE" ]; then
    echo -e "${RED}Error: Engine not found at $ENGINE${NC}"
    echo "Please run 'make' first."
    exit 1
fi

# Function to get evaluation from UCI
get_eval() {
    local fen="$1"
    # Extract the number before "cp" from "info string Evaluation: 40 cp ..."
    local eval_output=$(echo -e "position fen $fen\neval\nquit" | $ENGINE 2>/dev/null | grep "^info string Evaluation:" | awk '{print $4}')
    echo "$eval_output"
}

# Function to get mirrored position
get_mirrored_position() {
    local fen="$1"
    # Extract everything after "info string FEN: "
    local mirrored_fen=$(echo -e "position fen $fen\nflip\nquit" | $ENGINE 2>/dev/null | grep "^info string FEN:" | tail -1 | sed 's/^info string FEN: //')
    echo "$mirrored_fen"
}

# Function to search position and get score
search_position() {
    local fen="$1"
    local depth="$2"
    # Get the score from the last depth iteration
    local search_output=$(echo -e "position fen $fen\ngo depth $depth\nquit" | $ENGINE 2>/dev/null | grep "^info.*depth $depth " | grep "score cp" | tail -1 | sed 's/.*score cp \(-\?[0-9]\+\).*/\1/')
    echo "$search_output"
}

# Test evaluation symmetry
test_eval_symmetry() {
    local fen="$1"
    local description="$2"
    
    # Get evaluation for original position
    local eval1=$(get_eval "$fen")
    
    if [ -z "$eval1" ]; then
        echo -e "${RED}FAIL${NC} $description: Failed to get evaluation for original position"
        ((FAILED++))
        return
    fi
    
    # Get mirrored position
    local mirrored_fen=$(get_mirrored_position "$fen")
    
    if [ -z "$mirrored_fen" ]; then
        echo -e "${RED}FAIL${NC} $description: Failed to mirror position"
        ((FAILED++))
        return
    fi
    
    # Get evaluation for mirrored position
    local eval2=$(get_eval "$mirrored_fen")
    
    if [ -z "$eval2" ]; then
        echo -e "${RED}FAIL${NC} $description: Failed to get evaluation for mirrored position"
        ((FAILED++))
        return
    fi
    
    # Evaluations should be negated (eval1 = -eval2, approximately)
    local sum=$((eval1 + eval2))
    local abs_sum=${sum#-}  # Remove negative sign if present
    
    if [ "$abs_sum" -le "$EVAL_TOLERANCE" ]; then
        echo -e "${GREEN}PASS${NC} Eval Symmetry: $description (orig=$eval1 cp, mirror=$eval2 cp, diff=$sum cp)"
        ((PASSED++))
    elif [ "$abs_sum" -le "$((EVAL_TOLERANCE * 3))" ]; then
        echo -e "${YELLOW}WARN${NC} Eval Symmetry: $description (orig=$eval1 cp, mirror=$eval2 cp, diff=$sum cp)"
        ((WARNINGS++))
    else
        echo -e "${RED}FAIL${NC} Eval Symmetry: $description (orig=$eval1 cp, mirror=$eval2 cp, diff=$sum cp)"
        echo -e "       Original FEN: $fen"
        echo -e "       Mirrored FEN: $mirrored_fen"
        ((FAILED++))
    fi
}

# Test search symmetry
test_search_symmetry() {
    local fen="$1"
    local depth="$2"
    local description="$3"
    
    # Search original position
    local score1=$(search_position "$fen" "$depth")
    
    if [ -z "$score1" ]; then
        echo -e "${RED}FAIL${NC} $description: Failed to search original position"
        ((FAILED++))
        return
    fi
    
    # Get mirrored position
    local mirrored_fen=$(get_mirrored_position "$fen")
    
    if [ -z "$mirrored_fen" ]; then
        echo -e "${RED}FAIL${NC} $description: Failed to mirror position"
        ((FAILED++))
        return
    fi
    
    # Search mirrored position
    local score2=$(search_position "$mirrored_fen" "$depth")
    
    if [ -z "$score2" ]; then
        echo -e "${RED}FAIL${NC} $description: Failed to search mirrored position"
        ((FAILED++))
        return
    fi
    
    # Search scores should be negated (score1 = -score2, approximately)
    local sum=$((score1 + score2))
    local abs_sum=${sum#-}
    
    if [ "$abs_sum" -le "$SEARCH_TOLERANCE" ]; then
        echo -e "${GREEN}PASS${NC} Search Symmetry (d$depth): $description (orig=$score1 cp, mirror=$score2 cp, diff=$sum cp)"
        ((PASSED++))
    elif [ "$abs_sum" -le "$((SEARCH_TOLERANCE * 3))" ]; then
        echo -e "${YELLOW}WARN${NC} Search Symmetry (d$depth): $description (orig=$score1 cp, mirror=$score2 cp, diff=$sum cp)"
        ((WARNINGS++))
    else
        echo -e "${RED}FAIL${NC} Search Symmetry (d$depth): $description (orig=$score1 cp, mirror=$score2 cp, diff=$sum cp)"
        echo -e "       Original FEN: $fen"
        echo -e "       Mirrored FEN: $mirrored_fen"
        ((FAILED++))
    fi
}

echo "========================================"
echo "    SYMMETRY TEST SUITE"
echo "========================================"
echo "Engine: $ENGINE"
echo "Eval Tolerance: $EVAL_TOLERANCE cp"
echo "Search Tolerance: $SEARCH_TOLERANCE cp"
echo "========================================"
echo ""

# ============================================
# Position 1: Starting position
# ============================================
echo -e "${BLUE}[Test 1: Starting Position]${NC}"
FEN1="rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

test_eval_symmetry "$FEN1" "Startpos"
test_search_symmetry "$FEN1" 6 "Startpos"
echo ""

# ============================================
# Position 2: After e4
# ============================================
echo -e "${BLUE}[Test 2: After 1.e4]${NC}"
FEN2="rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1"

test_eval_symmetry "$FEN2" "After e4"
test_search_symmetry "$FEN2" 6 "After e4"
echo ""

# ============================================
# Position 3: Italian Opening
# ============================================
echo -e "${BLUE}[Test 3: Italian Opening]${NC}"
FEN3="r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3"

test_eval_symmetry "$FEN3" "Italian Opening"
test_search_symmetry "$FEN3" 6 "Italian Opening"
echo ""

# ============================================
# Position 4: Open position
# ============================================
echo -e "${BLUE}[Test 4: Open Position]${NC}"
FEN4="r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/3P1N2/PPP2PPP/RNBQK2R w KQkq - 4 5"

test_eval_symmetry "$FEN4" "Open Position"
test_search_symmetry "$FEN4" 6 "Open Position"
echo ""

# ============================================
# Position 5: Middlegame with imbalance
# ============================================
echo -e "${BLUE}[Test 5: Middlegame Imbalance]${NC}"
FEN5="r2qkb1r/ppp2ppp/2n1bn2/3pp3/3PP3/2N2N2/PPPQ1PPP/R1B1KB1R w KQkq - 4 6"

test_eval_symmetry "$FEN5" "Middlegame"
test_search_symmetry "$FEN5" 6 "Middlegame"
echo ""

# ============================================
# Position 6: Endgame - Rook endgame
# ============================================
echo -e "${BLUE}[Test 6: Rook Endgame]${NC}"
FEN6="8/5k2/8/3R4/8/5K2/8/8 w - - 0 1"

test_eval_symmetry "$FEN6" "Rook Endgame"
test_search_symmetry "$FEN6" 8 "Rook Endgame"
echo ""

# ============================================
# Position 7: Pawn endgame
# ============================================
echo -e "${BLUE}[Test 7: Pawn Endgame]${NC}"
FEN7="8/5k2/5p2/4pP2/4P3/5K2/8/8 w - - 0 1"

test_eval_symmetry "$FEN7" "Pawn Endgame"
test_search_symmetry "$FEN7" 10 "Pawn Endgame"
echo ""

# ============================================
# Position 8: Complex tactical position
# ============================================
echo -e "${BLUE}[Test 8: Tactical Position (Kiwipete)]${NC}"
FEN8="r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"

test_eval_symmetry "$FEN8" "Kiwipete"
test_search_symmetry "$FEN8" 5 "Kiwipete"
echo ""

# ============================================
# Position 9: Queen vs Rook endgame
# ============================================
echo -e "${BLUE}[Test 9: Queen vs Rook Endgame]${NC}"
FEN9="8/8/8/3q4/8/8/3R4/3K1k2 w - - 0 1"

test_eval_symmetry "$FEN9" "Q vs R Endgame"
test_search_symmetry "$FEN9" 8 "Q vs R Endgame"
echo ""

# ============================================
# Position 10: Bishop pair vs Knights
# ============================================
echo -e "${BLUE}[Test 10: Bishops vs Knights]${NC}"
FEN10="r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R w KQkq - 0 1"

test_eval_symmetry "$FEN10" "Bishops vs Knights"
test_search_symmetry "$FEN10" 6 "Bishops vs Knights"
echo ""

# ============================================
# Summary
# ============================================
echo "========================================"
echo "        SUMMARY"
echo "========================================"
echo -e "${GREEN}Passed: $PASSED${NC}"
echo -e "${YELLOW}Warnings: $WARNINGS${NC}"
echo -e "${RED}Failed: $FAILED${NC}"
echo "========================================"
echo ""

if [ $FAILED -gt 0 ]; then
    echo -e "${RED}CONCLUSION: Evaluation or search has color bias!${NC}"
    echo "This could explain why black loses more often in self-play."
    exit 1
elif [ $WARNINGS -gt 5 ]; then
    echo -e "${YELLOW}CONCLUSION: Some symmetry issues detected.${NC}"
    echo "Minor asymmetries found - may contribute to color bias."
    exit 0
else
    echo -e "${GREEN}CONCLUSION: No significant color bias detected.${NC}"
    echo "The issue with black losing in self-play is likely elsewhere."
    exit 0
fi
