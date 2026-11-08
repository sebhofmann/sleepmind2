#!/bin/bash

# Packt Partien des Generators (*.games, eine Partie pro Zeile) in ein
# Viriformat-Binpack für den Trainer (training.rs --viri <datei>).
# Jeder Zug wird beim Packen auf Legalität geprüft; unvollständige Zeilen
# (z. B. von einer abgebrochenen Instanz) werden übersprungen und gezählt.
#
# Altdaten im bulletformat werden separat gefiltert:
#   training/datatools/target/release/filter_material -i alt.dat -o alt_mat4.dat

set -e

usage() {
    echo "Usage: $0 -i <input-folder|datei.games> [-i ...] -o <output.vf>"
    echo ""
    echo "  -i  Ordner (alle *.games darin, rekursiv) oder einzelne .games-Datei"
    echo "  -o  Ausgabedatei (Viriformat-Binpack)"
    exit 1
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATATOOLS="$SCRIPT_DIR/training/datatools"
VIRI_PACK="$DATATOOLS/target/release/viri_pack"

INPUTS=()
while getopts "i:o:h" opt; do
    case $opt in
        i) INPUTS+=("$OPTARG") ;;
        o) OUTPUT_FILE="$OPTARG" ;;
        *) usage ;;
    esac
done
if [ ${#INPUTS[@]} -eq 0 ] || [ -z "$OUTPUT_FILE" ]; then
    usage
fi

if [ ! -x "$VIRI_PACK" ]; then
    echo "Baue datatools ..."
    (cd "$DATATOOLS" && cargo build --release)
fi

shopt -s globstar nullglob
FILES=()
for in in "${INPUTS[@]}"; do
    if [ -d "$in" ]; then
        FILES+=("$in"/**/*.games)
    else
        FILES+=("$in")
    fi
done
if [ ${#FILES[@]} -eq 0 ]; then
    echo "Fehler: keine .games-Dateien gefunden"
    exit 1
fi

echo "Packe ${#FILES[@]} Datei(en) nach $OUTPUT_FILE"
"$VIRI_PACK" -o "$OUTPUT_FILE" "${FILES[@]}"
