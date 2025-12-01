#!/bin/bash

# Status check script for IFEX Core v0 system

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}IFEX Core v0 System Status${NC}"
echo "=========================="
echo ""

# Function to check if a process is running
check_process() {
    local process_name=$1
    local port=$2
    local pids=$(pgrep -f "$process_name")
    
    if [ ! -z "$pids" ]; then
        echo -e "${GREEN}✅ $process_name${NC}"
        echo "   PID(s): $pids"
        if [ ! -z "$port" ]; then
            # Check if port is listening
            if netstat -tuln 2>/dev/null | grep -q ":$port "; then
                echo "   Port: $port (listening)"
            else
                echo "   Port: $port (not listening)"
            fi
        fi
        
        # Show process details
        for pid in $pids; do
            if [ -f /proc/$pid/cmdline ]; then
                echo "   CMD: $(tr '\0' ' ' < /proc/$pid/cmdline | cut -c1-80)"
            fi
        done
    else
        echo -e "${RED}❌ $process_name${NC}"
        echo "   Status: Not running"
    fi
    echo ""
}

# Check core services
echo -e "${YELLOW}Core Services:${NC}"
check_process "ifex-discovery-service" "50051"
check_process "ifex-dispatcher-service" "50052"

# Check test services
echo -e "${YELLOW}Test Services:${NC}"
check_process "ifex-echo-service" "50053"
check_process "ifex-settings-service" "50055"

# Check for log files
echo -e "${YELLOW}Log Files:${NC}"
for log_dir in logs test-logs; do
    if [ -d "$log_dir" ]; then
        echo "📁 $log_dir/"
        for log in "$log_dir"/*.log; do
            if [ -f "$log" ]; then
                size=$(du -h "$log" | cut -f1)
                modified=$(date -r "$log" "+%Y-%m-%d %H:%M:%S")
                basename=$(basename "$log")
                echo "   • $basename ($size, modified: $modified)"
                
                # Show last error if any
                last_error=$(grep -i "error\|fail" "$log" 2>/dev/null | tail -1)
                if [ ! -z "$last_error" ]; then
                    echo "     Last error: $(echo $last_error | cut -c1-60)..."
                fi
            fi
        done
    fi
done

# Network connections
echo ""
echo -e "${YELLOW}Network Connections:${NC}"
echo "gRPC Ports:"
netstat -tuln 2>/dev/null | grep -E ":(5005[0-9]|5001|5002)" | while read line; do
    port=$(echo $line | awk '{print $4}' | rev | cut -d: -f1 | rev)
    echo "   • Port $port: listening"
done

# System recommendations
echo ""
echo -e "${YELLOW}Quick Commands:${NC}"
echo "• Start system:    ./start_system.sh"
echo "• Run tests:       ./test_system.sh"
echo "• View logs:       tail -f logs/*.log"
echo "• Stop all:        pkill -f ifex-"

# Check if grpcurl is available for testing
echo ""
if command -v grpcurl &> /dev/null; then
    echo -e "${GREEN}✓ grpcurl available${NC} - Can test gRPC endpoints"
else
    echo -e "${YELLOW}⚠️  grpcurl not found${NC} - Install for gRPC testing:"
    echo "   go install github.com/fullstorydev/grpcurl/cmd/grpcurl@latest"
fi