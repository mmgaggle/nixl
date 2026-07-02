<!--
SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# NIXL Generic SPDK NVMe-KV Plugin (`SPDK`)

A clean-sheet, **transport-agnostic** and **backend-agnostic** NIXL backend that
speaks the **ratified NVMe Key-Value command set** over the SPDK NVMe driver
(`lib/nvme`). It works against **any** SPDK NVMe-KV target — an in-memory
`kvdev`, a librados-backed `kvdev`, a DPU-presented VF, an emulated NVMe in a
guest — and is intended to be upstreamable to `ai-dynamo/nixl` as *the* generic
SPDK KV backend.

It carries **no backend-specific behavior, no KV Exec, and no long-key path** by
design.

This directory implements the **walking skeleton**, **Exist (QUERY) and value
auto-sizing**, and **large values**.

## Scope

In:
- NIXL `WRITE` → KV **Store**, NIXL `READ` → KV **Retrieve**.
- NIXL `queryMem` / QUERY → KV **Exist** (cache hit/miss, **no data transfer**).
- **Value auto-sizing** on Retrieve: a short host buffer surfaces the device's
  **TRUE value length** (completion `cdw0`) so the caller can **resize and
  re-Retrieve** instead of silently truncating.
- **16-byte inline keys**, taken **verbatim** (opaque; no lineage parsing).
- **Small AND large values** (up to ~64 MiB) in **host DRAM**, large ones
  carried by a **region-bounded SGL** (see below). **No striping.**
- Connect via a **standard SPDK transport ID** — either
  `trtype:VFIOUSER traddr:<sock>` (an SPDK vfio-user target) or
  `trtype:PCIE traddr:<BDF>` (a real NVMe controller via `vfio-pci`). The
  transport is chosen **purely by the config string**, with **no code fork**.

Out:
- VRAM / P2PDMA.
- Delete/List, as needed.
- KV **Exec** and **long keys** — permanently out of scope for this generic
  plugin.

## Operation and memory-type mapping

| NIXL operation | Local mem  | Remote mem | NVMe KV command |
| -------------- | ---------- | ---------- | --------------- |
| `NIXL_WRITE`   | `DRAM_SEG` | `OBJ_SEG`  | KV **Store**    |
| `NIXL_READ`    | `DRAM_SEG` | `OBJ_SEG`  | KV **Retrieve** |
| `queryMem`     | —          | `OBJ_SEG`  | KV **Exist**    |

The remote `OBJ_SEG` descriptor carries the NIXL block identifier in its
`metaInfo` blob. The engine takes those bytes **verbatim** as the inline NVMe-KV
key (1–16 bytes). An empty or over-16-byte key is **rejected**
(`NIXL_ERR_INVALID_PARAM`), never truncated (truncation would alias distinct
keys sharing a prefix and corrupt data).

`queryMem` returns per the OBJ/file convention: `resp[i]` **engaged** (an empty
params map) on a **hit**, `std::nullopt` on a **miss**. A submit-/transport-level
failure returns an error status (never masked as a miss).

## Design decisions

- **Build on SPDK `lib/nvme`, not the raw vfio-user client.** The
  plugin submits KV commands through SPDK's public NVMe-KV API
  (`spdk_nvme_kv_store` / `spdk_nvme_kv_retrieve` in `include/spdk/nvme_kv.h`)
  over a controller attached with `spdk_nvme_probe`. SPDK's transport layer is
  what makes the datapath transport-agnostic for free (VFIOUSER now, PCIE
  later). This is why the plugin does **not** reuse the raw `nkv_vfu` client.
- **Own generic shim.** The C++ backend talks to a small, self-contained C shim
  (`spdk_kv_shim.{h,c}`) compiled into its own static lib. This isolates the
  C-only SPDK headers from the C++ TUs and keeps the plugin free of any external
  shim dependency. The shim is generic (no backend-specific behavior). It
  intentionally does **not** carry per-op identity-token / reconnect hardening;
  that robustness (for target-death mid-op) is deferred — this skeleton targets
  a healthy in-memory/vfio-user target.
- **Transport as config.** The `transport_id` param is a standard SPDK transport
  ID string parsed with `spdk_nvme_transport_id_parse`. A bare socket directory
  may be passed as `vfu_addr`/`socket` and is wrapped into a VFIOUSER trid.
- **Key is opaque.** Lineage-vs-flat is the caller's key-format choice; the
  generic plugin only checks the bytes fit the ratified 1–16-byte window.
- **DRAM staging.** SPDK Store/Retrieve need DMA-capable buffers; registered
  user DRAM generally is not, so the plugin stages through a per-request SPDK
  DMA buffer and copies. A future revision registers user/GPU buffers directly.
- **Value auto-sizing.** On Retrieve the completion `cdw0` carries the device's
  TRUE stored length. Devices signal a short host buffer two ways — SUCCESS with
  `cdw0 > buf_len` (this KV target), or `0x85 INVALID_VALUE_SIZE` directly — and
  the shim **normalizes both** to a single `BUFFER_TOO_SMALL` (`0x85`) return
  that reports the true length. The backend then does **not** copy a truncated
  value; it records the true length and reports `NIXL_ERR_MISMATCH`, so the
  caller resizes and re-Retrieves. The true length is read back with
  `getReqTrueLen(handle, idx)`. This implements the **optimistic-then-resize**
  strategy; a size-cache and a 2-RTT probe-first variant are left for later.

## Large values — region-bounded SGL, and why we do NOT stripe

A value up to **~64 MiB** rides a **single KV op** described by a **region-bounded
scatter-gather list**: one standard NVMe data-block descriptor per **2 MiB DMA
region** (no vendor extension). The shim hands `lib/nvme` one 2 MiB-bounded
segment at a time via the SGL iterator (`spdk_nvme_ctrlr_cmd_iov_raw_with_md`)
and **disables the PCIe SGL merge** so each segment becomes its own descriptor —
the vfio-user target maps each 2 MiB region independently, so a single descriptor
must **not** cross a region boundary. This is the same shape as the raw client's
`nvfu_sgl_set_dptr`. The descriptor count is bounded by the target's
`NVMF_REQ_MAX_BUFFERS` (`SPDK_NVMF_MAX_SGL_ENTRIES*2+1 = 33`), which caps a
single op at ~64 MiB (matching the vfio-user target's default `max_io_size`).

**No striping — a hard design decision.** A value that would need more than the
region budget is **rejected** (`NIXL_ERR_INVALID_PARAM`, before any DMA is
staged), never split across ops. This is sound because a **KV-cache block's byte
size is bounded and computable from model attributes**:

```
block_bytes = 2 (K,V) x n_layers x n_kv_heads x head_dim x dtype_bytes x tokens_per_block
```

For real models this lands in the low-MiB range (e.g. Llama-3-8B — GQA 8 KV
heads, head_dim 128, fp16, 16-token block ≈ 2 MiB), comfortably inside the
single-op SGL bound. So **one value = one op**: the caller sizes its blocks
(`tokens_per_block`) so a block fits one op, and a value that genuinely exceeds
the bound is out of scope for the KV-cache use case rather than something to
chunk. The single-op bound is `SPDK_KV_SHIM_MAX_VALUE_LEN` (64 MiB), further
clamped to the namespace-advertised `kvvml` when smaller.

Note: the region-crossing protection is **load-bearing only for hugepage-backed
DMA** (production), where each 2 MiB region is a separately-registered hugepage.
The `--no-huge` test harness backs DMA with a single region, so the byte-exact
large-value tests prove the SGL/iterator datapath end-to-end; the per-region
descriptor split is built by construction and exercised there, but its
*necessity* only manifests under hugepages.

## Custom backend parameters (`nixl_b_params_t`)

| Parameter      | Required | Default | Meaning |
| -------------- | -------- | ------- | ------- |
| `transport_id` | yes\*    | —       | SPDK transport ID, e.g. `trtype:VFIOUSER traddr:<socket-dir>`. |
| `vfu_addr`     | yes\*    | —       | Bare vfio-user socket dir; wrapped into a VFIOUSER `transport_id`. Aliases: `socket`, `vfio_user_path`. |
| `nsid`         | no       | `0`     | NVMe namespace id; `0`/unset auto-selects the first KV namespace. |
| `init_env`     | no       | `false` | `false`: the host/agent owns the SPDK env (multiple engines per process). `true`: the shim brings up its own no-hugepage SPDK env (single instance; for standalone tests). |

\* Provide **either** `transport_id` **or** `vfu_addr`.

## Build

The plugin is gated on the prebuilt SPDK NVMe library and the KV header; it is
skipped automatically if they are absent. It ships its own shim, so
`-Dspdk_kv_shim_dir` is **not** used by this plugin.

```bash
meson setup builddir \
    -Denable_plugins=SPDK \
    -Dspdk_root=/path/to/spdk \
    -Dspdk_kv_build_test=true
ninja -C builddir src/plugins/spdk_kv/libplugin_SPDK.so \
                  src/plugins/spdk_kv/spdk_kv_roundtrip_test
```

| Option              | Default | Description |
| ------------------- | ------- | ----------- |
| `spdk_root`         | `""`    | Path to the built SPDK tree to link against (unset — required). |
| `spdk_kv_build_test`| `false` | Build the direct-engine round-trip test binary. |

## Testing

`run_roundtrip.sh` stands up an SPDK `nvmf_tgt` with an **in-memory** KV
namespace (`kvdev_mem`, **no Ceph**) over VFIOUSER and runs the round-trip test:
Store a value under a 16-byte key, Retrieve it back, and verify byte-for-byte.
It covers a small value, the opaque key guards, **QUERY/Exist** (hit on a stored
key, miss on an absent key), **value auto-sizing** (a short-buffer READ reports
the true length, then a correctly-sized READ returns byte-exact), **large
values** at **1 MiB (1 region), 8 MiB (4 regions), and 60 MiB (30 regions)**, a
**large-value auto-sizing** case (a too-small READ of a multi-region value
reports the true length, then a resized re-Retrieve is byte-exact), and a **68
MiB oversize value that must be cleanly rejected, not split**. The script raises
the `kvdev_mem` `--max-value-len` so the target accepts the large stores (the
host-side ~64 MiB region-bounded-SGL bound remains the effective limit).

Note: the round-trip needs a **target-capable** KV SPDK build (one whose
`nvmf_tgt` has the `kvdev_mem` module + KV nvmf namespace RPCs). Point
`SPDK_ROOT` at that tree; the host-side SPDK the plugin links against may carry
only the NVMe-KV *host* driver.

```bash
SPDK_ROOT=/path/to/spdk ./run_roundtrip.sh
```

`run_block_roundtrip.sh` stands up an `nvmf_tgt` with an **NVM (block)**
namespace backed by a `malloc` bdev over VFIOUSER and runs the **block**
round-trip test (`spdk_kv_block_roundtrip_test`): write DRAM patterns to LBA
ranges (4 KiB…~60 MiB), read them back byte-exact, and confirm the misaligned,
over-single-op-bound, and out-of-capacity guards reject cleanly.

```bash
SPDK_ROOT=/path/to/spdk ./run_block_roundtrip.sh
```

`run_block_metadata_reject.sh` is the negative counterpart: it stands up a
**metadata-formatted** (interleaved / extended-LBA) `malloc` bdev as a block
namespace and asserts the engine **refuses it cleanly at init** (a distinct
`-ENOTSUP`), rather than faulting later at SGL build. The block datapath sizes
transfers from the **data-only** sector size, so a namespace carrying per-LBA
metadata (whose payload lib/nvme sizes from the larger *extended* sector size) is
rejected at open. If the target SPDK build cannot create a metadata malloc bdev,
the script reports `SKIP` (the guard is not exercised there).

```bash
SPDK_ROOT=/path/to/spdk ./run_block_metadata_reject.sh
```

### Device-mode (`PCIE`) testing

The block datapath is transport-agnostic, so the **same** block round-trip runs
against a **real local NVMe controller** over the `PCIE` transport — selected
purely by the transport-ID string (`trtype:PCIE traddr:<BDF>`), **no code fork**.
`run_block_pcie.sh` is the device-mode harness:

```bash
PCI_BDF=0000:xx:00.0 BIND=1 ./run_block_pcie.sh
```

> **DESTRUCTIVE — scratch device only.** This **WRITES LBAs (including LBA 0)**
> on the device at `PCI_BDF`, overwriting any partition table / filesystem /
> data. Point `PCI_BDF` at a **dedicated scratch** NVMe device or namespace
> **only**; never a device holding data.

The runner **fails safe** — before any bind or write it validates `PCI_BDF` and
**refuses** (non-zero exit, nothing touched) unless the target is a safe scratch
NVMe:

- the PCI device must **exist** and be an **NVMe controller** (class `0x0108xx`);
- any of its kernel block namespaces (or partitions) that is **mounted** or holds
  the **root filesystem** is refused **unconditionally** (`FORCE` cannot override);
- a namespace carrying a recognized **filesystem/partition signature** is refused
  **unless `FORCE=1`** — a truly blank scratch device passes without `FORCE`; a
  device you *intend* to overwrite needs `FORCE=1`. (This also covers a whole-disk
  filesystem with no partition table.)

It also requires an explicit `PCI_BDF` (never guesses), makes you type the BDF
back to confirm (skip with `ASSUME_YES=1` for non-interactive runs), and after a
`BIND=1` bind **asserts** the target actually landed on `vfio-pci` before writing.

To deliberately overwrite a device that has an existing signature:

```bash
PCI_BDF=0000:xx:00.0 BIND=1 FORCE=1 ./run_block_pcie.sh
```

Host requirements:

- **IOMMU enabled** (`intel_iommu=on`, or `amd_iommu=on iommu=pt`). The shim's
  SPDK env uses `no_huge` (DPDK IOVA=VA), so a `vfio-pci`-bound controller can
  only DMA through an IOMMU. Check `/sys/kernel/iommu_groups/` is non-empty.
- The scratch controller **bound to `vfio-pci`**. `BIND=1` binds it for you,
  scoped to just that BDF via `PCI_ALLOWED` (SPDK `scripts/setup.sh`), so no
  other NVMe controller — e.g. your boot drive — is touched. The vars are passed
  through `sudo env` so a hardened sudoers (`env_reset`) cannot strip
  `PCI_ALLOWED` and turn the scoped bind into a bind-everything. Equivalent
  manual bind:
  ```bash
  sudo env PCI_ALLOWED="$PCI_BDF" HUGEMEM=64 /path/to/spdk/scripts/setup.sh
  # hand it back to the kernel afterwards:
  sudo env PCI_ALLOWED="$PCI_BDF" /path/to/spdk/scripts/setup.sh reset
  ```
