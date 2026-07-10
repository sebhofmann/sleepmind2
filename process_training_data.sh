#!/bin/bash

# Training Data Processing Script
# Converts, interleaves, and shuffles training data using bullet-utils

set -e

# Usage function
usage() {
    echo "Usage: $0 -b <bullet-utils-path> -i <input-folder> -o <output-file> [-m <memory-mb>] [-r <random-moves>]"
    echo ""
    echo "Options:"
    echo "  -b  Path to bullet-utils executable"
    echo "  -i  Input folder containing .txt training data files"
    echo "  -o  Output file path for the final shuffled .dat file"
    echo "  -m  Memory to use for shuffling in MB (default: 1024)"
    echo "  -r  Drop entries recorded during the random-opening phase:"
    echo "      game ply (derived from the FEN) < N (default: 12, 0 disables)"
    echo ""
    echo "Example:"
    echo "  $0 -b ./target/release/bullet-utils.exe -i ./training_data -o ./final.dat"
    exit 1
}

# Default values
MEMORY_MB=1024
RANDOM_MOVES=12

# Parse arguments
while getopts "b:i:o:m:r:h" opt; do
    case $opt in
        b) BULLET_UTILS="$OPTARG" ;;
        i) INPUT_FOLDER="$OPTARG" ;;
        o) OUTPUT_FILE="$OPTARG" ;;
        m) MEMORY_MB="$OPTARG" ;;
        r) RANDOM_MOVES="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done

# Validate required arguments
if [ -z "$BULLET_UTILS" ] || [ -z "$INPUT_FOLDER" ] || [ -z "$OUTPUT_FILE" ]; then
    echo "Error: Missing required arguments"
    usage
fi

# Check if bullet-utils exists
if [ ! -f "$BULLET_UTILS" ]; then
    echo "Error: bullet-utils not found at $BULLET_UTILS"
    exit 1
fi

# Check if input folder exists
if [ ! -d "$INPUT_FOLDER" ]; then
    echo "Error: Input folder not found: $INPUT_FOLDER"
    exit 1
fi

# Create temp directory for intermediate files
TEMP_DIR=$(mktemp -d)
echo "Using temp directory: $TEMP_DIR"

# Cleanup function
cleanup() {
    echo "Cleaning up temp files..."
    rm -rf "$TEMP_DIR"
}
trap cleanup EXIT

# Step 1: Filter and convert all .txt files to .dat (including subdirectories)
echo "=== Step 1: Filtering and converting .txt files to .dat ==="

# Enable globstar for recursive matching (works in Git Bash on Windows)
shopt -s globstar nullglob

# Drops entries recorded during the random-opening phase. The text format
# ("fen | eval | wdl") carries no ply column, but the game ply follows from
# the FEN: ply = 2*(fullmove-1) + (1 if black to move). Entries with
# ply < RANDOM_MOVES are the oversampled opening positions with noisy
# result labels (older generator versions recorded them; see training_main.c).
filter_random_phase() {
    awk -F' \\| ' -v rm="$RANDOM_MOVES" '
    {
        split($1, f, " ")
        ply = (f[6] - 1) * 2 + (f[2] == "b" ? 1 : 0)
        if (ply >= rm) print
        else dropped++
    }
    END { printf "%d\n", dropped + 0 > "/dev/stderr" }
    '
}

TOTAL_DROPPED=0
DAT_FILES=()
for txt_file in "$INPUT_FOLDER"/**/*.txt; do
    if [ -f "$txt_file" ]; then
        # Create unique name using relative path (replace / and \ with _)
        rel_path="${txt_file#$INPUT_FOLDER/}"
        safe_name=$(echo "$rel_path" | sed 's/[\/\\]/_/g' | sed 's/\.txt$//')
        dat_file="$TEMP_DIR/${safe_name}.dat"
        if [ "$RANDOM_MOVES" -gt 0 ]; then
            filtered_file="$TEMP_DIR/${safe_name}.filtered.txt"
            dropped=$(filter_random_phase < "$txt_file" 2>&1 >"$filtered_file")
            TOTAL_DROPPED=$((TOTAL_DROPPED + dropped))
            echo "Filtering:  $txt_file -> dropped $dropped entries with ply < $RANDOM_MOVES"
            convert_input="$filtered_file"
        else
            convert_input="$txt_file"
        fi
        echo "Converting: $txt_file -> $dat_file"
        "$BULLET_UTILS" convert --from text --input "$convert_input" --output "$dat_file"
        if [ "$RANDOM_MOVES" -gt 0 ]; then
            # Free the filtered copy right away; with 20+ GB of input the
            # temp dir would otherwise hold everything twice.
            rm -f "$filtered_file"
        fi
        DAT_FILES+=("$dat_file")
    fi
done

if [ "$RANDOM_MOVES" -gt 0 ]; then
    echo "Dropped $TOTAL_DROPPED random-phase entries in total (ply < $RANDOM_MOVES)"
fi

if [ ${#DAT_FILES[@]} -eq 0 ]; then
    echo "Error: No .txt files found in $INPUT_FOLDER (including subdirectories)"
    exit 1
fi

echo "Converted ${#DAT_FILES[@]} file(s) from $INPUT_FOLDER (including subdirectories)"

# Step 2: Interleave all .dat files if more than one
echo ""
echo "=== Step 2: Interleaving .dat files ==="
if [ ${#DAT_FILES[@]} -eq 1 ]; then
    echo "Only one file, skipping interleave"
    COMBINED_FILE="${DAT_FILES[0]}"
else
    COMBINED_FILE="$TEMP_DIR/combined.dat"
    echo "Interleaving ${#DAT_FILES[@]} files..."
    "$BULLET_UTILS" interleave "${DAT_FILES[@]}" --output "$COMBINED_FILE"
fi

# Step 3: Shuffle the combined file
echo ""
echo "=== Step 3: Shuffling data ==="
echo "Shuffling with ${MEMORY_MB}MB memory..."
"$BULLET_UTILS" shuffle --input "$COMBINED_FILE" --output "$OUTPUT_FILE" --mem-used-mb "$MEMORY_MB"

echo ""
echo "=== Done! ==="
echo "Output file: $OUTPUT_FILE"
