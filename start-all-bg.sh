#!/bin/bash

# Start all IFEX services in background without cleanup trap
# This script starts services and exits, leaving them running

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="${SCRIPT_DIR}/build"
LOG_DIR="${SCRIPT_DIR}/logs"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}🚀 Starting IFEX Core v0 System (Background Mode)${NC}"
echo "================================="

# Check if build exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}❌ Build directory not found!${NC}"
    echo "Please run ./build.sh first"
    exit 1
fi

# Create logs directory
mkdir -p "$LOG_DIR"
echo -e "${GREEN}✓ Created logs directory${NC}"

# Initialize startup log
echo "IFEX Core v0 System Startup - $(date)" > "$LOG_DIR/startup.log"
echo "=================================" >> "$LOG_DIR/startup.log"

# Function to check if service is running
check_service() {
    local pid=$1
    local name=$2
    if ps -p $pid > /dev/null; then
        echo -e "${GREEN}✅ $name running (PID: $pid)${NC}"
        return 0
    else
        echo -e "${RED}❌ $name failed to start${NC}"
        return 1
    fi
}

# Start Discovery Service
echo ""
echo -e "${BLUE}1️⃣  Starting Discovery Service...${NC}"
GLOG_v=2 GLOG_logtostderr=1 \
"$BUILD_DIR/reference-services/discovery/ifex-discovery-service" \
    > "$LOG_DIR/discovery.log" 2>&1 &
DISCOVERY_PID=$!

# Wait for discovery to start
sleep 2
if ! check_service $DISCOVERY_PID "Discovery Service"; then
    echo "Check $LOG_DIR/discovery.log for errors"
    exit 1
fi

# Start Dispatcher Service
echo ""
echo -e "${BLUE}2️⃣  Starting Dispatcher Service...${NC}"
GLOG_v=2 GLOG_logtostderr=1 \
"$BUILD_DIR/reference-services/dispatcher/ifex-dispatcher-service" \
    --discovery=localhost:50051 \
    --ifex-schema="$SCRIPT_DIR/reference-services/ifex/ifex-dispatcher-service.yml" \
    > "$LOG_DIR/dispatcher.log" 2>&1 &
DISPATCHER_PID=$!

# Wait for dispatcher to start
sleep 2
if ! check_service $DISPATCHER_PID "Dispatcher Service"; then
    echo "Check $LOG_DIR/dispatcher.log for errors"
    exit 1
fi

# Start test services
echo ""
echo -e "${BLUE}3️⃣  Starting Test Services...${NC}"

# Start Echo Service
if [ -f "$BUILD_DIR/test-services/ifex-echo-service" ]; then
    echo -e "  ${YELLOW}→${NC} Starting Echo Service..."
    (cd "$BUILD_DIR/test-services" && \
     GLOG_v=2 GLOG_logtostderr=1 \
        "./ifex-echo-service" \
        --listen=0.0.0.0:50053 \
        --discovery=localhost:50051 \
        --ifex-schema="./ifex/echo_service.ifex.yml" \
        > "$LOG_DIR/echo.log" 2>&1) &
    ECHO_PID=$!
    sleep 1
    if ps -p $ECHO_PID > /dev/null; then
        echo -e "    ${GREEN}✓${NC} Started on port 50053"
    else
        echo -e "    ${RED}✗${NC} Failed to start"
    fi
fi


# Start Settings Service
if [ -f "$BUILD_DIR/test-services/settings/ifex-settings-service" ]; then
    echo -e "  ${YELLOW}→${NC} Starting Settings Service..."
    (cd "$BUILD_DIR/test-services/settings" && \
     GLOG_v=2 GLOG_logtostderr=1 \
        "./ifex-settings-service" \
        --listen=0.0.0.0:50055 \
        --discovery=localhost:50051 \
        > "$LOG_DIR/settings.log" 2>&1) &
    SETTINGS_PID=$!
    sleep 1
    if ps -p $SETTINGS_PID > /dev/null; then
        echo -e "    ${GREEN}✓${NC} Started on port 50055"
    else
        echo -e "    ${RED}✗${NC} Failed to start"
    fi
fi

# Start Beverage Service
if [ -f "$BUILD_DIR/test-services/beverage/beverage-service" ]; then
    echo -e "  ${YELLOW}→${NC} Starting Beverage Service..."
    (cd "$BUILD_DIR/test-services/beverage" && \
     GLOG_v=2 GLOG_logtostderr=1 \
        "./beverage-service" \
        --address=0.0.0.0:50061 \
        --discovery_address=localhost:50051 \
        --ifex_schema="./beverage-service.ifex.yml" \
        > "$LOG_DIR/beverage.log" 2>&1) &
    BEVERAGE_PID=$!
    sleep 1
    if ps -p $BEVERAGE_PID > /dev/null; then
        echo -e "    ${GREEN}✓${NC} Started on port 50061"
    else
        echo -e "    ${RED}✗${NC} Failed to start"
    fi
fi

# Start Climate Comfort Service
if [ -f "$BUILD_DIR/test-services/climate-comfort/climate-comfort-service" ]; then
    echo -e "  ${YELLOW}→${NC} Starting Climate Comfort Service..."
    (cd "$BUILD_DIR/test-services/climate-comfort" && \
     GLOG_v=2 GLOG_logtostderr=1 \
        "./climate-comfort-service" \
        --address=0.0.0.0:50062 \
        --discovery_address=localhost:50051 \
        --ifex_schema="./climate-comfort-service.ifex.yml" \
        > "$LOG_DIR/climate-comfort.log" 2>&1) &
    CLIMATE_COMFORT_PID=$!
    sleep 1
    if ps -p $CLIMATE_COMFORT_PID > /dev/null; then
        echo -e "    ${GREEN}✓${NC} Started on port 50062"
    else
        echo -e "    ${RED}✗${NC} Failed to start"
    fi
fi

# Start Defrost Service
if [ -f "$BUILD_DIR/test-services/defrost/defrost-service" ]; then
    echo -e "  ${YELLOW}→${NC} Starting Defrost Service..."
    (cd "$BUILD_DIR/test-services/defrost" && \
     GLOG_v=2 GLOG_logtostderr=1 \
        "./defrost-service" \
        --address=0.0.0.0:50063 \
        --discovery_address=localhost:50051 \
        --ifex_schema="./defrost-service.ifex.yml" \
        > "$LOG_DIR/defrost.log" 2>&1) &
    DEFROST_PID=$!
    sleep 1
    if ps -p $DEFROST_PID > /dev/null; then
        echo -e "    ${GREEN}✓${NC} Started on port 50063"
    else
        echo -e "    ${RED}✗${NC} Failed to start"
    fi
fi

# Start Lua Runtime Manager v2
if [ -f "$BUILD_DIR/lua-orchestration2/lua-runtime-manager2" ]; then
    echo -e "  ${YELLOW}→${NC} Starting Lua Runtime Manager v2..."
    GLOG_v=2 GLOG_logtostderr=1 \
        "$BUILD_DIR/lua-orchestration2/lua-runtime-manager2" \
        --service_address=0.0.0.0:50090 \
        --discovery_endpoint=localhost:50051 \
        --ifex_schema="$SCRIPT_DIR/lua-orchestration2/test/lua-runtime-manager2.ifex.yml" \
        > "$LOG_DIR/lua-runtime-manager2.log" 2>&1 &
    LUA_RUNTIME_MANAGER2_PID=$!
    sleep 2
    if ps -p $LUA_RUNTIME_MANAGER2_PID > /dev/null; then
        echo -e "    ${GREEN}✓${NC} Started on port 50090"
        
        # Deploy simple test service via runtime manager
        echo -e "  ${YELLOW}→${NC} Deploying Simple Test Service via Runtime Manager..."
        sleep 1
        
        # Read the service files
        SERVICE_CODE=$(cat "$SCRIPT_DIR/lua-orchestration2/test/simple_test_service.lua" | sed 's/"/\\"/g' | sed ':a;N;$!ba;s/\n/\\n/g')
        IFEX_SCHEMA=$(cat "$SCRIPT_DIR/lua-orchestration2/test/simple-test-service.ifex.yml" | sed 's/"/\\"/g' | sed ':a;N;$!ba;s/\n/\\n/g')
        
        # Deploy the service (wait a bit more for service to stabilize)
        sleep 2
        
        # Try to deploy using dispatcher client
        DEPLOY_PARAMS=$(cat <<EOF
{
    "request": {
        "service_code": "$SERVICE_CODE",
        "ifex_schema": "$IFEX_SCHEMA",
        "config": {
            "auto_start": true,
            "resource_limits": {
                "max_memory_mb": 100,
                "max_cpu_percent": 50,
                "max_execution_time_sec": 0
            }
        }
    }
}
EOF
)
        
        if "$BUILD_DIR/test-client/ifex-dispatcher-client" call lua_runtime_manager deploy_service \
            --params="$DEPLOY_PARAMS" > "$LOG_DIR/deploy_simple_test.log" 2>&1; then
            echo -e "    ${GREEN}✓${NC} Simple test service deployed"
            
            # Check if the service was actually deployed by listing services
            if "$BUILD_DIR/test-client/ifex-dispatcher-client" call lua_runtime_manager list_services \
                --params='{}' 2>&1 | grep -q "simple_test"; then
                echo -e "    ${GREEN}✓${NC} Verified: simple_test service is running"
            fi
        else
            echo -e "    ${RED}✗${NC} Failed to deploy simple test service"
            echo -e "    Check $LOG_DIR/deploy_simple_test.log for errors"
            echo -e "    You can deploy it later with: ./deploy_lua_service.sh"
        fi
        
        # Deploy timer counter service via runtime manager
        echo -e "  ${YELLOW}→${NC} Deploying Timer Counter Service via Runtime Manager..."
        sleep 1
        
        # Read the service files
        TIMER_SERVICE_CODE=$(cat "$SCRIPT_DIR/lua-orchestration2/test/timer_counter_service.lua" | sed 's/"/\\"/g' | sed ':a;N;$!ba;s/\n/\\n/g')
        TIMER_IFEX_SCHEMA=$(cat "$SCRIPT_DIR/lua-orchestration2/test/timer-counter-service.ifex.yml" | sed 's/"/\\"/g' | sed ':a;N;$!ba;s/\n/\\n/g')
        
        # Deploy the timer service
        TIMER_DEPLOY_PARAMS=$(cat <<EOF
{
    "request": {
        "service_code": "$TIMER_SERVICE_CODE",
        "ifex_schema": "$TIMER_IFEX_SCHEMA",
        "config": {
            "auto_start": true,
            "resource_limits": {
                "max_memory_mb": 50,
                "max_cpu_percent": 20,
                "max_execution_time_sec": 0
            }
        }
    }
}
EOF
)
        
        if "$BUILD_DIR/test-client/ifex-dispatcher-client" call lua_runtime_manager deploy_service \
            --params="$TIMER_DEPLOY_PARAMS" > "$LOG_DIR/deploy_timer_counter.log" 2>&1; then
            echo -e "    ${GREEN}✓${NC} Timer counter service deployed"
            
            # Verify deployment
            if "$BUILD_DIR/test-client/ifex-dispatcher-client" call lua_runtime_manager list_services \
                --params='{}' 2>&1 | grep -q "timer_counter"; then
                echo -e "    ${GREEN}✓${NC} Verified: timer_counter_service is running"
            fi
        else
            echo -e "    ${RED}✗${NC} Failed to deploy timer counter service"
            echo -e "    Check $LOG_DIR/deploy_timer_counter.log for errors"
        fi
        
        # Deploy failing test service with auto-restart enabled
        echo -e "  ${YELLOW}→${NC} Deploying Failing Test Service (with auto-restart)..."
        sleep 1
        
        # Read the service files
        FAILING_SERVICE_CODE=$(cat "$SCRIPT_DIR/lua-orchestration2/test/failing_test_service.lua" | sed 's/"/\\"/g' | sed ':a;N;$!ba;s/\n/\\n/g')
        FAILING_IFEX_SCHEMA=$(cat "$SCRIPT_DIR/lua-orchestration2/test/failing_test_service.ifex.yml" | sed 's/"/\\"/g' | sed ':a;N;$!ba;s/\n/\\n/g')
        
        # Deploy the failing service with auto-restart enabled
        FAILING_DEPLOY_PARAMS=$(cat <<EOF
{
    "request": {
        "service_code": "$FAILING_SERVICE_CODE",
        "ifex_schema": "$FAILING_IFEX_SCHEMA",
        "config": {
            "auto_start": true,
            "auto_restart": true,
            "restart_delay_ms": 3000,
            "max_restart_attempts": 3,
            "resource_limits": {
                "max_memory_mb": 50,
                "max_cpu_percent": 10,
                "max_execution_time_sec": 0
            }
        }
    }
}
EOF
)
        
        if "$BUILD_DIR/test-client/ifex-dispatcher-client" call lua_runtime_manager deploy_service \
            --params="$FAILING_DEPLOY_PARAMS" > "$LOG_DIR/deploy_failing_test.log" 2>&1; then
            echo -e "    ${GREEN}✓${NC} Failing test service deployed (will auto-restart on failure)"
            echo -e "    ${BLUE}ℹ️${NC}  Service will fail after 5 seconds and restart up to 3 times"
            
            # Verify deployment
            if "$BUILD_DIR/test-client/ifex-dispatcher-client" call lua_runtime_manager list_services \
                --params='{}' 2>&1 | grep -q "failing_test"; then
                echo -e "    ${GREEN}✓${NC} Verified: failing_test_service is running"
            fi
        else
            echo -e "    ${RED}✗${NC} Failed to deploy failing test service"
            echo -e "    Check $LOG_DIR/deploy_failing_test.log for errors"
        fi
    else
        echo -e "    ${RED}✗${NC} Failed to start"
        echo -e "    Check $LOG_DIR/lua-runtime-manager2.log for errors"
    fi
fi

# Note: Lua Orchestration Service (Morning Commute) is now deployed via Lua Runtime Manager
# To deploy it, run: ./deploy_morning_commute.sh

# Start MCP Bridge (after all other services)
if [ -f "$BUILD_DIR/reference-services/mcp-bridge/mcp_bridge" ]; then
    echo -e "  ${YELLOW}→${NC} Starting MCP Bridge..."
    GLOG_v=2 GLOG_logtostderr=1 \
        "$BUILD_DIR/reference-services/mcp-bridge/mcp_bridge" \
        --host=localhost \
        --port=8888 \
        --discovery=localhost:50051 \
        > "$LOG_DIR/mcp_bridge.log" 2>&1 &
    MCP_PID=$!
    sleep 1
    if ps -p $MCP_PID > /dev/null; then
        echo -e "    ${GREEN}✓${NC} Started on port 8888"
    else
        echo -e "    ${RED}✗${NC} Failed to start"
    fi
fi

# Display system status
echo ""
echo -e "${BLUE}📋 System Status:${NC}"
echo "================================="
echo -e "  ${GREEN}✅${NC} Discovery Service: localhost:50051"
echo -e "  ${GREEN}✅${NC} Dispatcher Service: localhost:50052"
echo -e "  ${GREEN}✅${NC} MCP Bridge: localhost:8080"
echo ""
echo -e "${BLUE}📊 Service Endpoints:${NC}"
echo "  • Discovery: grpc://localhost:50051"
echo "  • Dispatcher: grpc://localhost:50052"
echo "  • Echo: grpc://localhost:50053"
echo "  • Climate Control: grpc://localhost:50054"
echo "  • Settings: grpc://localhost:50055"
echo "  • Beverage: grpc://localhost:50061"
echo "  • Climate Comfort: grpc://localhost:50062"
echo "  • Defrost: grpc://localhost:50063"
echo "  • Lua Runtime Manager v2: grpc://localhost:50090"
echo "  • Deployed Lua Services: via Runtime Manager"
echo "  • MCP Bridge: http://localhost:8888"
echo ""

# Run sanity check
echo -e "${BLUE}🧪 Running Sanity Check...${NC}"
echo "================================="
if [ -f "$BUILD_DIR/test-client/ifex-dispatcher-client" ]; then
    "$BUILD_DIR/test-client/ifex-dispatcher-client" sanity \
        --discovery=localhost:50051 \
        >> "$LOG_DIR/startup.log" 2>&1
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ Sanity check passed${NC}"
    else
        echo -e "${RED}❌ Sanity check failed${NC}"
        echo "Check $LOG_DIR/startup.log for details"
    fi
else
    echo -e "${YELLOW}⚠️  Dispatcher client not found, skipping sanity check${NC}"
fi

echo ""
echo -e "${GREEN}✅ All services started in background${NC}"
echo ""
echo "📁 Log files:"
echo "  • $LOG_DIR/discovery.log"
echo "  • $LOG_DIR/dispatcher.log"
echo "  • $LOG_DIR/echo.log"
echo "  • $LOG_DIR/climate-control.log"
echo "  • $LOG_DIR/settings.log"
echo "  • $LOG_DIR/beverage.log"
echo "  • $LOG_DIR/climate-comfort.log"
echo "  • $LOG_DIR/defrost.log"
echo "  • $LOG_DIR/lua-runtime-manager2.log"
echo "  • $LOG_DIR/deploy_simple_test.log"
echo "  • $LOG_DIR/deploy_timer_counter.log"
echo "  • $LOG_DIR/deploy_failing_test.log"
echo "  • $LOG_DIR/mcp_bridge.log"
echo ""
echo "🛠️  Useful commands:"
echo "  • List services: ./build/test-client/ifex-dispatcher-client list"
echo "  • Stop all: ./stop_services.sh"
echo "  • Check logs: tail -f logs/*.log"
echo ""
echo "🤖 MCP Integration:"
echo "  • Add to Claude Code: claude mcp add --transport sse ifex-vehicle http://localhost:8888"
echo "  • Check MCP status: /mcp (in Claude Code chat)"
echo "  • Note: Restart Claude Code after adding the MCP server"
echo ""
echo "💡 Example test commands:"
echo "  • Test echo: ./build/test-client/ifex-dispatcher-client call echo_service echo --params='{\"message\":\"Hello\"}'"
echo "  • Get presets: ./build/test-client/ifex-dispatcher-client call settings_service get_presets --params='{\"service_method\":{\"service_name\":\"climate_control_service\",\"namespace_name\":\"climate\",\"method_name\":\"set_temperature\"}}'"
echo "  • List Lua services: grpcurl -plaintext -d '{}' localhost:50090 ifex.service.runtime.LuaRuntimeManager.v2.x/list_services"
echo "  • Deploy new service: ./deploy_lua_service.sh --lua-script <lua-file> --ifex-schema <ifex-yaml>"
echo "  • Test deployed service: grpcurl -plaintext -d '{\"name\":\"Test\"}' localhost:50090 ifex.service.examples.hello.Hello.v1.x/hello"
echo "  • Test timer counter: grpcurl -plaintext -d '{}' localhost:50090 ifex.service.test.TimerCounterService.v1.x/get_count"
echo "  • Test failing service: grpcurl -plaintext -d '{}' localhost:50090 ifex.service.test.FailingTestService.v1.x/get_info"
echo "  • Check service status: ./build/test-client/ifex-dispatcher-client call lua_runtime_manager get_service_status --params='{\"request\":{\"service_id\":\"<service_id>\"}}'"
