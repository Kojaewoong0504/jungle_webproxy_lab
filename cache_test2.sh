#!/bin/bash
#
# loadtest.sh - Proxy Lab 성능 테스트 스크립트 (랜덤 파일 요청 포함)
#

HOME_DIR=$(pwd)
TIMEOUT=5
MAX_RAND=63000
PORT_START=1024
PORT_MAX=65000
MAX_PORT_TRIES=10
FILE_COUNT=10
FETCH_FILE="home1.html"  # 캐시 워밍업용
LUA_SCRIPT="random_files.lua"

function wait_for_port_use() {
    timeout_count=0
    while [ "$timeout_count" -lt "$MAX_PORT_TRIES" ]; do
        if netstat -an | grep LISTEN | grep -q ":$1"; then
            return
        fi
        timeout_count=$((timeout_count + 1))
        sleep 1
    done
    echo "Timeout waiting for port $1"
    exit 1
}

function free_port {
    port=$(((RANDOM % MAX_RAND) + PORT_START))
    while true; do
        if ! netstat -an | grep LISTEN | grep -q ":$port"; then
            echo $port
            return
        fi
        port=$((port + 1))
        if [ $port -gt $PORT_MAX ]; then
            echo "-1"
            return
        fi
    done
}

function generate_test_files() {
    echo "Generating $FILE_COUNT test files (~100KB each)..."
    mkdir -p tiny
    cd tiny
    for i in $(seq 1 $FILE_COUNT); do
        fname="home${i}.html"
        head -c 102000 < /dev/urandom | base64 | head -c 102000 > "$fname"
    done
    cd ..
    echo "✅ Files generated in ./tiny/"
}

function generate_lua_script() {
    echo "Generating Lua script for wrk: $LUA_SCRIPT"
    cat > $LUA_SCRIPT <<EOF
math.randomseed(os.time())

request = function()
  local i = math.random(1, $FILE_COUNT)
  local uri = "/http://localhost:$tiny_port/home" .. i .. ".html"
  return wrk.format("GET", uri, {["Host"] = "localhost:$tiny_port"})
end
EOF
    echo "✅ Lua script created."
}

# ===== Main Execution =====

# Kill any stray processes
killall -q proxy tiny 2>/dev/null
sleep 1

# Generate test files
generate_test_files

# Check required binaries
[ ! -x ./tiny/tiny ] && echo "Building tiny server..." && (cd tiny && make)
[ ! -x ./proxy ] && echo "Error: ./proxy not found. Please build it." && exit 1

# Start Tiny
tiny_port=$(free_port)
echo "Starting tiny on port $tiny_port"
(cd tiny && ./tiny $tiny_port > /dev/null 2>&1 &) 
tiny_pid=$!
wait_for_port_use $tiny_port

# Start Proxy
proxy_port=$(free_port)
echo "Starting proxy on port $proxy_port"
./proxy $proxy_port > /dev/null 2>&1 &
proxy_pid=$!
wait_for_port_use $proxy_port

# Generate Lua script for wrk
generate_lua_script

# Warm-up cache
echo "Warming up cache with curl (for $FETCH_FILE)..."
curl -s -o /dev/null --proxy http://localhost:$proxy_port http://localhost:$tiny_port/$FETCH_FILE
if [ $? -ne 0 ]; then
    echo "⚠️  Cache warm-up failed"
fi

# Run wrk test
echo "Running wrk with random file access (4 threads, 50 connections, 10s)..."
wrk -t4 -c50 -d10s --latency -s $LUA_SCRIPT http://localhost:$proxy_port

# Cleanup
echo "Cleaning up: Killing tiny and proxy..."
kill $tiny_pid 2>/dev/null && wait $tiny_pid 2>/dev/null
kill $proxy_pid 2>/dev/null && wait $proxy_pid 2>/dev/null
pkill -f "./tiny" 2>/dev/null
pkill -f "./proxy" 2>/dev/null

echo "✅ Test completed and processes cleaned up."
