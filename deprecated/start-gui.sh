#!/bin/bash

# Live GUI Startup Script

# Change to test_scripts directory
cd "test-gui"

# Check if virtual environment exists or needs recreation
if [ ! -d "venv" ] || [ ! -f "venv/bin/python" ]; then
    echo "🔧 Setting up virtual environment..."
    rm -rf venv 2>/dev/null
    
    # Use system python3 explicitly to avoid conda interference
    /usr/bin/python3 -m venv venv
    if [ $? -ne 0 ]; then
        echo "❌ Failed to create virtual environment"
        echo "Make sure python3-venv is installed: sudo apt install python3-venv python3-full"
        exit 1
    fi
    echo "✅ Virtual environment created"
fi

# Activate virtual environment
source "venv/bin/activate"

# Install requirements using python -m pip to ensure we use the right pip
echo "📦 Installing Python dependencies..."
python -m pip install --upgrade pip > /dev/null 2>&1
python -m pip install -q -r requirements.txt
if [ $? -ne 0 ]; then
    echo "❌ Failed to install requirements"
    echo "Trying with verbose output:"
    python -m pip install -r requirements.txt
    exit 1
fi

# Cleanup function
cleanup() {
    echo ""
    echo "🛑 Shutting down all services..."
    
    # Kill all started processes
    pkill -f "ifex_explorer_proxy.py" 2>/dev/null
    
    echo "✅ All services stopped."
    exit 0
}

# Set trap for cleanup
trap cleanup EXIT INT TERM

echo ""
# Start IFEX Explorer Proxy
echo "  🔍 Starting IFEX Explorer Proxy..."
bash -c "source venv/bin/activate && PROXY_PORT=5002 python3 ./ifex_explorer_proxy.py" > ../logs/explorer_proxy.log 2>&1 &
EXPLORER_PROXY_PID=$!
sleep 3

if ! ps -p $EXPLORER_PROXY_PID > /dev/null; then
    echo "❌ IFEX Explorer Proxy failed to start"
    echo "Check ../logs/explorer_proxy.log for errors"
    exit 1
fi

echo "✅ IFEX Explorer Proxy running (PID: $EXPLORER_PROXY_PID)"

# Open IFEX Explorer
echo "  🔍 Opening IFEX Explorer..."
xdg-open "http://localhost:5002/ifex_explorer_gui.html" 2>/dev/null || echo "   ℹ️  Manually open: http://localhost:5002/ifex_explorer_gui.html"

echo "🏃 All services running! Press Ctrl+C to stop..."
echo ""

# Keep script running
while true; do
    sleep 1
done
