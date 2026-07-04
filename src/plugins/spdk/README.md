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

# NIXL SPDK NVMe Plugin (`SPDK`)

A NIXL backend that moves data between host DRAM and NVMe storage using SPDK's
polled-mode NVMe driver (`lib/nvme`). One plugin serves two storage models,
selected per engine:

- **Block (LBA)** — ordinary NVMe read/write against a block namespace.
- **Key-Value** — the ratified NVMe Key-Value command set (Store / Retrieve /
  Exist) against a KV namespace.

The plugin is **transport-agnostic**: the same datapath runs against a real local
NVMe device (`trtype:PCIE`), a stock SPDK NVMe-oF target over vfio-user
(`trtype:VFIOUSER`), or a DPU-presented function — chosen purely by a config
string, with no code change. The backend registers with NIXL under the name
`SPDK`.

## Choosing a mode

An engine binds **one** namespace kind, selected at construction by the `csi`
parameter (aliases `ns_kind`, `mode`):

| `csi` value | Mode | NIXL segments (local ↔ remote) |
| --- | --- | --- |
| `kv` *(default)* | Key-Value | `DRAM_SEG` ↔ `OBJ_SEG` |
| `block` *(aliases `blk`, `nvm`)* | Block / LBA | `DRAM_SEG` ↔ `BLK_SEG` |

`getSupportedMems()` reflects the bound mode — a KV engine advertises
`{DRAM_SEG, OBJ_SEG}`, a block engine `{DRAM_SEG, BLK_SEG}` — and posting the
wrong remote segment type to an engine is rejected with `NIXL_ERR_NOT_SUPPORTED`.

## Block mode (`csi=block`)

| NIXL op | Local | Remote | Effect |
| --- | --- | --- | --- |
| `NIXL_WRITE` | `DRAM_SEG` | `BLK_SEG` | Write DRAM → device LBAs |
| `NIXL_READ`  | `DRAM_SEG` | `BLK_SEG` | Read device LBAs → DRAM |

The remote `BLK_SEG` descriptor's **address is the starting LBA**, and the
transfer length (bytes) must be a **multiple of the namespace sector size**. A
range that is mis-sized, past the single-op limit (see [Transfer
size](#transfer-size)), or beyond namespace capacity is rejected
(`NIXL_ERR_INVALID_PARAM`) before any DMA. `queryMem` is not defined for block.
Namespaces formatted with per-LBA metadata (interleaved / extended-LBA / DIF/DIX)
are refused when the engine opens.

## Key-Value mode (`csi=kv`)

| NIXL op | Local | Remote | Effect |
| --- | --- | --- | --- |
| `NIXL_WRITE` | `DRAM_SEG` | `OBJ_SEG` | KV **Store** |
| `NIXL_READ`  | `DRAM_SEG` | `OBJ_SEG` | KV **Retrieve** |
| `queryMem`   | — | `OBJ_SEG` | KV **Exist** (hit/miss, no data transfer) |

The remote `OBJ_SEG` descriptor's `metaInfo` blob **is the inline KV key**, taken
verbatim — 1 to 16 bytes, opaque (the plugin does not parse or interpret it). An
empty or over-16-byte key is rejected, never truncated. `queryMem` reports a hit
as an engaged `resp[i]` (empty params map) and a miss as `std::nullopt`; a
transport error is surfaced as an error, not masked as a miss.

**A short Retrieve buffer doesn't truncate.** If the stored value is larger than
the buffer you provide, Retrieve returns `NIXL_ERR_MISMATCH` and records the
value's true length — read it with `getReqTrueLen(handle, idx)`, then resize and
re-Retrieve.

## Transfer size

A single transfer — a KV value or a block range — moves in **one NVMe op** up to
**~64 MiB**, described by a region-bounded scatter-gather list (one descriptor
per 2 MiB DMA region, up to 33 regions). A transfer larger than that is
**rejected, never silently split** — size your transfers to fit one op. This
suits KV-cache blocks well: a block's byte size is fixed by the model's
attributes and lands in the low-MiB range. On a real PCIE controller the bound is
additionally clamped to the device's MDTS.

## Zero-copy vs staging

The datapath DMAs **directly to and from your buffer** — no bounce copy — when
that buffer is DMA-reachable by the transport. Register it with `registerMem()`
first. What counts as reachable depends on the transport:

- **vfio-user** targets map client memory by file descriptor, so only fd-backed
  memory is zero-copy: SPDK-DMA (`spdk_dma_malloc`), hugepages, memfd, or a
  dma-buf. Plain anonymous DRAM is not.
- **PCIE / IOMMU** controllers can reach any page-aligned, IOMMU-mappable DRAM.

A buffer that cannot be made reachable still works — the plugin transparently
**stages** it through an SPDK-DMA buffer and copies (correct, just not zero-copy).
`dramIsDmaRegistered()` reports which path a registration took.

## Parameters (`nixl_b_params_t`)

| Parameter | Required | Default | Meaning |
| --- | --- | --- | --- |
| `transport_id` | yes\* | — | SPDK transport ID, e.g. `trtype:VFIOUSER traddr:<socket-dir>` or `trtype:PCIE traddr:<BDF>`. |
| `vfu_addr` | yes\* | — | Bare vfio-user socket dir; wrapped into a VFIOUSER `transport_id`. Aliases: `socket`, `vfio_user_path`. |
| `csi` | no | `kv` | Namespace kind: `kv` or `block` (aliases `blk`, `nvm`). This key also accepts the names `ns_kind` and `mode`. |
| `nsid` | no | `0` | NVMe namespace id; `0`/unset auto-selects the first namespace of the chosen kind. |
| `init_env` | no | `false` | `false`: the host/agent owns the SPDK env (many engines per process). `true`: the engine brings up its own no-hugepage SPDK env (single instance; for standalone tests). |

\* Provide **either** `transport_id` **or** `vfu_addr`.

## Build

The plugin links against a prebuilt SPDK tree and is skipped automatically if the
SPDK NVMe library (and, for KV, the `spdk/nvme_kv.h` header) is absent.

```bash
meson setup builddir -Denable_plugins=SPDK -Dspdk_root=/path/to/spdk
ninja -C builddir src/plugins/spdk/libplugin_SPDK.so
```

| Option | Default | Description |
| --- | --- | --- |
| `spdk_root` | `""` | Path to the built SPDK tree to link against (required). |
| `spdk_build_test` | `false` | Also build the round-trip test binaries. |

## Testing

Each harness stands up a local SPDK `nvmf_tgt` over vfio-user and runs a
byte-exact round-trip against it. Point `SPDK_ROOT` at a **target-capable** SPDK
build (one whose `nvmf_tgt` has the namespace RPCs the harness needs); the
host-side SPDK the plugin links against may carry only the driver. Build the test
binaries with `-Dspdk_build_test=true`.

```bash
# Key-Value: store/retrieve under 16-byte keys, Exist hit/miss, value
# auto-sizing, and large values (1 / 8 / 60 MiB) with an oversize value rejected.
SPDK_ROOT=/path/to/spdk ./run_roundtrip.sh

# Block: DRAM <-> LBA read/write (4 KiB .. ~60 MiB), with misaligned,
# over-single-op, and out-of-capacity ranges rejected cleanly.
SPDK_ROOT=/path/to/spdk ./run_block_roundtrip.sh

# Block (negative): a metadata-formatted namespace is refused at engine open.
SPDK_ROOT=/path/to/spdk ./run_block_metadata_reject.sh
```

### Device mode — real NVMe over PCIE

Because the transport is just a config string, the **same** block round-trip runs
against a **real local NVMe controller** on `vfio-pci`, selected with
`trtype:PCIE traddr:<BDF>`. `run_block_pcie.sh` is the device-mode harness:

```bash
PCI_BDF=0000:xx:00.0 BIND=1 ./run_block_pcie.sh
```

> **DESTRUCTIVE — scratch device only.** This **writes LBAs (including LBA 0)** on
> the device at `PCI_BDF`, overwriting any partition table / filesystem / data.
> Point `PCI_BDF` at a **dedicated scratch** NVMe device or namespace **only**,
> never one holding data.

The runner **fails safe** — before any bind or write it validates `PCI_BDF` and
refuses (non-zero exit, nothing touched) unless the target is a safe scratch NVMe:

- the PCI device must exist and be an **NVMe controller** (class `0x0108xx`);
- any of its namespaces (or partitions) that is **mounted** or holds the **root
  filesystem** is refused unconditionally (`FORCE` cannot override);
- a namespace carrying a recognized **filesystem/partition signature** is refused
  unless `FORCE=1` — a blank scratch device passes without `FORCE`.

It requires an explicit `PCI_BDF` (never guesses), asks you to type the BDF back
to confirm (`ASSUME_YES=1` skips this for non-interactive runs), and after a
`BIND=1` bind asserts the target actually landed on `vfio-pci` before writing.

Host requirements:

- **IOMMU enabled** (`intel_iommu=on`, or `amd_iommu=on iommu=pt`). The SPDK env
  runs `no_huge` (DPDK IOVA=VA), so a `vfio-pci`-bound controller can only DMA
  through an IOMMU. Check `/sys/kernel/iommu_groups/` is non-empty.
- The scratch controller **bound to `vfio-pci`**. `BIND=1` does this for you,
  scoped to just that BDF via `PCI_ALLOWED` so no other NVMe controller (e.g. your
  boot drive) is touched. Equivalent manual bind:
  ```bash
  sudo env PCI_ALLOWED="$PCI_BDF" HUGEMEM=64 /path/to/spdk/scripts/setup.sh
  # hand it back to the kernel afterwards:
  sudo env PCI_ALLOWED="$PCI_BDF" /path/to/spdk/scripts/setup.sh reset
  ```

## Scope

This backend is intentionally generic: it carries **no** backend-specific
behavior, KV Exec, long-key path, or VRAM/P2PDMA. Direct-to-VRAM transfers
(P2PDMA into a GPU dma-buf) are a planned follow-up that extends the same
registration hook.
