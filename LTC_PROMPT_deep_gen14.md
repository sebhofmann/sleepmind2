# Auftrag: LTC-SPRT für das Netz deep_gen14

Dieser Branch (`deep-gen14-net`) unterscheidet sich von `main` (91bcc74) **nur** durch das
Netz `quantised_deep.bin` (deep_gen14) und diese Datei. Bitte einen LTC-SPRT
„deep_gen14 gegen main“ auf diesem Rechner durchführen und das Ergebnis berichten.

## Hintergrund

- Neues Deep-Netz (gleiche Architektur 768x16 -> 768 pairwise -> 16 -> 32 -> 1, 8 Buckets),
  trainiert am 21.11.2026 mit 1100 Superbatches (100/800/200) auf Altdaten (`training_data_mat4.dat`)
  + gen12 + gen13 (alle 18 Dateien) + gen14 (13 Dateien), proportional gemischt (51,5 % viriformat).
- Basis-Netz auf `main`: deep_gen13.
- Erwartete SHA-256:
  - Kandidat (`quantised_deep.bin` auf diesem Branch): `d48d77832829ded318fe31b13a782dc18415b60270500ec851b37df7ded9e0eb`
  - Basis (`quantised_deep.bin` auf `main` 91bcc74): `ad49e2022b78e4405be8b7e11283935943e4e914e036dd48d835893764c0c469`
- Bisherige Ergebnisse auf dem Hauptrechner:
  - Fixed Nodes 30k: **+9,43 ± 5,57**, 6044 Partien, H1 akzeptiert (LLR 2,95).
  - LTC 60+0.6 (abgebrochen, nur Richtwert): +17,70 ± 18,89 nach 334 Partien, LLR 0,51.
    Bei einem echten Gewinn um +10 Elo werden voraussichtlich 4000–5000 Partien nötig.

## Vorgehen

1. **Pfad prüfen:** `tournament.sh` und `variants.sh` haben
   `WORKSPACE="/home/paschty/workspace/sleepmind2"` fest eingetragen. Liegt das Repo auf diesem
   Rechner woanders, die Variable in beiden Skripten **lokal** anpassen (nicht committen).
   Voraussetzungen: `gcc`, `make`, `python3`, `fastchess` im `PATH`, `curl`/`unzip` (das UHO-Buch
   wird beim ersten Lauf automatisch nach `books/` geladen).
2. **Basis bauen** (immer Clean-Builds, keine inkrementellen):
   ```bash
   git checkout main            # muss 91bcc74 oder ein Commit mit identischem Engine-Code/Netz sein
   make clean && make both && make test
   ./variants.sh create main_91bcc74
   ```
3. **Kandidat bauen:**
   ```bash
   git checkout deep-gen14-net
   make clean && make both && make test
   ./variants.sh create deep_gen14
   ```
4. **Netze prüfen:** `sha256sum variants/*/quantised.bin` muss die beiden Hashes oben ergeben.
   Optional einen Sanity-Check: beide Binaries mit `bench` (falls vorhanden) bzw. per UCI starten.
5. **LTC-SPRT starten** — als `systemd-run --user`-Unit, damit der Lauf nicht mit der
   Terminal-/Claude-Sitzung stirbt (`setsid`/`nohup` reicht nicht):
   ```bash
   systemd-run --user --unit=deep-gen14-ltc --working-directory="$PWD" \
     bash -c 'TC=60+0.6 CONCURRENCY=<N> ./tournament.sh sprt deep_gen14 main_91bcc74 \
       > variants/deep_gen14_vs_main_91bcc74_sprt_ltc.log 2>&1'
   ```
   - SPRT-Grenzen sind die Defaults von `tournament.sh`: elo0=0, elo1=3, alpha=beta=0,05,
     logistic. **Nicht ändern**, sonst sind die Ergebnisse nicht mit früheren LTCs vergleichbar.
   - `CONCURRENCY` = Anzahl physischer Kerne minus 1–2 (jede Partie braucht 2 Engines mit
     je 1 Thread; Hyperthreads nicht doppelt zählen, Turbo/Boost sollte aktiv sein).
   - Adjudication nicht verändern (`ADJUDICATE` bleibt Default).
6. **Überwachen:** `grep -aE '^(Elo|LLR|Games)' variants/deep_gen14_vs_main_91bcc74_sprt_ltc.log | tail -3`.
   Der Lauf endet von selbst bei LLR ±2,94.

## Ergebnis berichten

- Endstand (Elo ± Fehler, nElo, Partien, W/L/D, LLR, H0/H1) und Zeitraum.
- Rechner (CPU, Concurrency), Commit-Hashes beider Builds.
- PGN liegt unter `variants/sprt_deep_gen14_vs_main_91bcc74_tc60+0.6.pgn` und das Log unter
  `variants/deep_gen14_vs_main_91bcc74_sprt_ltc.log`. Beide nicht committen, aber aufbewahren.
- **Nicht** nach `main` mergen und keinen PR öffnen. Darüber entscheidet der User nach dem Ergebnis.
