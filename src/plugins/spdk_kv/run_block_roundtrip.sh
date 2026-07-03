#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
# SPDX-License-Identifier: Apache-2.0
#
# Brings up an SPDK nvmf target with an NVM (block) namespace backed by a
# malloc bdev over VFIOUSER, then runs the SPDK direct-engine BLOCK round-trip
# test against it. The test writes DRAM patterns to LBA ranges and reads them
# back byte-exact across 4 KiB..~60 MiB (large ranges ride the region-bounded
# SGL, one data-block descriptor per 2 MiB region), and checks that misaligned,
# over-single-op-bound (68 MiB), and out-of-capacity ranges are cleanly rejected
# (not split).
#
# The nvmf_tgt bring-up/teardown (sock dir, cleanup trap, launch, RPC poll,
# VFIOUSER transport + listener) is shared via tgt_common.sh; this script keeps
# only its malloc-bdev/namespace RPCs and the test invocation.
#
# Env overrides:
#   SPDK_ROOT  (required) path to a target-capable SPDK build providing a built
#              build/bin/nvmf_tgt with the malloc bdev module and the block nvmf
#              namespace RPCs (bdev_malloc_create / nvmf_subsystem_add_ns). NOTE:
#              the host-side SPDK build the plugin links against may lack a built
#              nvmf_tgt; point SPDK_ROOT at a target-capable build in that case.
#   TEST_BIN   path to the built spdk_kv_block_roundtrip_test binary
#   BLOCK_SIZE malloc bdev logical block size in bytes (default 512)
#   DEV_SIZE_MB malloc bdev size in MiB (default 128, so a ~60 MiB write fits)
set -euo pipefail

if [[ -z "${SPDK_ROOT:-}" ]]; then
    echo "error: SPDK_ROOT must be set to a target-capable SPDK build" >&2
    exit 1
fi
TEST_BIN="${TEST_BIN:-$(dirname "$0")/../../../builddir/src/plugins/spdk_kv/spdk_kv_block_roundtrip_test}"
TEST_BIN="$(readlink -f "$TEST_BIN")"
BLOCK_SIZE="${BLOCK_SIZE:-512}"
DEV_SIZE_MB="${DEV_SIZE_MB:-128}"

source "$(dirname "$0")/tgt_common.sh"

nqn="nqn.2026-06.io.spdk:spdk-blk-cnode0"
bdev_name="SpdkBlkMalloc0"
tgt_setup spdk_blk_rt

# -s 1536: headroom for the malloc bdev backing store plus mapping the client's
# up-to-~64 MiB DMA staging region (the region-bounded SGL large-transfer path).
tgt_start 1536

echo "== configuring block (NVM) namespace over VFIOUSER =="
tgt_create_vfiouser_transport
$rpc_py bdev_malloc_create -b "$bdev_name" "$DEV_SIZE_MB" "$BLOCK_SIZE"
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKBLK001 -a
$rpc_py nvmf_subsystem_add_ns "$nqn" "$bdev_name"
tgt_add_vfiouser_listener "$nqn"

echo "== running block round-trip test =="
"$TEST_BIN" "trtype:VFIOUSER traddr:$muser_dir"
rc=$?
echo "== test exit code: $rc =="
exit $rc
