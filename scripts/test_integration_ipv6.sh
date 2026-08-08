#!/bin/sh
# Verify simultaneous IPv4 and IPv6 edge registration through one supernode.

set -eu

[ -z "${TOPDIR:-}" ] && TOPDIR=.
[ -z "${BINDIR:-}" ] && BINDIR=.

TEST_PORT=27654
TEST_DIR=$(mktemp -d)
SN_PID=
EDGE4_PID=
EDGE6_PID=

cleanup() {
    if [ -n "$EDGE4_PID" ]; then
        kill "$EDGE4_PID" 2>/dev/null || true
        wait "$EDGE4_PID" 2>/dev/null || true
    fi
    if [ -n "$EDGE6_PID" ]; then
        kill "$EDGE6_PID" 2>/dev/null || true
        wait "$EDGE6_PID" 2>/dev/null || true
    fi
    if [ -n "$SN_PID" ]; then
        kill "$SN_PID" 2>/dev/null || true
        wait "$SN_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT HUP INT TERM

"${BINDIR}/supernode" -f -p "[::]:${TEST_PORT}" -t 0 -vv \
    >"${TEST_DIR}/supernode.log" 2>&1 &
SN_PID=$!
sleep 1

# Registration and automatic overlay IPv4 assignment happen before TAP setup.
# A restricted test environment may therefore let edge exit at TUNSETIFF after
# the behavior under test has already completed successfully.
timeout 8 "${BINDIR}/edge" -f -S1 -t 0 -l "127.0.0.1:${TEST_PORT}" -c test -vv \
    >"${TEST_DIR}/edge4.log" 2>&1 &
EDGE4_PID=$!
timeout 8 "${BINDIR}/edge" -f -S1 -t 0 -l "[::1]:${TEST_PORT}" -c test -vv \
    >"${TEST_DIR}/edge6.log" 2>&1 &
EDGE6_PID=$!

wait "$EDGE4_PID" 2>/dev/null || true
EDGE4_PID=
wait "$EDGE6_PID" 2>/dev/null || true
EDGE6_PID=

if grep -q "received REGISTER_SUPER_ACK from supernode for IP address asignment" \
       "${TEST_DIR}/edge4.log" \
   && grep -q "received REGISTER_SUPER_ACK from supernode for IP address asignment" \
       "${TEST_DIR}/edge6.log" \
   && grep -q "processing incoming UDP packet.*sender: 127.0.0.1" \
       "${TEST_DIR}/supernode.log" \
   && grep -q "processing incoming UDP packet.*sender: \[::1\]" \
       "${TEST_DIR}/supernode.log"; then
    echo "IPv4/IPv6 edge registration with overlay IPv4 assignment: PASS"
else
    echo "IPv4/IPv6 edge registration with overlay IPv4 assignment: FAIL" >&2
    sed -n '1,160p' "${TEST_DIR}/supernode.log" >&2
    sed -n '1,160p' "${TEST_DIR}/edge4.log" >&2
    sed -n '1,160p' "${TEST_DIR}/edge6.log" >&2
    exit 1
fi
