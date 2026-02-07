#!/bin/bash
# Start a local web server to view the SPSA dashboard

echo "Starting SPSA Dashboard..."
echo "Open http://localhost:8080/spsa_dashboard.html in your browser"
echo "Press Ctrl+C to stop"
echo ""

cd "$(dirname "$0")"
python3 -m http.server 8080
