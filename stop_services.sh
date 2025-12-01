#!/bin/bash

# Stop script for IFEX Core v0 system

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${YELLOW}🛑 Stopping IFEX Core v0 Services...${NC}"
echo "================================="

# Kill services in reverse order
echo -e "${BLUE}Stopping test services...${NC}"
pkill -f "ifex-echo-service" 2>/dev/null && echo -e "  ${GREEN}✓${NC} Stopped Echo Service" || echo -e "  ${YELLOW}-${NC} Echo Service not running"
pkill -f "ifex-settings-service" 2>/dev/null && echo -e "  ${GREEN}✓${NC} Stopped Settings Service" || echo -e "  ${YELLOW}-${NC} Settings Service not running"
pkill -f "beverage-service" 2>/dev/null && echo -e "  ${GREEN}✓${NC} Stopped Beverage Service" || echo -e "  ${YELLOW}-${NC} Beverage Service not running"
pkill -f "climate-comfort-service" 2>/dev/null && echo -e "  ${GREEN}✓${NC} Stopped Climate Comfort Service" || echo -e "  ${YELLOW}-${NC} Climate Comfort Service not running"
pkill -f "defrost-service" 2>/dev/null && echo -e "  ${GREEN}✓${NC} Stopped Defrost Service" || echo -e "  ${YELLOW}-${NC} Defrost Service not running"
pkill -f "departure-orchestration-service" 2>/dev/null && echo -e "  ${GREEN}✓${NC} Stopped Departure Orchestration Service" || echo -e "  ${YELLOW}-${NC} Departure Orchestration Service not running"
pkill -f "lua-runtime-manager2" 2>/dev/null && echo -e "  ${GREEN}✓${NC} Stopped Lua Runtime Manager v2" || echo -e "  ${YELLOW}-${NC} Lua Runtime Manager v2 not running"

echo ""
echo -e "${BLUE}Stopping bridge services...${NC}"
pkill -f "mcp_bridge" 2>/dev/null && echo -e "  ${GREEN}✓${NC} Stopped MCP Bridge" || echo -e "  ${YELLOW}-${NC} MCP Bridge not running"

echo ""
echo -e "${BLUE}Stopping core services...${NC}"
pkill -f "ifex-dispatcher-service" 2>/dev/null && echo -e "  ${GREEN}✓${NC} Stopped Dispatcher Service" || echo -e "  ${YELLOW}-${NC} Dispatcher Service not running"
pkill -f "ifex-discovery-service" 2>/dev/null && echo -e "  ${GREEN}✓${NC} Stopped Discovery Service" || echo -e "  ${YELLOW}-${NC} Discovery Service not running"

echo ""
echo -e "${GREEN}✅ All services stopped${NC}"
