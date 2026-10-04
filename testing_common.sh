#!/bin/bash
# Gemeinsame Test-Einstellungen für tournament.sh und variants.sh (wird per
# `source` eingebunden): Match-Runner, Eröffnungsbuch und Adjudication.
#
# Env-Overrides:
#   RUNNER=fastchess|cutechess   Match-Runner (Default: fastchess)
#   OPENINGS_FILE=<pfad>         eigenes Buch (.epd oder .pgn) statt UHO
#   ADJUDICATE=0                 Remis-/Aufgabe-Adjudication abschalten

TESTING_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RUNNER="${RUNNER:-fastchess}"
ADJUDICATE="${ADJUDICATE:-1}"

# Unausgeglichenes Buch (UHO): deutlich weniger Remis als 2moves_v2, dadurch
# trägt jedes Partiepaar mehr Information. Wird bei Bedarf heruntergeladen.
BOOKS_DIR="$TESTING_ROOT/books"
UHO_NAME="UHO_Lichess_4852_v1.epd"
UHO_URL="https://github.com/official-stockfish/books/raw/master/$UHO_NAME.zip"
UHO_ZIP_SHA256="4e298f11e8acfa106babe02968f2e61582145e7874c59284690b20b9650e0e07"
OPENINGS_FILE="${OPENINGS_FILE:-$BOOKS_DIR/$UHO_NAME}"

case "$RUNNER" in
    fastchess) RUNNER_BIN="fastchess" ;;
    cutechess) RUNNER_BIN="cutechess-cli" ;;
    *)
        echo "Fehler: unbekannter RUNNER '$RUNNER' (fastchess|cutechess)" >&2
        exit 1
        ;;
esac

# Lädt das UHO-Buch nach books/, falls es fehlt. Ein per OPENINGS_FILE
# gesetztes eigenes Buch muss bereits existieren.
ensure_book() {
    [ -f "$OPENINGS_FILE" ] && return 0

    if [ "$OPENINGS_FILE" != "$BOOKS_DIR/$UHO_NAME" ]; then
        echo "Fehler: Eröffnungsbuch nicht gefunden: $OPENINGS_FILE" >&2
        return 1
    fi

    local zip="$BOOKS_DIR/$UHO_NAME.zip"
    mkdir -p "$BOOKS_DIR"
    echo "Lade Eröffnungsbuch $UHO_NAME ..." >&2
    if ! curl -fL --retry 3 -o "$zip.part" "$UHO_URL"; then
        rm -f "$zip.part"
        echo "Fehler: Download fehlgeschlagen: $UHO_URL" >&2
        return 1
    fi
    if ! echo "$UHO_ZIP_SHA256  $zip.part" | sha256sum -c --quiet - >&2; then
        rm -f "$zip.part"
        echo "Fehler: Prüfsumme von $UHO_NAME.zip stimmt nicht" >&2
        return 1
    fi
    mv "$zip.part" "$zip"
    unzip -o -q "$zip" "$UHO_NAME" -d "$BOOKS_DIR"
    rm -f "$zip"
    echo "Eröffnungsbuch bereit: $OPENINGS_FILE" >&2
}

# Setzt die Arrays OPENING_ARGS, ADJUDICATION_ARGS und RUNNER_ARGS für den
# gewählten Runner. Nach ensure_book aufrufen.
setup_match_args() {
    local format="epd"
    [[ "$OPENINGS_FILE" == *.pgn ]] && format="pgn"
    OPENING_ARGS=(-openings "file=$OPENINGS_FILE" "format=$format" order=random)

    # Entschiedene Partien früh beenden - spart vor allem bei LTC viel Zeit.
    ADJUDICATION_ARGS=()
    if [ "$ADJUDICATE" != "0" ]; then
        ADJUDICATION_ARGS=(
            -draw movenumber=40 movecount=8 score=10
            -resign movecount=3 score=1000
        )
    fi

    # fastchess schreibt sonst alle 20 Partien eine config.json ins
    # Arbeitsverzeichnis.
    RUNNER_ARGS=(-recover)
    [ "$RUNNER" = "fastchess" ] && RUNNER_ARGS+=(-autosaveinterval 0)
    return 0
}

# Setzt das Array PGNOUT_ARGS (die Syntax unterscheidet sich je Runner).
pgnout_args() {
    if [ "$RUNNER" = "fastchess" ]; then
        PGNOUT_ARGS=(-pgnout "file=$1")
    else
        PGNOUT_ARGS=(-pgnout "$1")
    fi
}

# Setzt das Array SPRT_ARGS. fastchess wertet Partiepaare pentanomial aus;
# cutechess kennt nur das trinomiale logistische Modell.
sprt_args() {
    SPRT_ARGS=(-sprt "elo0=$1" "elo1=$2" "alpha=$3" "beta=$4")
    [ "$RUNNER" = "fastchess" ] && SPRT_ARGS+=("model=$SPRT_MODEL")
    return 0
}
