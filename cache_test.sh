#!/bin/bash
#
# loadtest.sh - Proxy Lab 성능 테스트용 스크립트 (driver.sh 기반 개선)
#

HOME_DIR=$(pwd)
TIMEOUT=5
MAX_RAND=63000
PORT_START=1024
PORT_MAX=65000
MAX_PORT_TRIES=10
FETCH_FILE="home.html"

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

# Kill any stray processes
killall -q proxy tiny 2>/dev/null
sleep 1

# Check required files
[ ! -d ./tiny ] && echo "Error: ./tiny directory not found." && exit 1
[ ! -x ./tiny/tiny ] && echo "Building tiny server..." && (cd tiny && make)
[ ! -x ./proxy ] && echo "Error: ./proxy not found. Please build it." && exit 1
[ ! -f ./tiny/${FETCH_FILE} ] && echo "Error: ./tiny/${FETCH_FILE} not found." && exit 1

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

# Warm-up cache (optional)
echo "Warming up cache with curl..."
curl -s -o /dev/null --proxy http://localhost:$proxy_port http://localhost:$tiny_port/$FETCH_FILE
if [ $? -ne 0 ]; then
    echo "⚠️  Cache warm-up failed (curl returned non-zero). Check proxy or tiny status."
fi

# Run wrk
echo "Running wrk load test (4 threads, 50 connections, 10s)..."
wrk -t4 -c10 -d10s --latency \
  -H "Host: localhost:$tiny_port" \
  http://localhost:$proxy_port/http://localhost:$tiny_port/home.html


if [ $? -ne 0 ]; then
    echo "❌ wrk execution failed. Check proxy for read/write error handling or URI parsing logic."
fi

# Cleanup
echo "Cleaning up: Killing tiny and proxy..."
kill $tiny_pid 2>/dev/null && wait $tiny_pid 2>/dev/null
kill $proxy_pid 2>/dev/null && wait $proxy_pid 2>/dev/null

# Backup kill just in case
pkill -f "./tiny" 2>/dev/null
pkill -f "./proxy" 2>/dev/null

echo "✅ Load test completed and processes cleaned up."
