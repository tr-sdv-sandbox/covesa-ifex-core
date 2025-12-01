#!/bin/bash

# Run IFEX Explorer GUI
# This starts the REST proxy that bridges gRPC services to the web GUI

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Check if virtual environment exists
if [ ! -d "$SCRIPT_DIR/venv" ]; then
    echo "Creating Python virtual environment..."
    python3 -m venv "$SCRIPT_DIR/venv"
    source "$SCRIPT_DIR/venv/bin/activate"
    pip install -r "$SCRIPT_DIR/requirements.txt"
else
    source "$SCRIPT_DIR/venv/bin/activate"
fi

# Set environment variables
export SERVICE_DISCOVERY_HOST=localhost
export SERVICE_DISCOVERY_PORT=50051
export PROXY_PORT=5002

echo "🌐 Starting IFEX Explorer REST Proxy..."
echo "================================="
echo "Service Discovery: $SERVICE_DISCOVERY_HOST:$SERVICE_DISCOVERY_PORT"
echo "REST API Port: $PROXY_PORT"
echo ""
echo "Once started, open your browser to:"
echo "  http://localhost:$PROXY_PORT/ifex_explorer_gui.html"
echo ""

# Run the proxy
python "$SCRIPT_DIR/ifex_explorer_proxy.py"