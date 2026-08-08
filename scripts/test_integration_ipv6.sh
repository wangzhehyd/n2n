#!/bin/sh
# Verify that an edge can register through IPv6 and receive an overlay IPv4.

set -eu

[ -z "${TOPDIR:-}" ] && TOPDIR=.
[ -z "${BINDIR:-}" ] && BINDIR=.

TEST_PORT=27654
TEST_DIR=$(mktemp -d)
SN_PID=

cleanup() {
    if [ -n "$SN_PID" ]; then
        kill "$SN_PID" 2>/dev/null || true
        wait "$SN_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT HUP INT TERM

"${BINDIR}/supernode" -f -p "[::1]:${TEST_PORT}" -t 0 -vv \
    >"${TEST_DIR}/supernode.log" 2>&1 &
SN_PID=$!
sleep 1

# Registration and automatic overlay IPv4 assignment happen before TAP setup.
# A restricted test environment may therefore let edge exit at TUNSETIFF after
# the behavior under test has already completed successfully.
timeout 8 "${BINDIR}/edge" -f -S1 -l "[::1]:${TEST_PORT}" -c test -vv \
    >"${TEST_DIR}/edge.log" 2>&1 || true

if grep -q "received REGISTER_SUPER_ACK from supernode for IP address asignment" \
       "${TEST_DIR}/edge.log" \
   && grep -q "processing incoming UDP packet.*sender: \[::1\]" \
       "${TEST_DIR}/supernode.log"; then
    echo "IPv6 edge registration with overlay IPv4 assignment: PASS"
else
    echo "IPv6 edge registration with overlay IPv4 assignment: FAIL" >&2
    sed -n '1,160p' "${TEST_DIR}/supernode.log" >&2
    sed -n '1,160p' "${TEST_DIR}/edge.log" >&2
    exit 1
fi
