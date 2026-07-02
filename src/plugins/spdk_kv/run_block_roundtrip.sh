#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
# SPDX-License-Identifier: Apache-2.0
#
# Brings up an SPDK nvmf target with an NVM (block) namespace backed by a
# malloc bdev over VFIOUSER, then runs the SPDK direct-engine BLOCK round-trip
# test against it. The test writes DRAM patterns to LBA ranges and reads them
# back byte-exact (single-range, 4 KiB..2 MiB), and checks that misaligned,
# over-single-range, and out-of-capacity ranges are cleanly rejected.
#
# Env overrides:
#   SPDK_ROOT  (required) path to a target-capable SPDK build providing a built
#              build/bin/nvmf_tgt with the malloc bdev module and the block nvmf
#              namespace RPCs (bdev_malloc_create / nvmf_subsystem_add_ns). NOTE:
#              the host-side SPDK build the plugin links against may lack a built
#              nvmf_tgt; point SPDK_ROOT at a target-capable build in that case.
#   TEST_BIN   path to the built spdk_kv_block_roundtrip_test binary
#   BLOCK_SIZE malloc bdev logical block size in bytes (default 512)
#   DEV_SIZE_MB malloc bdev size in MiB (default 64)
set -euo pipefail

if [[ -z "${SPDK_ROOT:-}" ]]; then
    echo "error: SPDK_ROOT must be set to a target-capable SPDK build" >&2
    exit 1
fi
TEST_BIN="${TEST_BIN:-$(dirname "$0")/../../../builddir/src/plugins/spdk_kv/spdk_kv_block_roundtrip_test}"
TEST_BIN="$(readlink -f "$TEST_BIN")"
BLOCK_SIZE="${BLOCK_SIZE:-512}"
DEV_SIZE_MB="${DEV_SIZE_MB:-64}"

nqn="nqn.2026-06.io.spdk:spdk-blk-cnode0"
bdev_name="SpdkBlkMalloc0"
sock_dir="$(mktemp -d /tmp/spdk_blk_rt.XXXXXX)"
muser_dir="$sock_dir/domain/muser0/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$SPDK_ROOT/scripts/rpc.py -s $rpc_sock"
mkdir -p "$muser_dir"

nvmfpid=""
cleanup() {
    [[ -n "$nvmfpid" ]] && kill "$nvmfpid" 2>/dev/null || true
    rm -rf "$sock_dir"
}
trap cleanup EXIT

echo "== starting nvmf_tgt =="
# -s 1024: headroom for mapping the client's up-to-2 MiB DMA staging region.
"$SPDK_ROOT/build/bin/nvmf_tgt" -r "$rpc_sock" -m 0x1 --no-huge -s 1024 &
nvmfpid=$!

# Wait for the RPC socket to be ready.
for _ in $(seq 1 50); do
    if $rpc_py rpc_get_methods >/dev/null 2>&1; then break; fi
    sleep 0.2
done

echo "== configuring block (NVM) namespace over VFIOUSER =="
$rpc_py nvmf_create_transport -t VFIOUSER
$rpc_py bdev_malloc_create -b "$bdev_name" "$DEV_SIZE_MB" "$BLOCK_SIZE"
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKBLK001 -a
$rpc_py nvmf_subsystem_add_ns "$nqn" "$bdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0

echo "== running block round-trip test =="
"$TEST_BIN" "trtype:VFIOUSER traddr:$muser_dir"
rc=$?
echo "== test exit code: $rc =="
exit $rc
