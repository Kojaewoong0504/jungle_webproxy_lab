#!/bin/bash

# =============== 설정 ====================
DURATION="10s"
THREADS=4
CONNECTIONS=50
HOME_DIR=$(pwd)
TIMEOUT=5
MAX_RAND=63000
PORT_START=1024
PORT_MAX=65000
MAX_PORT_TRIES=10
TARGET_FILE="home.html"
TINY_DIR="./tiny"
PROXY_EXEC="./proxy"
TINY_EXEC="./tiny/tiny"
WRK_EXEC="wrk"
# =========================================

# ============== 함수 ======================

function free_port {
    port=$((( RANDOM % MAX_RAND ) + PORT_START ))
    while true
    do
        netstat -ant | grep -q ":$port "
        if [ $? -eq 0 ]; then
            (( port++ ))
            if [ $port -gt $PORT_MAX ]; then
                echo "-1"
                return
            fi
        else
            echo "$port"
            return
        fi
    done
}

function wait_for_port_use {
    local port=$1
    for i in {1..10}; do
        netstat -ant | grep -q ":$port "
        if [ $? -eq 0 ]; then return 0; fi
        sleep 0.5
    done
    echo "Timeout: Port $port not responding"
    exit 1
}

function start_tiny {
    tiny_port=$(free_port)
    echo "🛰  Starting Tiny on port $tiny_port"
    cd $TINY_DIR
    ./tiny $tiny_port &> /dev/null &
    tiny_pid=$!
    cd $HOME_DIR
    wait_for_port_use $tiny_port
}

function start_proxy {
    proxy_port=$(free_port)
    echo "🛰  Starting Proxy on port $proxy_port"
    $PROXY_EXEC $proxy_port > proxy_out.log 2> proxy_err.log &
    proxy_pid=$!
    sleep 1
    if ! ps -p $proxy_pid > /dev/null; then
        echo "❌ Proxy crashed. See proxy_err.log"
        cat proxy_err.log
        exit 1
    fi
    wait_for_port_use $proxy_port
}

function run_wrk_test {
    local desc=$1
    echo ""
    echo "🚀 Running wrk test: $desc"
    $WRK_EXEC -t$THREADS -c$CONNECTIONS -d$DURATION -H "Host: localhost:$tiny_port" --timeout $TIMEOUT "http://localhost:$proxy_port/$TARGET_FILE"
}

# ============== 실행 ======================

# 프록시/타이니 실행
start_tiny
start_proxy

# wrk cold cache test
run_wrk_test "Cold Cache (1st request)"

# wrk warm cache test
run_wrk_test "Warm Cache (2nd request - should be from cache)"

# TIME_WAIT 체크 (optional)
tw_start=$(netstat -ant | grep TIME_WAIT | wc -l)
sleep 1
tw_end=$(netstat -ant | grep TIME_WAIT | wc -l)
tw_diff=$((tw_end - tw_start))
echo -e "\n🧾 TIME_WAIT sockets increased: $tw_diff"

# 종료
echo "🧹 Killing processes"
kill $proxy_pid 2>/dev/null
kill $tiny_pid 2>/dev/null
wait $proxy_pid 2>/dev/null
wait $tiny_pid 2>/dev/null
