#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
# SPDX-License-Identifier: Apache-2.0
#
# Brings up an SPDK nvmf target with an in-memory KV namespace (kvdev_mem, no
# Ceph) over VFIOUSER, then runs the SPDK direct-engine round-trip test
# against it. Stores small AND large values (up to ~64 MiB, via the
# region-bounded SGL) under 16-byte keys and Retrieves them back byte-exact, and
# checks an over-bound value is cleanly rejected (not striped).
#
# The nvmf_tgt bring-up/teardown (sock dir, cleanup trap, launch, RPC poll,
# VFIOUSER transport + listener) is shared via tgt_common.sh; this script keeps
# only its KV-namespace RPCs and the test invocation.
#
# Env overrides:
#   SPDK_ROOT  (required) path to a target-capable NVMe-KV SPDK build. This
#              tree must provide a built build/bin/nvmf_tgt with the in-memory
#              kvdev module + KV nvmf namespace RPCs (kvdev_mem_create /
#              nvmf_subsystem_add_kv_ns). NOTE: the host-side SPDK build the
#              plugin links against may carry only the NVMe-KV *host* driver; if
#              it has no nvmf_tgt built, point SPDK_ROOT at a target-capable KV
#              SPDK build.
#   TEST_BIN   path to the built spdk_roundtrip_test binary
set -euo pipefail

if [[ -z "${SPDK_ROOT:-}" ]]; then
    echo "error: SPDK_ROOT must be set to a target-capable NVMe-KV SPDK build" >&2
    exit 1
fi
TEST_BIN="${TEST_BIN:-$(dirname "$0")/../../../builddir/src/plugins/spdk/spdk_roundtrip_test}"
TEST_BIN="$(readlink -f "$TEST_BIN")"

source "$(dirname "$0")/tgt_common.sh"

nqn="nqn.2026-06.io.spdk:spdk-kv-cnode0"
kvdev_name="SpdkKvMem0"
tgt_setup spdk_rt

# -s 1024: headroom for mapping the client's up-to-~64 MiB DMA regions plus the
# in-memory kvdev holding several large stored values simultaneously.
tgt_start 1024

echo "== configuring in-memory KV namespace =="
tgt_create_vfiouser_transport
# Raise the kvdev value cap to 128 MiB so large-value stores (up to the plugin's
# ~64 MiB single-op bound) are not rejected by the target's default 1 MiB cap.
# The host-side ~64 MiB region-bounded-SGL bound is still the effective limit.
$rpc_py kvdev_mem_create "$kvdev_name" --max-value-len $((128 * 1024 * 1024))
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKKV001 -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
tgt_add_vfiouser_listener "$nqn"

echo "== running round-trip test =="
"$TEST_BIN" "trtype:VFIOUSER traddr:$muser_dir"
rc=$?
echo "== test exit code: $rc =="
exit $rc
