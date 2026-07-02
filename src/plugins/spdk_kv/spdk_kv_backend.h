/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SPDK_KV_BACKEND_H
#define SPDK_KV_BACKEND_H

#include <cstdint>
#include <string>
#include <vector>

#include "backend/backend_engine.h"
#include "spdk_kv_key.h" // spdkKvKeyFromBlobId (SPDK-free key mapping)

// Forward declaration of the opaque SPDK KV shim handle (C ABI).
struct spdk_kv_shim;

/**
 * nixlSpdkKvEngine: a clean-sheet, transport-agnostic, backend-agnostic NIXL
 * backend that speaks the ratified NVMe Key-Value command set over the SPDK
 * NVMe driver (lib/nvme). It works against ANY SPDK NVMe-KV target (an
 * in-memory kvdev, a librados-backed kvdev, a DPU-presented VF, ...); it
 * carries NO backend-specific behavior, NO KV Exec, and NO long-key path.
 *
 * SCOPE -- Exist + value auto-sizing + large values:
 *   - Store (NIXL_WRITE) and Retrieve (NIXL_READ) of SMALL and LARGE values in
 *     host DRAM. Large values (up to ~64 MiB) ride a single op via a
 *     region-bounded SGL (one data-block descriptor per 2 MiB region, bounded by
 *     the target's NVMF_REQ_MAX_BUFFERS = 33). NO striping: a value past the
 *     single-op bound is rejected (NIXL_ERR_INVALID_PARAM), never split.
 *   - Exist (NIXL queryMem / QUERY -> KV Exist): cache hit/miss, NO data xfer.
 *   - Value auto-sizing on Retrieve: a short host buffer surfaces the device's
 *     TRUE value length (completion cdw0, via getReqTrueLen) and reports
 *     NIXL_ERR_MISMATCH so the caller can resize and re-READ, instead of
 *     silently truncating.
 *   - 16-byte inline keys taken verbatim (opaque); no lineage parsing.
 *   - DRAM only. VRAM / P2PDMA is deferred.
 *   - NO Delete/List (deferred), NO KV Exec (out of scope for this generic
 *     plugin by design).
 *
 * Memory-type mapping (storage-backend shape, mirrors OBJ):
 *   - local  source/destination : DRAM_SEG (host DRAM)
 *   - remote key-addressed blob  : OBJ_SEG (the NVMe-KV key space)
 *   - remote LBA range           : BLK_SEG (an NVMe NVM/block namespace)
 *
 * One plugin, an LBA/KV knob: the op-set is chosen by the REMOTE memory type
 * per transfer. OBJ_SEG drives the KV path above; BLK_SEG drives an NVMe block
 * (LBA read/write) path against a CSI==NVM namespace. The block namespace is
 * bound at init by the "csi=block" (a.k.a. ns_kind/mode=block) param -- ratified
 * option (b), one namespace kind per engine; an agent that needs both KV and
 * block opens two engines. For BLK_SEG the remote descriptor's addr is the
 * starting LBA and devId is the namespace; there is NO key derivation and NO
 * value auto-sizing (a block read/write moves exactly len bytes). A range up to
 * ~64 MiB rides a single op via the SAME region-bounded SGL as the KV large-value
 * path (one data-block descriptor per 2 MiB DMA region, bounded by the 33-region
 * budget); a range past the single-op bound is REJECTED (NIXL_ERR_INVALID_PARAM),
 * never striped.
 *
 * Operation mapping:
 *   - NIXL_WRITE (local DRAM -> remote OBJ) becomes a KV Store
 *   - NIXL_READ  (remote OBJ -> local DRAM) becomes a KV Retrieve
 *   - NIXL_WRITE (local DRAM -> remote BLK) becomes an NVMe block write
 *   - NIXL_READ  (remote BLK -> local DRAM) becomes an NVMe block read
 *
 * The remote OBJ_SEG descriptor carries the NIXL block identifier in its
 * metaInfo blob. The engine takes those bytes VERBATIM as the NVMe-KV key
 * (spdkKvKeyFromBlobId) -- opaque, 1..16 bytes, no hash, no truncation -- and
 * stores the key in the per-descriptor metadata so transfers read it back from
 * the descriptor. An empty or over-16-byte key is rejected
 * (NIXL_ERR_INVALID_PARAM).
 *
 * Transport: the "transport_id" custom param is a standard SPDK transport ID
 * string (e.g. "trtype:VFIOUSER traddr:<socket-dir>"), which is what makes the
 * datapath transport-agnostic. For ergonomics a bare socket dir may be passed
 * as "vfu_addr"/"socket" and is wrapped into a VFIOUSER transport ID.
 *
 * Staging / zero-copy approach (DOCUMENTED):
 *   SPDK Store/Retrieve require the value buffer to be SPDK-DMA memory. User
 *   DRAM registered through registerMem() is ordinary host memory and is
 *   generally NOT DMA-capable, so this skeleton STAGES through a per-request
 *   shim DMA buffer and copies:
 *     - WRITE: memcpy(user DRAM -> DMA buf) then Store(key, DMA buf)
 *     - READ : Retrieve(key, DMA buf) then memcpy(DMA buf -> user DRAM)
 *   The staging DMA buffer is described to the device by a region-bounded SGL
 *   (see spdk_kv_shim), so this holds for large values too. A future revision
 *   (VRAM/P2PDMA) registers the user/GPU buffer directly and skips the copy.
 */
class nixlSpdkKvEngine : public nixlBackendEngine {
public:
    explicit nixlSpdkKvEngine(const nixlBackendInitParams *init_params);
    ~nixlSpdkKvEngine() override;

    bool
    supportsRemote() const override {
        return false;
    }

    bool
    supportsLocal() const override {
        return true;
    }

    bool
    supportsNotif() const override {
        return false;
    }

    nixl_mem_list_t
    getSupportedMems() const override;

    nixl_status_t
    registerMem(const nixlBlobDesc &mem, const nixl_mem_t &nixl_mem, nixlBackendMD *&out) override;

    nixl_status_t
    deregisterMem(nixlBackendMD *meta) override;

    nixl_status_t
    connect(const std::string &remote_agent) override {
        return NIXL_SUCCESS;
    }

    nixl_status_t
    disconnect(const std::string &remote_agent) override {
        return NIXL_SUCCESS;
    }

    nixl_status_t
    unloadMD(nixlBackendMD *input) override {
        return NIXL_SUCCESS;
    }

    nixl_status_t
    loadLocalMD(nixlBackendMD *input, nixlBackendMD *&output) override {
        output = input;
        return NIXL_SUCCESS;
    }

    nixl_status_t
    prepXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    postXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    checkXfer(nixlBackendReqH *handle) const override;

    nixl_status_t
    releaseReqH(nixlBackendReqH *handle) const override;

    // QUERY -> NVMe-KV Exist. Mirrors the OBJ backend's queryMem convention:
    //   present => resp[i] engaged (empty params); absent => std::nullopt.
    // A transport/backend error returns an error status (never masked as a
    // miss), so a real failure is not mistaken for a cache miss. Transfers no
    // value data.
    nixl_status_t
    queryMem(const nixl_reg_dlist_t &descs,
             std::vector<nixl_query_resp_t> &resp) const override;

    // Value auto-sizing helper. After a postXfer READ that reported
    // NIXL_ERR_MISMATCH because the host buffer was too small, this returns the
    // device's TRUE value length (completion cdw0) recorded for descriptor
    // \c idx, so the caller can resize its buffer/descriptor and re-Retrieve.
    // Returns 0 when the handle recorded no true length for \c idx (e.g. the
    // READ fit, or \c idx is out of range).
    size_t
    getReqTrueLen(nixlBackendReqH *handle, int idx = 0) const;

private:
    // Ratified maximum NVMe-KV inline key length (bytes).
    static constexpr uint8_t kMaxKeyLen = 16;

    // Block (BLK_SEG) helpers. Dispatched from prep/postXfer when the remote
    // memory type is BLK_SEG; the OBJ_SEG (KV) path is unchanged.
    //
    // computeBlockRange: derive the LBA (remote.addr) and sector count for one
    // descriptor pair, enforcing the block semantics -- local/remote byte
    // lengths must match, the length must be a multiple of the namespace sector
    // size, the length must not exceed the region-bounded single-op bound
    // (~64 MiB; NO striping), and [LBA, LBA+nlba) must fit the namespace
    // capacity. A zero-length descriptor yields nlba_out == 0 (a no-op the
    // caller skips). Returns NIXL_ERR_INVALID_PARAM on any violation.
    nixl_status_t
    computeBlockRange(const nixlMetaDesc &local_desc,
                      const nixlMetaDesc &remote_desc,
                      uint64_t &lba_out,
                      uint32_t &nlba_out) const;

    // postXferBlock: stage through a region-aligned SPDK DMA buffer and issue
    // spdk_kv_shim_write (WRITE) / spdk_kv_shim_read (READ) per descriptor.
    nixl_status_t
    postXferBlock(const nixl_xfer_op_t &operation,
                  const nixl_meta_dlist_t &local,
                  const nixl_meta_dlist_t &remote,
                  nixlBackendReqH *handle) const;

    // The SPDK KV shim handle (owns the controller attach + qpair).
    spdk_kv_shim *shim_ = nullptr;

    // Effective key length: min(kMaxKeyLen, kvkml advertised by the namespace).
    uint8_t maxKeyLen_ = kMaxKeyLen;

    // True when the shim bound a CSI==NVM block namespace (csi=block init param).
    bool blockMode_ = false;
};

#endif // SPDK_KV_BACKEND_H
