#!/bin/bash
# Run the IFEX V2 Scheduler Dashboard
#
# Usage: ./run_dashboard.sh [port]
#
# Environment variables:
#   SCHEDULER_HOST - Cloud scheduler host (default: localhost)
#   SCHEDULER_PORT - Cloud scheduler port (default: 50102)
#   DASHBOARD_PORT - Dashboard HTTP port (default: 8080)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Default port
PORT="${1:-${DASHBOARD_PORT:-8080}}"

# Check dependencies
if ! python3 -c "import flask" 2>/dev/null; then
    echo "Installing Python dependencies..."
    pip3 install -r requirements.txt
fi

# Export environment
export SCHEDULER_HOST="${SCHEDULER_HOST:-localhost}"
export SCHEDULER_PORT="${SCHEDULER_PORT:-50102}"
export DASHBOARD_PORT="$PORT"

echo ""
echo "Starting IFEX V2 Scheduler Dashboard"
echo "  Dashboard:  http://localhost:$PORT"
echo "  Scheduler:  $SCHEDULER_HOST:$SCHEDULER_PORT"
echo ""

python3 scheduler_api.py
