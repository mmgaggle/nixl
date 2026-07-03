#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
# SPDX-License-Identifier: Apache-2.0
#
# Negative test for the block open guard: brings up an SPDK nvmf target with a
# metadata-formatted (interleaved / extended-LBA) malloc bdev exposed as an NVM
# (block) namespace over VFIOUSER, then runs the SPDK direct-engine BLOCK
# round-trip test against it and asserts the engine/shim REFUSES it CLEANLY at
# init -- non-zero exit, a distinct -ENOTSUP (rc=-95) refusal in the log -- rather
# than faulting later at SGL build.
#
# Rationale: the block datapath sizes each transfer from the DATA-only sector
# size, but lib/nvme sizes the DMA payload of a metadata-formatted namespace from
# the EXTENDED sector size (data + metadata). The shim detects the metadata
# format at open (spdk_nvme_ns_get_md_size > 0 / supports_extended_lba) and fails
# with -ENOTSUP so init fails informatively instead of the payload/SGL mismatch
# faulting mid-transfer. Full metadata / DIF/DIX support is out of scope.
#
# The nvmf_tgt bring-up/teardown (sock dir, cleanup trap, launch, RPC poll,
# VFIOUSER transport + listener) is shared via tgt_common.sh; this script keeps
# only its metadata-malloc-bdev/namespace RPCs and the refusal assertions.
#
# Env overrides:
#   SPDK_ROOT  (required) path to a target-capable SPDK build providing a built
#              build/bin/nvmf_tgt whose bdev_malloc_create supports the metadata
#              options (--md-size / --md-interleave).
#   TEST_BIN   path to the built spdk_kv_block_roundtrip_test binary
#   BLOCK_SIZE malloc bdev data block size in bytes (default 512)
#   MD_SIZE    malloc bdev metadata size in bytes (default 8; must be > 0)
#   DEV_SIZE_MB malloc bdev size in MiB (default 64)
set -euo pipefail

if [[ -z "${SPDK_ROOT:-}" ]]; then
    echo "error: SPDK_ROOT must be set to a target-capable SPDK build" >&2
    exit 1
fi
TEST_BIN="${TEST_BIN:-$(dirname "$0")/../../../builddir/src/plugins/spdk_kv/spdk_kv_block_roundtrip_test}"
TEST_BIN="$(readlink -f "$TEST_BIN")"
BLOCK_SIZE="${BLOCK_SIZE:-512}"
MD_SIZE="${MD_SIZE:-8}"
DEV_SIZE_MB="${DEV_SIZE_MB:-64}"

source "$(dirname "$0")/tgt_common.sh"

nqn="nqn.2026-06.io.spdk:spdk-blkmd-cnode0"
bdev_name="SpdkBlkMdMalloc0"
tgt_setup spdk_blkmd_rt

tgt_start 1024

# Create a malloc bdev with INTERLEAVED per-LBA metadata: the extended sector
# size is BLOCK_SIZE + MD_SIZE, so extended_sector_size > sector_size and the
# host sees md_size > 0. If this SPDK build cannot create a metadata malloc bdev,
# the guard cannot be exercised here -- report and skip cleanly (rc 0) rather than
# fail, so the harness treats "no metadata-capable target" as not-applicable.
echo "== configuring metadata (interleaved / extended-LBA) block namespace =="
tgt_create_vfiouser_transport
if ! $rpc_py bdev_malloc_create -b "$bdev_name" -m "$MD_SIZE" -i "$DEV_SIZE_MB" "$BLOCK_SIZE"; then
    echo "SKIP: this SPDK build cannot create a metadata-formatted malloc bdev" >&2
    echo "SKIP: block metadata-rejection guard not exercised (no metadata-capable target)" >&2
    exit 0
fi
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKBLKMD01 -a
# Do NOT pass --hide-metadata: the host must SEE the metadata format so the guard
# (md_size > 0) fires.
$rpc_py nvmf_subsystem_add_ns "$nqn" "$bdev_name"
tgt_add_vfiouser_listener "$nqn"

echo "== running block round-trip test (expecting CLEAN refusal at init) =="
set +e
out="$("$TEST_BIN" "trtype:VFIOUSER traddr:$muser_dir" 2>&1)"
rc=$?
set -e
echo "$out"
echo "== test exit code: $rc =="

# The engine must have REFUSED the metadata namespace: non-zero exit AND the
# distinct -ENOTSUP (rc=-95) metadata refusal in the log (not a crash or an
# unrelated init failure, and NOT a successful round-trip on a faulting payload).
if [[ $rc -eq 0 ]]; then
    echo "FAIL: engine accepted a metadata-formatted block namespace (expected clean refusal)" >&2
    exit 1
fi
if ! grep -q "rc=-95" <<<"$out"; then
    echo "FAIL: init failed but not via the -ENOTSUP metadata guard (expected 'rc=-95')" >&2
    exit 1
fi
if ! grep -qi "per-LBA metadata" <<<"$out"; then
    echo "FAIL: missing the informative metadata-refusal message" >&2
    exit 1
fi
echo "== metadata-formatted block namespace correctly REFUSED (clean -ENOTSUP init failure) =="
echo "run_block_metadata_reject: PASS"
exit 0
