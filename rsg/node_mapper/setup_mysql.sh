#!/usr/bin/env bash
# setup_mysql.sh
#
# Restarts mysqld with stderr redirected to a log file so that the
# SQLCOM_EXEC output from the patched sql_parse.cc can be captured.
#
# Usage:
#   ./setup_mysql.sh [start|stop|status]
#
# Environment variables (all optional):
#   MYSQLD_BIN        Path to the mysqld binary
#                     (default: searches PATH, then common install locations)
#   MYSQLD_DATADIR    MySQL data directory
#                     (default: tries 'mysqld --verbose --help | grep datadir')
#   MYSQLD_EXTRA_ARGS Extra arguments forwarded to mysqld
#   SQLCOM_LOG        Destination for mysqld stderr (SQLCOM_EXEC output)
#                     (default: /tmp/mysql_stderr.log)
#   MYSQL_PORT        Port to use (default: 3306)
#   PID_FILE          Path to store mysqld PID (default: /tmp/node_mapper_mysqld.pid)
#
# After "start", the script prints the log file path.  Pass this to
# node_mapper with --log-file.
#
# Example workflow:
#   ./setup_mysql.sh start
#   make run LOG_FILE=/tmp/mysql_stderr.log
#   ./setup_mysql.sh stop   # brings back the original MySQL service

set -euo pipefail

# ---- defaults ---------------------------------------------------------------
SQLCOM_LOG="${SQLCOM_LOG:-/tmp/mysql_stderr.log}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
PID_FILE="${PID_FILE:-/tmp/node_mapper_mysqld.pid}"
MYSQL_SOCKET="${MYSQL_SOCKET:-/tmp/mysql_node_mapper.sock}"

# ---- locate mysqld ----------------------------------------------------------
find_mysqld() {
    if command -v mysqld &>/dev/null; then
        command -v mysqld
        return
    fi
    for candidate in \
        /usr/sbin/mysqld \
        /usr/local/sbin/mysqld \
        /opt/mysql/bin/mysqld \
        /usr/local/mysql/bin/mysqld; do
        if [[ -x "$candidate" ]]; then
            echo "$candidate"
            return
        fi
    done
    # Search inside the local build tree.
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    local bld_mysqld
    bld_mysqld="$(find "${script_dir}/../../mysql-server-mysql-8.0.33/bld" \
                       -name "mysqld" -type f 2>/dev/null | head -1)"
    if [[ -n "$bld_mysqld" ]]; then
        echo "$bld_mysqld"
        return
    fi
    echo ""
}

MYSQLD_BIN="${MYSQLD_BIN:-$(find_mysqld)}"
if [[ -z "$MYSQLD_BIN" ]]; then
    echo "ERROR: cannot locate mysqld binary.  Set MYSQLD_BIN=/path/to/mysqld" >&2
    exit 1
fi

# ---- locate data directory --------------------------------------------------
find_datadir() {
    # Try the running MySQL instance first.
    if command -v mysql &>/dev/null; then
        local d
        d=$(mysql -u root -e "SHOW VARIABLES LIKE 'datadir';" 2>/dev/null \
             | awk '/datadir/ {print $2}')
        if [[ -n "$d" && -d "$d" ]]; then
            echo "$d"
            return
        fi
    fi
    # Common defaults.
    for candidate in /var/lib/mysql /data/mysql /usr/local/mysql/data; do
        if [[ -d "$candidate" ]]; then
            echo "$candidate"
            return
        fi
    done
    echo ""
}

MYSQLD_DATADIR="${MYSQLD_DATADIR:-$(find_datadir)}"
if [[ -z "$MYSQLD_DATADIR" ]]; then
    MYSQLD_DATADIR="/tmp/mysql_node_mapper_data"
    if [[ ! -d "$MYSQLD_DATADIR" ]]; then
        mkdir -p "$MYSQLD_DATADIR"
    fi
fi

MYSQLD_EXTRA_ARGS="${MYSQLD_EXTRA_ARGS:-}"

# ============================================================
# Helper: stop any mysqld owned by this script (or any root mysqld)
# ============================================================
stop_mysqld() {
    local pid_file="$PID_FILE"
    if [[ -f "$pid_file" ]]; then
        local pid
        pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            echo "[setup_mysql] stopping mysqld (PID $pid) …"
            kill "$pid"
            # Wait up to 30 seconds for it to exit.
            for _ in $(seq 1 30); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 1
            done
            if kill -0 "$pid" 2>/dev/null; then
                echo "[setup_mysql] SIGKILL …"
                kill -9 "$pid" || true
            fi
        fi
        rm -f "$pid_file"
        echo "[setup_mysql] mysqld stopped."
    else
        echo "[setup_mysql] no PID file found at $pid_file"
        # Try to stop any system MySQL service as a fallback.
        if systemctl is-active --quiet mysql 2>/dev/null; then
            echo "[setup_mysql] stopping system MySQL service …"
            systemctl stop mysql || true
        elif systemctl is-active --quiet mysqld 2>/dev/null; then
            echo "[setup_mysql] stopping system mysqld service …"
            systemctl stop mysqld || true
        fi
    fi
}

# ============================================================
# Start mysqld with stderr → SQLCOM_LOG
# ============================================================
start_mysqld() {
    echo "[setup_mysql] starting mysqld with SQLCOM log: $SQLCOM_LOG"

    # Stop any existing instance first.
    stop_mysqld 2>/dev/null || true

    # Truncate the log file so old output doesn't confuse the executor.
    > "$SQLCOM_LOG"
    chmod 666 "$SQLCOM_LOG" 2>/dev/null || true

    # Build the command.
    # We use --skip-grant-tables / --skip-networking is NOT used because we
    # need the TCP port open for the C++ executor.
    local cmd=(
        "$MYSQLD_BIN"
        "--datadir=$MYSQLD_DATADIR"
        "--port=$MYSQL_PORT"
        "--socket=$MYSQL_SOCKET"
        "--pid-file=$PID_FILE"
        "--daemonize"
    )
    if [[ -n "$MYSQLD_EXTRA_ARGS" ]]; then
        # shellcheck disable=SC2206
        cmd+=($MYSQLD_EXTRA_ARGS)
    fi

    echo "[setup_mysql] command: ${cmd[*]}"
    echo "[setup_mysql] stderr  → $SQLCOM_LOG"

    # Launch in background with stderr redirected.
    "${cmd[@]}" >>"$SQLCOM_LOG" 2>>"$SQLCOM_LOG" &
    local pid=$!
    echo "$pid" > "$PID_FILE"
    echo "[setup_mysql] mysqld started (PID $pid)"

    # Wait for the TCP port to be ready (up to 30 seconds).
    echo -n "[setup_mysql] waiting for MySQL on port $MYSQL_PORT"
    for _ in $(seq 1 60); do
        if (echo > /dev/tcp/127.0.0.1/"$MYSQL_PORT") 2>/dev/null; then
            echo ""
            echo "[setup_mysql] MySQL is ready."
            break
        fi
        echo -n "."
        sleep 0.5
    done

    echo ""
    echo "[setup_mysql] SQLCOM log: $SQLCOM_LOG"
    echo "[setup_mysql] Run:  make -C rsg/node_mapper run LOG_FILE=$SQLCOM_LOG"
}

# ============================================================
# Status
# ============================================================
show_status() {
    if [[ -f "$PID_FILE" ]]; then
        local pid
        pid=$(cat "$PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            echo "[setup_mysql] mysqld is running (PID $pid, log: $SQLCOM_LOG)"
        else
            echo "[setup_mysql] PID file exists but process $pid is dead."
        fi
    else
        echo "[setup_mysql] no PID file — mysqld not managed by this script."
    fi
}

# ============================================================
# Entry point
# ============================================================
ACTION="${1:-start}"
case "$ACTION" in
    start)   start_mysqld ;;
    stop)    stop_mysqld  ;;
    restart) stop_mysqld; start_mysqld ;;
    status)  show_status ;;
    *)
        echo "Usage: $0 [start|stop|restart|status]" >&2
        exit 1
        ;;
esac
