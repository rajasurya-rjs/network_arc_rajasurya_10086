#!/usr/bin/env bash
# One-command evaluator demo (`make demo`):
#   1. starts calc_server on a free port,
#   2. runs demo_client in sequential mode: 7 requests over ONE TCP connection,
#   3. runs demo_client in pipelined mode: the same 7 requests in ONE send(),
#   4. prints the server log, which shows one accept() per client run and
#      the exact stream byte range of every request.
#
# Extra arguments are passed to demo_client, e.g. `./scripts/demo.sh --show-bytes`
# to print every request and response as escaped wire bytes.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
SERVER="$BUILD/calc_server"
CLIENT="$BUILD/demo_client"
if [[ ! -x "$SERVER" || ! -x "$CLIENT" ]]; then
  echo "binaries not found in $BUILD/ - run 'make' first" >&2
  exit 1
fi

LOG="$(mktemp "${TMPDIR:-/tmp}/calc_server_demo.XXXXXX")"
"$SERVER" --port "${PORT:-0}" 2>"$LOG" &
SERVER_PID=$!
cleanup() {
  kill -INT "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  rm -f "$LOG"
}
trap cleanup EXIT

port=""
for _ in $(seq 1 100); do
  port="$(sed -n 's/.*listening on [^ ]*:\([0-9][0-9]*\) .*/\1/p' "$LOG")"
  [[ -n "$port" ]] && break
  sleep 0.05
done
if [[ -z "$port" ]]; then
  echo "server failed to start:" >&2
  cat "$LOG" >&2
  exit 1
fi

banner() { printf '\n==== %s ====\n\n' "$1"; }

banner "1. Sequential: 7 requests, one at a time, over ONE TCP connection (server on port $port)"
"$CLIENT" --port "$port" "$@"

banner "2. Pipelined: the same 7 requests written in ONE send(), responses read back in order"
"$CLIENT" --port "$port" --pipeline "$@"

sleep 0.1
banner "3. Server log: one 'accepted connection' per client run; every request's stream byte range"
cat "$LOG"
