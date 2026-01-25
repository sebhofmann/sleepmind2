# SleepMind Ansible Training

Ansible-Playbooks zur Generierung von Trainingsdaten auf mehreren Ubuntu-Maschinen.

## Playbooks

| Playbook | Beschreibung |
|----------|--------------|
| `start_training.yml` | Startet Training im Hintergrund |
| `stop_training.yml` | Beendet Training und sichert Daten |
| `status_training.yml` | Zeigt aktuellen Status |

## Voraussetzungen

- Ansible auf dem Control-Node installiert (`pip install ansible`)
- SSH-Zugang zu den Ubuntu-Maschinen
- Git-Zugang zum Repository (SSH-Key oder HTTPS)

## Setup

1. **Inventory erstellen:**
   ```bash
   cp inventory.example.yml inventory.yml
   vim inventory.yml
   ```
   Trage deine Ubuntu-Maschinen und Repository-URL ein.

2. **SSH-Keys verteilen (falls nötig):**
   ```bash
   ssh-copy-id ubuntu@training1
   ```

## Verwendung

```bash
# Teste Verbindung zu allen Nodes
ansible -i inventory.yml training_nodes -m ping

# Training starten
ansible-playbook -i inventory.yml start_training.yml

# Status prüfen
ansible-playbook -i inventory.yml status_training.yml

# Training stoppen und Daten sichern
ansible-playbook -i inventory.yml stop_training.yml
```

### Mit angepassten Parametern

```bash
ansible-playbook -i inventory.yml start_training.yml \
  -e "training_num_games=5000000" \
  -e "training_nodes=10000" \
  -e "training_concurrency=16"
```

### Nur auf bestimmten Hosts

```bash
ansible-playbook -i inventory.yml start_training.yml --limit training1,training2
```

## Parameter

Alle Parameter können in `inventory.yml` oder per `-e` übergeben werden:

| Variable | Default | Beschreibung |
|----------|---------|--------------|
| `repo_url` | - | Git Repository URL |
| `repo_branch` | main | Git Branch |
| `training_num_games` | 1000000 | Anzahl zu generierende Spiele |
| `training_concurrency` | (auto) | Parallele Instanzen (Standard: CPU-Anzahl) |
| `training_depth` | 6 | Suchtiefe (wenn nodes=0) |
| `training_nodes` | 5000 | Suchknoten pro Zug (0=benutze Tiefe) |
| `training_random_moves` | 12 | Zufallszüge am Anfang |
| `training_random_prob` | 100 | Wahrscheinlichkeit für Zufallszüge (0-100%) |
| `training_max_moves` | 250 | Maximale Züge pro Spiel |
| `training_draw_threshold` | 50 | Züge ohne Fortschritt bis Remis |
| `training_eval_threshold` | 3 | Max Bewertung in Bauern nach Zufallszügen (0=aus) |
| `training_adjudicate` | 10 | Spiel beenden bei +/- N Bauern (0=aus) |
| `training_filter_tactics` | 1 | Taktische Positionen filtern (1=an, 0=aus) |
| `training_verbose` | 1 | Verbosity Level (0-2) |

## Ergebnisse

Nach `stop_training.yml` werden die Daten nach `./collected_data/` kopiert:

```
collected_data/
├── training1/
│   ├── training_combined.txt
│   └── training.log
├── training2/
│   └── ...
└── training_all.txt          # Kombiniert aus allen Hosts
```

### Duplikate entfernen

```bash
cd collected_data
sort -u training_all.txt > training_unique.txt
```
